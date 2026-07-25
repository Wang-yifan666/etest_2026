#include "vision/camera.hpp"

#include "core/logger.hpp"

#include <algorithm>
#include <cctype>
#include <exception>
#include <sstream>
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

	} // namespace

	Camera::Camera(CameraConfig config): config_(std::move(config)) {}

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
			            << cap_.get(cv::CAP_PROP_FPS);

			ETEST_LOG_INFO("CAMERA", description.str());

			consecutive_failures_ = 0;
			read_error_reported_ = false;
			return true;
		}
		catch(const cv::Exception& error)
		{
			ETEST_LOG_ERROR(
			    "CAMERA",
			    std::string("OpenCV open exception: ") + error.what());

			return false;
		}
		catch(const std::exception& error)
		{
			ETEST_LOG_ERROR(
			    "CAMERA",
			    std::string("open exception: ") + error.what());

			return false;
		}
		catch(...)
		{
			ETEST_LOG_ERROR("CAMERA", "unknown open exception");

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

				return false;
			}

			const bool success = cap_.read(frame) && !frame.empty();

			if(!success)
			{
				++consecutive_failures_;

				if(!read_error_reported_)
				{
					ETEST_LOG_ERROR("CAMERA",
					                "failed to read a valid frame");

					read_error_reported_ = true;
				}

				if(consecutive_failures_ >= kMaxConsecutiveFailures)
				{
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

			return false;
		}
		catch(...)
		{
			if(!read_error_reported_)
			{
				ETEST_LOG_ERROR("CAMERA", "unknown read exception");

				read_error_reported_ = true;
			}

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

	void Camera::release() noexcept
	{
		try
		{
			if(cap_.isOpened())
			{
				cap_.release();

				ETEST_LOG_INFO("CAMERA", "released");
			}
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
