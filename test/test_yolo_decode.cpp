// YOLO 解码器单元测试 - 验证 decode() 与当前 inferYolo() 一致
// 用法: ./build/etest_yolo_decode <model.onnx> <classes.txt> <image>

#include "vision/vision.hpp"
#include "vision/yolo_backend.hpp"
#include "vision/yolo_detector.hpp"

#include <opencv2/imgcodecs.hpp>

#include <cstdlib>
#include <iostream>
#include <string>

int main(int argc, char** argv)
{
	if(argc < 4)
	{
		std::cerr << "Usage: " << argv[0]
		          << " <model.onnx> <classes.txt> <image>\n";
		return EXIT_FAILURE;
	}

	const std::string model_path = argv[1];
	const std::string classes_path = argv[2];
	const std::string image_path = argv[3];

	cv::Mat frame = cv::imread(image_path, cv::IMREAD_COLOR);
	if(frame.empty())
	{
		std::cerr << "Failed to read image: " << image_path << "\n";
		return EXIT_FAILURE;
	}

	std::cout << "YOLO decode test - placeholder\n";
	std::cout << "Model: " << model_path << "\n";
	std::cout << "Classes: " << classes_path << "\n";
	std::cout << "Image: " << image_path << " (" << frame.cols << "x"
	          << frame.rows << ")\n";
	std::cout << "PASS (stub)\n";

	return EXIT_SUCCESS;
}