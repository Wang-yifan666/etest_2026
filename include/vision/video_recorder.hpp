#pragma once

#include "core/config.hpp"

#include <opencv2/core.hpp>
#include <opencv2/videoio.hpp>
#include <chrono>
#include <string>

namespace etest::vision
{

	class VideoRecorder
	{
	public:
		explicit VideoRecorder(RecordConfig config);

		bool writeRaw(const cv::Mat& frame) noexcept;
		bool writeDebug(const cv::Mat& frame) noexcept;

		void release() noexcept;

	private:
		bool ensureOpened(cv::VideoWriter& writer, const cv::Mat& frame,
		                  const std::string& type) noexcept;

		bool shouldRotate() const noexcept;
		std::string makeFilePath(const std::string& type) const;

	private:
		RecordConfig config_;

		cv::VideoWriter raw_writer_;
		cv::VideoWriter debug_writer_;

		std::chrono::steady_clock::time_point segment_start_;
		bool recording_disabled_due_to_error_ = false;
	};

} // namespace etest::vision