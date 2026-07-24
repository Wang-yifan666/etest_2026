#pragma once

#include "core/config.hpp"

#include <opencv2/core.hpp>
#include <opencv2/videoio.hpp>

namespace etest::vision
{
	class Camera
	{
	public:
		explicit Camera(CameraConfig config);

		bool open() noexcept;
		bool read(cv::Mat& frame) noexcept;
		bool isOpened() const noexcept;
		void release() noexcept;

	private:
		CameraConfig config_;
		cv::VideoCapture cap_;
		bool read_error_reported_ = false;
		int consecutive_failures_ = 0;
		static constexpr int kMaxConsecutiveFailures = 20;
	};

} // namespace etest::vision
