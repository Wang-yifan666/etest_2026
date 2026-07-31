/**
 * etest_2026 标定采样 worker (V2)。
 *
 * 从 stdin 接收指令，使用与主程序相同的 NCNN 检测器进行帧推理，
 * 结果通过 stdout 返回。制表符分隔协议，无需 JSON 依赖。
 *
 * 用法：
 *   ./build/etest_calibration_worker config/
 *
 * V2 协议（▶ 发送，◀ 接收）：
 *   ▶ HELLO\t2
 *   ◀ READY\t2
 *
 *   ▶ RESET
 *   ◀ RESET_OK
 *
 *   ▶ INFER\t17\t/tmp/frame_17.png\tFULL
 *   ◀ RESULT\t17\tOK\t640.250\t320.500\t0.914
 *   ◀ RESULT\t17\tLOST
 *   ◀ RESULT\t17\tERROR\t描述
 *
 *   ▶ QUIT
 */

#include "core/config.hpp"
#include "vision/ball_ncnn_manager.hpp"

#include <opencv2/imgcodecs.hpp>

#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace
{
	std::vector<std::string> splitTabs(const std::string& line)
	{
		std::vector<std::string> parts;
		std::stringstream stream(line);
		std::string part;

		while(std::getline(stream, part, '\t'))
		{
			parts.push_back(part);
		}

		return parts;
	}

	etest::TrackingMode parseMode(const std::string& mode)
	{
		if(mode == "CENTER")
		{
			return etest::TrackingMode::CENTER;
		}

		return etest::TrackingMode::FULL;
	}
} // namespace

int main(int argc, char** argv)
{
	if(argc != 2)
	{
		std::cerr
		    << "Usage: etest_calibration_worker <config_dir>\n";
		return EXIT_FAILURE;
	}

	const auto load_result =
	    etest::loadAppConfigFromDir(argv[1]);

	if(!load_result.file_loaded)
	{
		std::cout << "ERROR\t无法加载配置目录\n" << std::flush;
		return EXIT_FAILURE;
	}

	const auto& app_config = load_result.config;
	const auto& bn = app_config.vision.ball_ncnn;

	etest::vision::BallNcnnManager manager;
	std::string error;

	if(!manager.initialize(bn, error))
	{
		std::cout << "ERROR\t" << error << "\n" << std::flush;
		return EXIT_FAILURE;
	}

	// 向 stderr 输出当前使用的模型配置
	std::cerr
	    << "[worker] 模型已加载\n"
	    << "[worker]   FULL model : "
	    << bn.full_input_width << "×" << bn.full_input_height
	    << "  (ROI " << bn.full_src_width << "×"
	    << bn.full_src_height << "; frame "
	    << app_config.camera.width << "×"
	    << app_config.camera.height << ")\n"
	    << "[worker]   CENTER model: "
	    << bn.center_input_width << "×" << bn.center_input_height
	    << "  (ROI " << bn.center_src_width << "×"
	    << bn.center_src_height << "; frame "
	    << app_config.camera.width << "×"
	    << app_config.camera.height << ")\n"
	    << "[worker]   threads=" << bn.num_threads
	    << " fp16_storage=" << (bn.use_fp16_storage ? "true" : "false")
	    << " fp16_arithmetic="
	    << (bn.use_fp16_arithmetic ? "true" : "false")
	    << "\n"
	    << "[worker]   confidence_min=" << bn.minimum_confidence
	    << "\n"
	    << std::flush;

	// ── V2 握手 ──
	std::string line;
	std::getline(std::cin, line);

	if(line != "HELLO\t2")
	{
		std::cout << "ERROR\t协议版本不匹配，需要 V2\n"
		          << std::flush;
		return EXIT_FAILURE;
	}

	std::cout << "READY\t2\n" << std::flush;
	std::string last_mode = "";

	while(std::getline(std::cin, line))
	{
		if(line.empty())
		{
			continue;
		}

		if(line == "QUIT")
		{
			break;
		}

		if(line == "RESET")
		{
			manager.resetTracking();
			std::cout << "RESET_OK\n" << std::flush;
			last_mode = "";
			continue;
		}

		const auto parts = splitTabs(line);

		// INFER  请求ID  图片路径  FULL/CENTER
		if(parts.size() != 4 || parts[0] != "INFER")
		{
			std::cout << "RESULT\t0\tERROR\t非法命令\n"
			          << std::flush;
			continue;
		}

		const std::string& request_id = parts[1];
		const std::string& image_path = parts[2];
		const std::string& mode_str = parts[3];

		// 模式变化时输出日志
		if(mode_str != last_mode)
		{
			last_mode = mode_str;
			if(last_mode == "FULL")
			{
				std::cerr
				    << "[worker] mode=FULL ("
				    << bn.full_input_width << "×"
				    << bn.full_input_height << ")\n"
				    << std::flush;
			}
			else if(last_mode == "CENTER")
			{
				std::cerr
				    << "[worker] mode=CENTER ("
				    << bn.center_input_width << "×"
				    << bn.center_input_height << ")\n"
				    << std::flush;
			}
		}

		cv::Mat frame =
		    cv::imread(image_path, cv::IMREAD_COLOR);

		if(frame.empty())
		{
			std::cout
			    << "RESULT\t" << request_id
			    << "\tERROR\t无法读取图片\n"
			    << std::flush;
			continue;
		}

		const auto measurement = manager.process(
		    frame,
		    parseMode(mode_str));

		if(!measurement.valid)
		{
			std::cout
			    << "RESULT\t" << request_id
			    << "\tLOST\n"
			    << std::flush;
			continue;
		}

		std::cout
		    << std::fixed
		    << std::setprecision(3)
		    << "RESULT\t" << request_id << "\tOK\t"
		    << measurement.global_center.x << "\t"
		    << measurement.global_center.y << "\t"
		    << measurement.confidence
		    << "\n"
		    << std::flush;
	}

	return EXIT_SUCCESS;
}
