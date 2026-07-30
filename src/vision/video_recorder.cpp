#include "vision/video_recorder.hpp"

namespace etest::vision
{

	VideoRecorder::VideoRecorder(RecordConfig config):
	config_(std::move(config))
	{
	}

	bool VideoRecorder::writeRaw(const cv::Mat&) noexcept
	{
		return false;
	}

	bool VideoRecorder::writeDebug(const cv::Mat&) noexcept
	{
		return false;
	}

	void VideoRecorder::release() noexcept
	{
		if(raw_writer_.isOpened())
			raw_writer_.release();
		if(debug_writer_.isOpened())
			debug_writer_.release();
	}

	bool VideoRecorder::ensureOpened(cv::VideoWriter&, const cv::Mat&,
	                                 const std::string&) noexcept
	{
		return false;
	}

	bool VideoRecorder::shouldRotate() const noexcept
	{
		return false;
	}

	std::string VideoRecorder::makeFilePath(const std::string&) const
	{
		return {};
	}

} // namespace etest::vision