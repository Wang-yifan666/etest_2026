#include "vision/vision.hpp"

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
	using Clock = std::chrono::steady_clock;

	double percentile(std::vector<double> values, double ratio)
	{
		if(values.empty())
			return 0.0;

		std::sort(values.begin(), values.end());

		const double position =
		    ratio * static_cast<double>(values.size() - 1);

		const auto lower =
		    static_cast<std::size_t>(std::floor(position));
		const auto upper =
		    static_cast<std::size_t>(std::ceil(position));

		if(lower == upper)
			return values[lower];

		const double weight = position - static_cast<double>(lower);

		return values[lower] * (1.0 - weight) + values[upper] * weight;
	}

	void printResult(const std::string& name,
	                 const std::vector<double>& timings_ms)
	{
		if(timings_ms.empty())
			return;

		const double total =
		    std::accumulate(timings_ms.begin(), timings_ms.end(), 0.0);

		const double mean =
		    total / static_cast<double>(timings_ms.size());

		const auto [minimum, maximum] =
		    std::minmax_element(timings_ms.begin(), timings_ms.end());

		std::cout << "\n" << name << "\n";
		std::cout << "--------------------------------\n";
		std::cout << std::fixed << std::setprecision(2);
		std::cout << "Mean: " << mean << " ms\n";
		std::cout << "P50:  " << percentile(timings_ms, 0.50)
		          << " ms\n";
		std::cout << "P90:  " << percentile(timings_ms, 0.90)
		          << " ms\n";
		std::cout << "P95:  " << percentile(timings_ms, 0.95)
		          << " ms\n";
		std::cout << "P99:  " << percentile(timings_ms, 0.99)
		          << " ms\n";
		std::cout << "Min:  " << *minimum << " ms\n";
		std::cout << "Max:  " << *maximum << " ms\n";
		std::cout << "FPS:  " << 1000.0 / mean << "\n";
	}

	int parsePositiveInt(const char* text, const char* name)
	{
		const int value = std::stoi(text);

		if(value <= 0)
			throw std::invalid_argument(std::string(name)
			                            + " must be greater than zero");

		return value;
	}

} // namespace

int main(int argc, char** argv)
{
	if(argc < 4)
	{
		std::cerr << "Usage:\n"
		          << "  " << argv[0]
		          << " <model.onnx> <classes.txt> <image>"
		          << " [warmup=30] [iterations=200] [threads=4]\n";

		return EXIT_FAILURE;
	}

	try
	{
		const std::string model_path = argv[1];
		const std::string class_names_path = argv[2];
		const std::string image_path = argv[3];

		const int warmup =
		    argc >= 5 ? parsePositiveInt(argv[4], "warmup") : 30;

		const int iterations =
		    argc >= 6 ? parsePositiveInt(argv[5], "iterations") : 200;

		const int threads =
		    argc >= 7 ? parsePositiveInt(argv[6], "threads") : 4;

		// 启用 OpenCV 优化，并固定线程数，保证测试可复现。
		cv::setUseOptimized(true);
		cv::setNumThreads(threads);

		cv::Mat frame = cv::imread(image_path, cv::IMREAD_COLOR);

		if(frame.empty())
		{
			std::cerr << "Failed to read image: " << image_path << "\n";
			return EXIT_FAILURE;
		}

		etest::vision::VisionProcessor processor;

		constexpr double confidence_threshold = 0.45;
		constexpr double nms_threshold = 0.45;

		if(!processor.loadNnModel(model_path, class_names_path,
		                          confidence_threshold, nms_threshold))
		{
			std::cerr << "Failed to load model: " << model_path << "\n";
			return EXIT_FAILURE;
		}

		std::cout << "OpenCV version: " << CV_VERSION << "\n";
		std::cout << "OpenCV threads: " << cv::getNumThreads() << "\n";
		std::cout << "Input frame:    " << frame.cols << "x"
		          << frame.rows << "\n";
		std::cout << "Model input:    640x640\n";
		std::cout << "Warm-up:        " << warmup << "\n";
		std::cout << "Iterations:     " << iterations << "\n";

		for(int i = 0; i < warmup; ++i)
			processor.inferYolo(frame);

		std::vector<double> inference_timings;
		inference_timings.reserve(iterations);

		std::size_t total_detection_count = 0;

		for(int i = 0; i < iterations; ++i)
		{
			const auto start = Clock::now();

			const auto detections = processor.inferYolo(frame);

			const auto end = Clock::now();

			total_detection_count += detections.size();

			const double elapsed_ms =
			    std::chrono::duration<double, std::milli>(end - start)
			        .count();

			inference_timings.push_back(elapsed_ms);
		}

		printResult("inferYolo: preprocess + forward + decode + NMS",
		            inference_timings);

		std::cout << "Average detections/frame: "
		          << static_cast<double>(total_detection_count)
		        / iterations
		          << "\n";

		processor.resetYoloSession();

		for(int i = 0; i < warmup; ++i)
		{
			processor.process(frame, etest::vision::VisionMode::Ball);
		}

		std::vector<double> process_timings;
		process_timings.reserve(iterations);

		int valid_count = 0;

		for(int i = 0; i < iterations; ++i)
		{
			const auto start = Clock::now();

			const auto result = processor.process(
			    frame, etest::vision::VisionMode::Ball);

			const auto end = Clock::now();

			if(result.valid)
				++valid_count;

			const double elapsed_ms =
			    std::chrono::duration<double, std::milli>(end - start)
			        .count();

			process_timings.push_back(elapsed_ms);
		}

		printResult("process(frame, VisionMode::Ball)",
		            process_timings);

		std::cout << "Valid results: " << valid_count << "/"
		          << iterations << "\n";

		return EXIT_SUCCESS;
	}
	catch(const std::exception& error)
	{
		std::cerr << "Benchmark error: " << error.what() << "\n";
		return EXIT_FAILURE;
	}
}


// ./build/etest_yolo_benchmark \
//     model/best.onnx \
//     model/classes.txt \
//     data/images/5_f00623.jpg \
//     30 \
//     200 \
//     4
