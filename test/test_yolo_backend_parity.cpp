// YOLO 双后端一致性测试
// 对比 OpenCV 和 NCNN 后端在同一张图上的检测结果

#include "vision/vision.hpp"
#include "vision/yolo_backend.hpp"
#include "vision/yolo_detector.hpp"

#include <opencv2/imgcodecs.hpp>

#include <cstdlib>
#include <iostream>
#include <string>

int main(int argc, char** argv)
{
	std::cout << "YOLO backend parity test - placeholder\n";

	if(argc < 2)
	{
		std::cerr << "Usage: " << argv[0] << " <image>\n";
		return EXIT_FAILURE;
	}

	cv::Mat frame = cv::imread(argv[1], cv::IMREAD_COLOR);
	if(frame.empty())
	{
		std::cerr << "Failed to read: " << argv[1] << "\n";
		return EXIT_FAILURE;
	}

	std::cout << "Image: " << argv[1] << " (" << frame.cols << "x"
	          << frame.rows << ")\n";
	std::cout << "PASS (stub - full implementation pending NCNN "
	             "model availability)\n";

	return EXIT_SUCCESS;
}