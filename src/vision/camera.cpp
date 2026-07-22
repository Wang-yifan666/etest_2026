#include "vision/camera.hpp"

#include <algorithm>
#include <cctype>
#include <iostream>

namespace etest::vision
{

	namespace
	{

		bool isInteger(const std::string& str)
		{
			if(str.empty())
			{
				return false;
			}

			return std::all_of(str.begin(), str.end(),
			                   [](unsigned char c) {
				                   return std::isdigit(c);
			                   });
		}

	} // namespace

	Camera::Camera(CameraConfig config): config_(std::move(config)) {}

	bool Camera::open()
	{
		bool success = false;

		// source = "0"、"1" 这种情况，认为是摄像头编号
		if(isInteger(config_.source))
		{
			const int camera_id = std::stoi(config_.source);

			success = cap_.open(camera_id, cv::CAP_ANY);

			if(success)
			{
				cap_.set(cv::CAP_PROP_FRAME_WIDTH, config_.width);
				cap_.set(cv::CAP_PROP_FRAME_HEIGHT, config_.height);
				cap_.set(cv::CAP_PROP_FPS, config_.fps);
			}
		}
		else
		{
			// 否则认为是视频文件
			success = cap_.open(config_.source);
		}

		if(!success)
		{
			std::cerr << "[Camera] Failed to open source: "
			          << config_.source << '\n';

			return false;
		}

		std::cout << "[Camera] Opened source: " << config_.source
		          << '\n';

		std::cout
		    << "[Camera] Resolution: "
		    << static_cast<int>(cap_.get(cv::CAP_PROP_FRAME_WIDTH))
		    << " x "
		    << static_cast<int>(cap_.get(cv::CAP_PROP_FRAME_HEIGHT))
		    << '\n';

		return true;
	}

	bool Camera::read(cv::Mat& frame)
	{
		if(!cap_.isOpened())
		{
			return false;
		}

		return cap_.read(frame) && !frame.empty();
	}

	bool Camera::isOpened() const
	{
		return cap_.isOpened();
	}

	void Camera::release()
	{
		if(cap_.isOpened())
		{
			cap_.release();
		}
	}

} // namespace etest::vision
