#include "vision/camera.hpp"

#include "core/logger.hpp"

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdlib>
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
			const std::string lower = [&] {
				std::string s = source;
				std::transform(
				    s.begin(), s.end(), s.begin(),
				    [](unsigned char ch) {
					    return static_cast<char>(std::tolower(ch));
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

	Camera::Camera(CameraConfig camera_config,
	               StreamConfig stream_config, int retry_interval_ms):
	config_(std::move(camera_config)),
	stream_config_(std::move(stream_config)),
	retry_interval_ms_(retry_interval_ms)
	{
		file_source_ = !isInteger(config_.source)
		    && !isDevicePath(config_.source)
		    && isFileExtension(config_.source);
	}

	bool Camera::loopVideo() const noexcept
	{
		return config_.loop_video;
	}

	bool Camera::streamingActive() const noexcept
	{
		return streaming_active_;
	}

	std::string Camera::buildMjpegStreamPipeline(
	    const std::string& resolved_device) const
	{
		std::ostringstream pipeline;

		pipeline << "v4l2src " << "device=" << resolved_device << " "
		         << "io-mode=mmap " << "do-timestamp=true "

		         << "! image/jpeg," << "width=" << config_.width << ","
		         << "height=" << config_.height << ","
		         << "framerate=" << config_.fps << "/1 "

		         << "! jpegparse "
		         << "! tee name=t "

		         // 图传分支：直接发送摄像头原始 MJPEG
		         << "t. ! queue " << "leaky=downstream "
		         << "max-size-buffers=1 " << "max-size-bytes=0 "
		         << "max-size-time=0 " << "! rtpjpegpay "
		         << "pt=" << stream_config_.payload_type << " "
		         << "mtu=" << stream_config_.mtu << " " << "! udpsink "
		         << "host=" << stream_config_.host << " "
		         << "port=" << stream_config_.port << " "
		         << "sync=false "
		         << "async=false "

		         // 视觉分支：只在这里解码一次
		         << "t. ! queue " << "leaky=downstream "
		         << "max-size-buffers=1 " << "max-size-bytes=0 "
		         << "max-size-time=0 " << "! jpegdec "
		         << "! videoconvert " << "! video/x-raw,"
		         << "format=BGR," << "width=" << config_.width << ","
		         << "height=" << config_.height << ","
		         << "framerate=" << config_.fps << "/1 " << "! appsink "
		         << "drop=true " << "max-buffers=1 " << "sync=false";

		return pipeline.str();
	}

	bool Camera::openGstreamerPipeline(
	    const std::string& resolved_device) noexcept
	{
		const std::string pipeline =
		    buildMjpegStreamPipeline(resolved_device);

		ETEST_LOG_INFO(
		    "CAMERA",
		    "opening GStreamer MJPEG tee: device=" + resolved_device
		        + ", receiver=" + stream_config_.host + ":"
		        + std::to_string(stream_config_.port));

		const bool success = cap_.open(pipeline, cv::CAP_GSTREAMER);

		using_gstreamer_pipeline_ = success;
		streaming_active_ = success;

		if(!success)
		{
			ETEST_LOG_ERROR(
			    "CAMERA",
			    "failed to open GStreamer streaming pipeline");
		}

		return success;
	}

	bool Camera::openV4l2(const std::string& resolved_device) noexcept
	{
		bool success = cap_.open(resolved_device, cv::CAP_V4L2);

		if(!success)
		{
			ETEST_LOG_WARN(
			    "CAMERA", "V4L2 backend failed; retrying with CAP_ANY");

			success = cap_.open(resolved_device, cv::CAP_ANY);
		}

		using_gstreamer_pipeline_ = false;
		streaming_active_ = false;

		return success;
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
				std::string resolved_path = config_.source;
				char real_path[PATH_MAX] = {};
				if(::realpath(config_.source.c_str(), real_path)
				   != nullptr)
				{
					resolved_path = real_path;
				}

				if(stream_config_.enabled)
				{
					success = openGstreamerPipeline(resolved_path);

					if(!success && stream_config_.allow_fallback)
					{
						ETEST_LOG_WARN(
						    "CAMERA",
						    "streaming unavailable; falling back "
						    "to V4L2");

						cap_.release();
						success = openV4l2(resolved_path);
					}
				}
				else
				{
					success = openV4l2(resolved_path);
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

			// 使用自定义 GStreamer 管线时，分辨率/FOURCC/FPS
			// 已由 caps 固定，跳过 cap_.set() 调用。
			if((numeric_source || device_path)
			   && !using_gstreamer_pipeline_)
			{
				if(config_.fourcc.size() == 4)
				{
					if(!cap_.set(
					       cv::CAP_PROP_FOURCC,
					       cv::VideoWriter::fourcc(
					           config_.fourcc[0], config_.fourcc[1],
					           config_.fourcc[2], config_.fourcc[3])))
					{
						ETEST_LOG_WARN(
						    "CAMERA",
						    "driver rejected requested FOURCC "
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

				if(!cap_.set(cv::CAP_PROP_BUFFERSIZE, 1))
				{
					ETEST_LOG_WARN("CAMERA",
					               "backend rejected "
					               "CAP_PROP_BUFFERSIZE=1");
				}
			}

			std::ostringstream description;

			description << "opened source=" << config_.source
			            << ", actual_width="
			            << cap_.get(cv::CAP_PROP_FRAME_WIDTH)
			            << ", actual_height="
			            << cap_.get(cv::CAP_PROP_FRAME_HEIGHT)
			            << ", actual_fps=" << cap_.get(cv::CAP_PROP_FPS)
			            << ", is_file="
			            << (file_source_ ? "true" : "false");

			if(using_gstreamer_pipeline_)
			{
				description << ", gstreamer_tee=active";
			}

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

				frame.release();

				if(!read_error_reported_)
				{
					ETEST_LOG_ERROR("CAMERA",
					                "failed to read a valid frame");

					read_error_reported_ = true;
				}

				if(file_source_
				   && consecutive_failures_ >= kMaxConsecutiveFailures)
				{
					if(config_.loop_video)
					{
						ETEST_LOG_INFO(
						    "CAMERA",
						    "file source ended; looping back "
						    "to start");

						cap_.set(cv::CAP_PROP_POS_FRAMES, 0);
						consecutive_failures_ = 0;
						state_ = CameraState::OK;
						return read(frame);
					}

					ETEST_LOG_INFO("CAMERA",
					               "file source reached end; not "
					               "reopening");

					state_ = CameraState::FILE_EOF;
				}

				return false;
			}

			consecutive_failures_ = 0;

			if(read_error_reported_)
			{
				ETEST_LOG_INFO("CAMERA", "frame reading recovered");

				read_error_reported_ = false;
			}

			// GStreamer 管线正常情况下已经输出目标尺寸；
			// 此处只作为后端协商异常时的保险。
			const int target_w = config_.width;
			const int target_h = config_.height;
			if(target_w > 0 && target_h > 0
			   && (frame.cols != target_w || frame.rows != target_h))
			{
				cv::resize(frame, frame, cv::Size(target_w, target_h),
				           0.0, 0.0, cv::INTER_LINEAR);
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

	bool Camera::realtimePlayback() const noexcept
	{
		return config_.realtime_playback;
	}

	int Camera::playbackFps() const noexcept
	{
		return config_.playback_fps;
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

			using_gstreamer_pipeline_ = false;
			streaming_active_ = false;
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