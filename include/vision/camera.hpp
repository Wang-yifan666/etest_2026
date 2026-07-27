#pragma once

#include "core/config.hpp"

#include <opencv2/core.hpp>
#include <opencv2/videoio.hpp>

namespace etest::vision
{

enum class CameraState
{
	OK,
	DISCONNECTED,
	FILE_EOF,
	ERROR
};

class Camera
{
public:
	explicit Camera(CameraConfig config, int retry_interval_ms = 500);

	bool open() noexcept;
	bool read(cv::Mat& frame) noexcept;
	bool isOpened() const noexcept;
	CameraState getState() const noexcept;
	int consecutiveFailures() const noexcept;
	bool isFileSource() const noexcept;
	bool loopVideo() const noexcept;
	void release() noexcept;

private:
	CameraConfig config_;
	int retry_interval_ms_;
	cv::VideoCapture cap_;
	bool read_error_reported_ = false;
	int consecutive_failures_ = 0;
	CameraState state_ = CameraState::OK;
	static constexpr int kMaxConsecutiveFailures = 20;
	bool file_source_ = false;
};

} // namespace etest::vision