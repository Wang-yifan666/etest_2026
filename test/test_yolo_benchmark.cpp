#include "vision/vision.hpp"
#include "vision/yolo_backend.hpp"
#include "vision/yolo_detector.hpp"

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
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

	struct TimingStats
	{
		double mean = 0.0;
		double p50 = 0.0;
		double p90 = 0.0;
		double p95 = 0.0;
		double p99 = 0.0;
		double min_val = 0.0;
		double max_val = 0.0;
		double fps = 0.0;
	};

	TimingStats computeStats(std::vector<double>& timings_ms)
	{
		TimingStats s;
		if(timings_ms.empty())
			return s;

		const double total =
		    std::accumulate(timings_ms.begin(), timings_ms.end(), 0.0);
		s.mean = total / static_cast<double>(timings_ms.size());

		auto [min_it, max_it] =
		    std::minmax_element(timings_ms.begin(), timings_ms.end());
		s.min_val = *min_it;
		s.max_val = *max_it;

		s.p50 = percentile(timings_ms, 0.50);
		s.p90 = percentile(timings_ms, 0.90);
		s.p95 = percentile(timings_ms, 0.95);
		s.p99 = percentile(timings_ms, 0.99);
		s.fps = 1000.0 / s.mean;

		return s;
	}

	void printPhaseStats(const std::string& name,
	                     const std::vector<double>& timings_ms)
	{
		auto stats =
		    computeStats(const_cast<std::vector<double>&>(timings_ms));

		std::cout << std::fixed << std::setprecision(2);
		std::cout << std::left << std::setw(16) << name;
		std::cout << " mean=" << std::right << std::setw(8)
		          << stats.mean;
		std::cout << "  p50=" << std::setw(8) << stats.p50;
		std::cout << "  p90=" << std::setw(8) << stats.p90;
		std::cout << "  p95=" << std::setw(8) << stats.p95;
		std::cout << "  p99=" << std::setw(8) << stats.p99;
		std::cout << "  min=" << std::setw(8) << stats.min_val;
		std::cout << "  max=" << std::setw(8) << stats.max_val;
		std::cout << "  fps=" << std::setw(7) << std::setprecision(1)
		          << stats.fps << std::setprecision(2) << "\n";
	}

	struct Args
	{
		std::string backend = "opencv";
		std::string model_path;
		std::string param_path;
		std::string class_names_path = "model/onnx_640_640/classes.txt";
		std::string image_path = "data/images/test.jpg";
		int input_width = 640;
		int input_height = 640;
		int threads = 4;
		int warmup = 30;
		int iterations = 200;
		bool use_fp16_storage = false;
		bool use_fp16_arithmetic = false;
		bool use_vulkan = false;
	};

	Args parseArgs(int argc, char** argv)
	{
		Args a;

		// 兼容旧版位置参数：<model.onnx> <classes.txt> <image> [warmup]
		// [iterations] [threads]
		if(argc >= 4 && argv[1][0] != '-')
		{
			a.model_path = argv[1];
			a.class_names_path = argv[2];
			a.image_path = argv[3];

			if(argc >= 5)
				a.warmup = std::stoi(argv[4]);

			if(argc >= 6)
				a.iterations = std::stoi(argv[5]);

			if(argc >= 7)
				a.threads = std::stoi(argv[6]);

			return a;
		}

		for(int i = 1; i < argc; ++i)
		{
			std::string arg = argv[i];
			if(arg == "--backend" && i + 1 < argc)
				a.backend = argv[++i];
			else if(arg == "--model" && i + 1 < argc)
				a.model_path = argv[++i];
			else if(arg == "--param" && i + 1 < argc)
				a.param_path = argv[++i];
			else if(arg == "--classes" && i + 1 < argc)
				a.class_names_path = argv[++i];
			else if(arg == "--image" && i + 1 < argc)
				a.image_path = argv[++i];
			else if(arg == "--width" && i + 1 < argc)
				a.input_width = std::stoi(argv[++i]);
			else if(arg == "--height" && i + 1 < argc)
				a.input_height = std::stoi(argv[++i]);
			else if(arg == "--threads" && i + 1 < argc)
				a.threads = std::stoi(argv[++i]);
			else if(arg == "--warmup" && i + 1 < argc)
				a.warmup = std::stoi(argv[++i]);
			else if(arg == "--iterations" && i + 1 < argc)
				a.iterations = std::stoi(argv[++i]);
			else if(arg == "--fp16-storage")
				a.use_fp16_storage = true;
			else if(arg == "--fp16-arithmetic")
				a.use_fp16_arithmetic = true;
			else if(arg == "--vulkan")
				a.use_vulkan = true;
		}

		return a;
	}

	void printUsage(const char* prog)
	{
		std::cerr
		    << "Usage: " << prog << " [options]\n"
		    << "\n"
		    << "  --backend opencv|ncnn    (default: opencv)\n"
		    << "  --model <path>           ONNX model for opencv\n"
		    << "  --param <path>           .param file for ncnn\n"
		    << "  --classes <path>         class names file\n"
		    << "  --image <path>           test image\n"
		    << "  --width  <int>           input width\n"
		    << "  --height <int>           input height\n"
		    << "  --threads <int>          CPU threads\n"
		    << "  --warmup <int>           warmup iterations\n"
		    << "  --iterations <int>       benchmark iterations\n"
		    << "  --fp16-storage           enable FP16 storage "
		       "(ncnn)\n"
		    << "  --fp16-arithmetic        enable FP16 arithmetic "
		       "(ncnn)\n"
		    << "  --vulkan                 enable Vulkan compute "
		       "(ncnn)\n"
		    << "\n"
		    << "Legacy positional:\n"
		    << "  " << prog
		    << " <model.onnx> <classes.txt> <image> "
		       "[warmup=30] [iterations=200] [threads=4]\n";
	}

} // namespace

int main(int argc, char** argv)
{
	if(argc < 2)
	{
		printUsage(argv[0]);
		return EXIT_FAILURE;
	}

	try
	{
		const Args args = parseArgs(argc, argv);

		cv::setUseOptimized(true);
		cv::setNumThreads(args.threads);

		cv::Mat frame = cv::imread(args.image_path, cv::IMREAD_COLOR);

		if(frame.empty())
		{
			std::cerr << "Failed to read image: " << args.image_path
			          << "\n";
			return EXIT_FAILURE;
		}

		// ── 构造后端配置 ──
		etest::vision::YoloBackendConfig backend_cfg;
		backend_cfg.input_width = args.input_width;
		backend_cfg.input_height = args.input_height;
		backend_cfg.num_threads = args.threads;
		backend_cfg.use_fp16_storage = args.use_fp16_storage;
		backend_cfg.use_fp16_arithmetic = args.use_fp16_arithmetic;
		backend_cfg.use_vulkan = args.use_vulkan;

		// ── 加载类别 ──
		std::vector<std::string> class_names;
		{
			std::ifstream f(args.class_names_path);
			if(!f.is_open())
			{
				std::cerr << "Cannot open class names: "
				          << args.class_names_path << "\n";
				return EXIT_FAILURE;
			}

			std::string line;
			while(std::getline(f, line))
				if(!line.empty())
					class_names.push_back(line);
		}

		constexpr float confidence_threshold = 0.45F;
		constexpr float nms_threshold = 0.45F;

		// ── 创建检测器 + 后端 ──
		etest::vision::YoloDetector detector;
		std::string err;

		if(args.backend == "ncnn")
		{
#ifdef ETEST_HAS_NCNN
			if(args.param_path.empty())
			{
				std::cerr << "--param is required for ncnn "
				             "backend\n";
				return EXIT_FAILURE;
			}

			auto ncnn = etest::vision::createNcnnBackend(
			    args.param_path, backend_cfg);

			if(!ncnn)
			{
				std::cerr << "Failed to create NCNN backend\n";
				return EXIT_FAILURE;
			}

			if(!detector.initialize(
			       std::move(ncnn), backend_cfg, std::move(class_names),
			       confidence_threshold, nms_threshold, err))
			{
				std::cerr << "Detector init failed: " << err << "\n";
				return EXIT_FAILURE;
			}
#else
			std::cerr << "NCNN backend requested but project was "
			             "built without NCNN (ETEST_ENABLE_NCNN=OFF)\n";
			return EXIT_FAILURE;
#endif
		}
		else
		{
			// opencv
			if(args.model_path.empty())
			{
				std::cerr << "--model is required for opencv "
				             "backend\n";
				return EXIT_FAILURE;
			}

			auto ocv = etest::vision::createOpenCvBackend();
			std::string load_err;

			if(!ocv->load(args.model_path, backend_cfg, load_err))
			{
				std::cerr << "OpenCV load failed: " << load_err << "\n";
				return EXIT_FAILURE;
			}

			if(!detector.initialize(
			       std::move(ocv), backend_cfg, std::move(class_names),
			       confidence_threshold, nms_threshold, err))
			{
				std::cerr << "Detector init failed: " << err << "\n";
				return EXIT_FAILURE;
			}
		}

		// ── 打印配置 ──
		std::cout << "Backend:        " << detector.backendName()
		          << "\n";
		if(args.backend == "opencv")
			std::cout << "Model:          " << args.model_path << "\n";
		else
			std::cout << "Param:          " << args.param_path << "\n";
		std::cout << "OpenCV version: " << CV_VERSION << "\n";
		std::cout << "Threads:        " << args.threads << "\n";
		std::cout << "Input frame:    " << frame.cols << "x"
		          << frame.rows << "\n";
		std::cout << "Model input:    " << args.input_width << "x"
		          << args.input_height << "\n";
		if(args.backend == "ncnn")
		{
			std::cout << "FP16 storage:   "
			          << (args.use_fp16_storage ? "true" : "false")
			          << "\n";
			std::cout << "FP16 arithmetic: "
			          << (args.use_fp16_arithmetic ? "true" : "false")
			          << "\n";
			std::cout << "Vulkan:         "
			          << (args.use_vulkan ? "true" : "false") << "\n";
		}
		std::cout << "Warm-up:        " << args.warmup << "\n";
		std::cout << "Iterations:     " << args.iterations << "\n\n";

		// ── Warm-up ──
		for(int i = 0; i < args.warmup; ++i)
			detector.infer(frame);

		// ── 收集拆分计时 ──
		std::vector<double> preprocess_ms, forward_ms, decode_ms,
		    nms_ms, total_ms;

		preprocess_ms.reserve(args.iterations);
		forward_ms.reserve(args.iterations);
		decode_ms.reserve(args.iterations);
		nms_ms.reserve(args.iterations);
		total_ms.reserve(args.iterations);

		std::size_t total_detection_count = 0;

		for(int i = 0; i < args.iterations; ++i)
		{
			etest::vision::YoloTiming timing;
			const auto detections = detector.infer(frame, &timing);

			total_detection_count += detections.size();

			preprocess_ms.push_back(timing.preprocess_ms);
			forward_ms.push_back(timing.forward_ms);
			decode_ms.push_back(timing.decode_ms);
			nms_ms.push_back(timing.nms_ms);
			total_ms.push_back(timing.total_ms);
		}

		std::cout << "─── 各阶段耗时 ───\n\n";

		printPhaseStats("Preprocess", preprocess_ms);
		printPhaseStats("Forward", forward_ms);
		printPhaseStats("Decode", decode_ms);
		printPhaseStats("NMS", nms_ms);
		printPhaseStats("Total", total_ms);

		std::cout << "\nAverage detections/frame: "
		          << static_cast<double>(total_detection_count)
		        / args.iterations
		          << "\n";

		// ── 打印首帧检测签名 ──
		std::cout << "\n─── 首帧检测签名 ───\n";
		{
			const auto dets = detector.infer(frame);
			for(const auto& d: dets)
			{
				std::cout << "class=" << d.class_id
				          << " confidence=" << std::fixed
				          << std::setprecision(4) << d.confidence
				          << " box=(" << d.box.x << "," << d.box.y
				          << "," << d.box.width << "," << d.box.height
				          << ") center=(" << d.center().x << ","
				          << d.center().y << ")\n";
			}
		}

		return EXIT_SUCCESS;
	}
	catch(const std::exception& error)
	{
		std::cerr << "Benchmark error: " << error.what() << "\n";
		return EXIT_FAILURE;
	}
}