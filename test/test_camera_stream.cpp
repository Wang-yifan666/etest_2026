#include "core/config.hpp"
#include "core/logger.hpp"
#include "vision/camera.hpp"

#include <cstdlib>
#include <iostream>

int main()
{
	// 此测试依赖真实的 USB 摄像头和网络连接，
	// 不加入 ctest 自动执行。

	etest::CameraConfig camera_config;
	camera_config.source =
	    "/dev/v4l/by-id/usb-170428-_Integrated_Webcam_HD-video-index0";
	camera_config.width = 1280;
	camera_config.height = 720;
	camera_config.fps = 30;
	camera_config.fourcc = "MJPG";

	etest::StreamConfig stream_config;
	stream_config.enabled = true;
	stream_config.host = "10.178.117.236";
	stream_config.port = 5000;
	stream_config.payload_type = 26;
	stream_config.mtu = 1200;
	stream_config.allow_fallback = false;

	etest::vision::Camera camera(camera_config, stream_config, 500);

	if(!camera.open())
	{
		std::cerr << "[FAIL] camera.open() returned false\n";
		return 1;
	}

	for(int i = 0; i < 300; ++i)
	{
		cv::Mat frame;

		if(!camera.read(frame))
		{
			std::cerr << "[FAIL] camera.read() returned false at frame "
			          << i << "\n";
			return 2;
		}

		if(frame.empty() || frame.cols != 1280 || frame.rows != 720
		   || frame.type() != CV_8UC3)
		{
			std::cerr << "[FAIL] invalid frame at frame " << i
			          << ": cols=" << frame.cols
			          << ", rows=" << frame.rows
			          << ", type=" << frame.type() << "\n";
			return 3;
		}
	}

	std::cout << "[PASS] camera streaming test passed\n";
	return 0;
}