#include "vision/camera.hpp"

#include "core/logger.hpp"

#include <algorithm>
#include <cctype>
#include <exception>
#include <sstream>
#include <thread>
#include <utility>

namespace etest::vision
{

namespace
{

bool isInteger(const std::string& text)
{
	if(text.empty())
	{
		return false;
	}

	return std::all_of(text.begin(), text.end(),
	                   [](unsigned char ch) {
		                   return std::isdigit(ch) != 0;
	                   });
}

bool isDevicePath(const std::string& source)
{
	return source.rfind("/dev/", 0) == 0;
}

bool isFileExtension(const std::string& source)
{
	// 常见视频文件扩展名
	const std::string lower = [&] {
		std::string s = source;
		std::transform(s.begin(), s.end(), s.begin(),
		               [](unsigned char ch) {
			               return static_cast<char>(
			                   std::tolower(ch));
		               });
		return s;
	}();

	return lower.find(".mp4") != std::string::npos
	       || lower.find(".avi") != std::string::npos
	       || lower.find(".mov") != std::string::npos
	       || lower.find(".mkv") != std::string::npos
	       || lower.find(".jpg") != std::string::npos
	       || lower.find(".png") != std::string::npos
	       || lower.find(".bmp") != std::string::npos;
}

} // namespace

Camera::Camera(CameraConfig config, int retry_interval_ms)
    : config_(std::move(config))
    , retry_interval_ms_(retry_interval_ms)
{
	// 判断是否是文件源
	file_source_ =
	    !isInteger(config_.source) && !isDevicePath(config_.source)
	    && isFileExtension(config_.source);
}

bool Camera::loopVideo() const noexcept
{
	return config_.loop_video;
}

bool Camera::open() noexcept
{
	try
	{
		if(cap_.isOpened())
		{
			cap_.release();
		}

		bool success = false;
		const bool numeric_source = isInteger(config_.source);

		const bool device_path = isDevicePath(config_.source);

		if(numeric_source)
		{
			const int camera_id = std::stoi(config_.source);

			success = cap_.open(camera_id, cv::CAP_ANY);
		}
		else if(device_path)
		{
			// Linux / WSL / Raspberry Pi 上优先使用 V4L2。
			success = cap_.open(config_.source, cv::CAP_V4L2);

			if(!success)
			{
				ETEST_LOG_WARN(
				    "CAMERA",
				    "V4L2 backend failed; retrying with CAP_ANY");

				success = cap_.open(config_.source, cv::CAP_ANY);
			}
		}
		else
		{
			success = cap_.open(config_.source, cv::CAP_ANY);
		}

		if(!success)
		{
			ETEST_LOG_ERROR(
			    "CAMERA",
			    "failed to open source: " + config_.source);

			state_ = CameraState::ERROR;
			return false;
		}

		if(numeric_source || device_path)
		{
			if(config_.fourcc.size() == 4)
			{
				if(!cap_.set(
				       cv::CAP_PROP_FOURCC,
				       cv::VideoWriter::fourcc(
				           config_.fourcc[0], config_.fourcc[1],
				           config_.fourcc[2], config_.fourcc[3])))
				{
					ETEST_LOG_WARN("CAMERA",
					               "driver rejected requested "
					               "FOURCC "
					                   + config_.fourcc);
				}
			}

			if(!cap_.set(cv::CAP_PROP_FRAME_WIDTH, config_.width))
			{
				ETEST_LOG_WARN("CAMERA",
				               "driver rejected requested width "
				                   + std::to_string(config_.width));
			}

			if(!cap_.set(cv::CAP_PROP_FRAME_HEIGHT, config_.height))
			{
				ETEST_LOG_WARN(
				    "CAMERA",
				    "driver rejected requested height "
				        + std::to_string(config_.height));
			}

			if(!cap_.set(cv::CAP_PROP_FPS, config_.fps))
			{
				ETEST_LOG_WARN("CAMERA",
				               "driver rejected requested FPS "
				                   + std::to_string(config_.fps));
			}
		}

		std::ostringstream description;

		description << "opened source=" << config_.source
		            << ", actual_width="
		            << cap_.get(cv::CAP_PROP_FRAME_WIDTH)
		            << ", actual_height="
		            << cap_.get(cv::CAP_PROP_FRAME_HEIGHT)
		            << ", actual_fps="
		            << cap_.get(cv::CAP_PROP_FPS)
		            << ", is_file=" << (file_source_ ? "true" : "false");

		ETEST_LOG_INFO("CAMERA", description.str());

		consecutive_failures_ = 0;
		read_error_reported_ = false;
		state_ = CameraState::OK;
		return true;
	}
	catch(const cv::Exception& error)
	{
		ETEST_LOG_ERROR(
		    "CAMERA",
		    std::string("OpenCV open exception: ") + error.what());

		state_ = CameraState::ERROR;
		return false;
	}
	catch(const std::exception& error)
	{
		ETEST_LOG_ERROR(
		    "CAMERA",
		    std::string("open exception: ") + error.what());

		state_ = CameraState::ERROR;
		return false;
	}
	catch(...)
	{
		ETEST_LOG_ERROR("CAMERA", "unknown open exception");

		state_ = CameraState::ERROR;
		return false;
	}
}

bool Camera::read(cv::Mat& frame) noexcept
{
	try
	{
		if(!cap_.isOpened())
		{
			if(!read_error_reported_)
			{
				ETEST_LOG_ERROR(
				    "CAMERA",
				    "read requested while camera is not open");

				read_error_reported_ = true;
			}

			state_ = CameraState::ERROR;
			frame.release();
			return false;
		}

		const bool success = cap_.read(frame) && !frame.empty();

		if(!success)
		{
			++consecutive_failures_;

			// 清除输出帧，禁止使用上一帧
			frame.release();

			if(!read_error_reported_)
			{
				ETEST_LOG_ERROR("CAMERA",
				                "failed to read a valid frame");

				read_error_reported_ = true;
			}

			if(consecutive_failures_ >= kMaxConsecutiveFailures)
			{
				// 文件源：到达 EOF
				if(file_source_)
				{
					if(config_.loop_video)
					{
						ETEST_LOG_INFO(
						    "CAMERA",
						    "file source ended; looping back to start");

						cap_.set(cv::CAP_PROP_POS_FRAMES, 0);
						consecutive_failures_ = 0;
						state_ = CameraState::OK;
						// 下一帧尝试读取
						return read(frame);
					}

					ETEST_LOG_INFO(
					    "CAMERA",
					    "file source reached end; "
					    "not reopening");

					state_ = CameraState::FILE_EOF;
					return false;
				}

				ETEST_LOG_WARN(
				    "CAMERA",
				    "too many consecutive read failures; "
				    "reopening camera");

				cap_.release();

				if(open())
				{
					ETEST_LOG_INFO("CAMERA",
					               "camera reopened successfully");
				}
				else
				{
					ETEST_LOG_ERROR("CAMERA",
					                "camera reopen failed");

					state_ = CameraState::DISCONNECTED;

					// 重连失败时休眠，避免高频重试
					if(retry_interval_ms_ > 0)
					{
						std::this_thread::sleep_for(
						    std::chrono::milliseconds(
						        retry_interval_ms_));
					}
				}
			}

			return false;
		}

		consecutive_failures_ = 0;

		if(read_error_reported_)
		{
			ETEST_LOG_INFO("CAMERA", "frame reading recovered");

			read_error_reported_ = false;
		}

		state_ = CameraState::OK;
		return true;
	}
	catch(const cv::Exception& error)
	{
		if(!read_error_reported_)
		{
			ETEST_LOG_ERROR("CAMERA",
			                std::string("OpenCV read exception: ")
			                    + error.what());

			read_error_reported_ = true;
		}

		frame.release();
		state_ = CameraState::ERROR;
		return false;
	}
	catch(const std::exception& error)
	{
		if(!read_error_reported_)
		{
			ETEST_LOG_ERROR(
			    "CAMERA",
			    std::string("read exception: ") + error.what());

			read_error_reported_ = true;
		}

		frame.release();
		state_ = CameraState::ERROR;
		return false;
	}
	catch(...)
	{
		if(!read_error_reported_)
		{
			ETEST_LOG_ERROR("CAMERA", "unknown read exception");

			read_error_reported_ = true;
		}

		frame.release();
		state_ = CameraState::ERROR;
		return false;
	}
}

bool Camera::isOpened() const noexcept
{
	try
	{
		return cap_.isOpened();
	}
	catch(...)
	{
		return false;
	}
}

CameraState Camera::getState() const noexcept
{
	return state_;
}

int Camera::consecutiveFailures() const noexcept
{
	return consecutive_failures_;
}

bool Camera::isFileSource() const noexcept
{
	return file_source_;
}

void Camera::release() noexcept
{
	try
	{
		if(cap_.isOpened())
		{
			cap_.release();

			ETEST_LOG_INFO("CAMERA", "released");
		}

		state_ = CameraState::DISCONNECTED;
	}
	catch(const cv::Exception& error)
	{
		ETEST_LOG_ERROR(
		    "CAMERA",
		    std::string("release exception: ") + error.what());
	}
	catch(...)
	{
		ETEST_LOG_ERROR("CAMERA", "unknown release exception");
	}
}

} // namespace etest::vision