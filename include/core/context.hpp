#pragma once

#include <opencv2/opencv.hpp>

#include "vision/camera.hpp"
#include "vision/vision.hpp"
#include "uart/uart.hpp"

namespace etest
{

	struct AppContext
	{
		vision::Camera& camera;
		vision::VisionProcessor& vision;
		Uart& uart;

		cv::Mat frame;
		vision::VisionResult vision_result;

		bool running = true;
	};

} // namespace etest
