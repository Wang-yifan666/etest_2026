#pragma once

#include <opencv2/opencv.hpp>
#include <string>

namespace etest::vision
{

	struct CameraConfig
	{
		// "0" 表示摄像头 0
		// 也可以传入视频文件路径
		std::string source = "0";

		int width = 640;
		int height = 480;
		int fps = 60;
	};

	class Camera
	{
	public:
		explicit Camera(CameraConfig config);

		bool open();
		bool read(cv::Mat& frame);

		bool isOpened() const;

		void release();

	private:
		CameraConfig config_;
		cv::VideoCapture cap_;
	};

} // namespace etest::vision
