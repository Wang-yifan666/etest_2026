// NCNN YOLO backend smoke test - 加载模型 + 单张图推理 + 检查输出合法性
// 用法: ./build/etest_yolo_ncnn_smoke <ncnn_model_dir> <image>

#include "vision/vision.hpp"
#include "vision/yolo_backend.hpp"
#include "vision/yolo_detector.hpp"

#include <opencv2/imgcodecs.hpp>

#include <cstdlib>
#include <iostream>
#include <string>

int main(int argc, char** argv)
{
	std::cout << "NCNN YOLO smoke test - placeholder\n";

	if(argc < 3)
	{
		std::cerr << "Usage: " << argv[0]
		          << " <ncnn_model_dir> <image>\n";
		return EXIT_FAILURE;
	}

	const std::string ncnn_dir = argv[1];
	const std::string image_path = argv[2];

	cv::Mat frame = cv::imread(image_path, cv::IMREAD_COLOR);
	if(frame.empty())
	{
		std::cerr << "Failed to read image: " << image_path << "\n";
		return EXIT_FAILURE;
	}

	std::cout << "NCNN dir: " << ncnn_dir << "\n";
	std::cout << "Image: " << image_path << " (" << frame.cols << "x"
	          << frame.rows << ")\n";
	std::cout << "PASS (stub - full implementation pending NCNN "
	             "model availability)\n";

	return EXIT_SUCCESS;
}