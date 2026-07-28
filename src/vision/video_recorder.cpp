#include "vision/video_recorder.hpp"

#include "core/logger.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>

namespace etest::vision
{

VideoRecorder::VideoRecorder(RecordConfig config)
    : config_(std::move(config))
{
}

bool VideoRecorder::ensureOpened(cv::VideoWriter& writer,
                                  const cv::Mat& frame,
                                  const std::string& type) noexcept
{
	if(writer.isOpened())
	{
		return true;
	}

	if(recording_disabled_due_to_error_)
	{
		return false;
	}

	if(frame.empty())
	{
		return false;
	}

	const int fourcc_code = cv::VideoWriter::fourcc(
	    config_.fourcc[0], config_.fourcc[1],
	    config_.fourcc[2], config_.fourcc[3]);

	const std::string path = makeFilePath(type);

	try
	{
		if(!writer.open(path, fourcc_code,
		                static_cast<double>(config_.fps),
		                cv::Size(frame.cols, frame.rows)))
		{
			ETEST_LOG_ERROR("VIDEO_RECORDER",
			                "failed to create video file: " + path);
			recording_disabled_due_to_error_ = true;
			return false;
		}
	}
	catch(const cv::Exception& error)
	{
		ETEST_LOG_ERROR("VIDEO_RECORDER",
		                std::string("OpenCV exception creating video: ")
		                    + error.what());
		recording_disabled_due_to_error_ = true;
		return false;
	}
	catch(...)
	{
		ETEST_LOG_ERROR("VIDEO_RECORDER",
		                "unknown exception creating video");
		recording_disabled_due_to_error_ = true;
		return false;
	}

	segment_start_ = std::chrono::steady_clock::now();

	ETEST_LOG_INFO("VIDEO_RECORDER",
	               "created " + type + " video: " + path);

	return true;
}

bool VideoRecorder::shouldRotate() const noexcept
{
	if(config_.segment_seconds <= 0)
	{
		return false;
	}

	const auto now = std::chrono::steady_clock::now();
	const auto elapsed =
	    std::chrono::duration_cast<std::chrono::seconds>(
	        now - segment_start_)
	        .count();

	return elapsed >= config_.segment_seconds;
}

std::string VideoRecorder::makeFilePath(
    const std::string& type) const
{
	const auto now = std::chrono::system_clock::now();
	const auto time_t_now = std::chrono::system_clock::to_time_t(now);
	const auto* tm_now = std::localtime(&time_t_now);

	std::ostringstream filename;
	filename << config_.directory << "/" << type << "_";

	if(tm_now != nullptr)
	{
		filename << std::put_time(tm_now, "%Y%m%d_%H%M%S");
	}
	else
	{
		filename << "unknown";
	}

	filename << ".avi";

	// 确保目录存在
	std::string dir = config_.directory;
	if(!dir.empty())
	{
		::mkdir(dir.c_str(), 0755);
	}

	return filename.str();
}

bool VideoRecorder::writeRaw(const cv::Mat& frame) noexcept
{
	if(!config_.enabled || !config_.save_raw)
	{
		return true;
	}

	if(recording_disabled_due_to_error_)
	{
		return false;
	}

	if(shouldRotate())
	{
		if(raw_writer_.isOpened())
		{
			raw_writer_.release();
		}
	}

	if(!ensureOpened(raw_writer_, frame, "raw"))
	{
		return false;
	}

	try
	{
		raw_writer_.write(frame);
		return true;
	}
	catch(const cv::Exception& error)
	{
		ETEST_LOG_ERROR("VIDEO_RECORDER",
		                std::string("write raw frame error: ")
		                    + error.what());
		recording_disabled_due_to_error_ = true;
		return false;
	}
	catch(...)
	{
		ETEST_LOG_ERROR("VIDEO_RECORDER",
		                "unknown write raw frame error");
		recording_disabled_due_to_error_ = true;
		return false;
	}
}

bool VideoRecorder::writeDebug(const cv::Mat& frame) noexcept
{
	if(!config_.enabled || !config_.save_debug)
	{
		return true;
	}

	if(recording_disabled_due_to_error_)
	{
		return false;
	}

	if(shouldRotate())
	{
		if(debug_writer_.isOpened())
		{
			debug_writer_.release();
		}
	}

	if(!ensureOpened(debug_writer_, frame, "debug"))
	{
		return false;
	}

	try
	{
		debug_writer_.write(frame);
		return true;
	}
	catch(const cv::Exception& error)
	{
		ETEST_LOG_ERROR("VIDEO_RECORDER",
		                std::string("write debug frame error: ")
		                    + error.what());
		recording_disabled_due_to_error_ = true;
		return false;
	}
	catch(...)
	{
		ETEST_LOG_ERROR("VIDEO_RECORDER",
		                "unknown write debug frame error");
		recording_disabled_due_to_error_ = true;
		return false;
	}
}

void VideoRecorder::release() noexcept
{
	try
	{
		if(raw_writer_.isOpened())
		{
			raw_writer_.release();
			ETEST_LOG_INFO("VIDEO_RECORDER", "raw writer released");
		}

		if(debug_writer_.isOpened())
		{
			debug_writer_.release();
			ETEST_LOG_INFO("VIDEO_RECORDER", "debug writer released");
		}
	}
	catch(const cv::Exception& error)
	{
		ETEST_LOG_ERROR("VIDEO_RECORDER",
		                std::string("release error: ") + error.what());
	}
	catch(...)
	{
		ETEST_LOG_ERROR("VIDEO_RECORDER", "unknown release error");
	}
}

} // namespace etest::vision