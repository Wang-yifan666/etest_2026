/**
 * etest_2026 标定采样 worker。
 *
 * 从 stdin 接收指令，使用与主程序相同的 NCNN 检测器进行帧推理，
 * 结果通过 stdout 返回。制表符分隔协议，无需 JSON 依赖。
 *
 * 用法：
 *   ./build/etest_calibration_worker config/vision.toml
 *
 * 协议（▶ 发送，◀ 接收）：
 *   ▶ RESET
 *   ◀ RESET_OK
 *
 *   ▶ INFER\t/tmp/frame.jpg\tFULL
 *   ◀ OK\t639.250\t320.500\t0.914\t-0.200
 *   ◀ LOST
 *   ◀ ERROR\t错误描述
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
		    << "Usage: etest_calibration_worker <vision.toml>\n";
		return EXIT_FAILURE;
	}

	const auto load_result =
	    etest::ConfigLoader::load(argv[1]);

	if(!load_result.file_loaded)
	{
		std::cout << "ERROR\t无法加载配置\n" << std::flush;
		return EXIT_FAILURE;
	}

	etest::vision::BallNcnnManager manager;
	std::string error;

	const auto& bn = load_result.config.vision.ball_ncnn;

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
	    << bn.full_src_height << " from 1280×640)\n"
	    << "[worker]   CENTER model: "
	    << bn.center_input_width << "×" << bn.center_input_height
	    << "  (ROI " << bn.center_src_width << "×"
	    << bn.center_src_height << " from 1280×640)\n"
	    << "[worker]   threads=" << bn.num_threads
	    << " fp16_storage=" << (bn.use_fp16_storage ? "true" : "false")
	    << " fp16_arithmetic="
	    << (bn.use_fp16_arithmetic ? "true" : "false")
	    << "\n"
	    << "[worker]   confidence_min=" << bn.minimum_confidence
	    << "\n"
	    << std::flush;

	std::cout << "READY\n" << std::flush;

	std::string line;
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

		if(parts.size() != 3 || parts[0] != "INFER")
		{
			std::cout
			    << "ERROR\t非法命令\n"
			    << std::flush;
			continue;
		}

		// 模式变化时输出日志
		if(parts[2] != last_mode)
		{
			last_mode = parts[2];
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
		    cv::imread(parts[1], cv::IMREAD_COLOR);

		if(frame.empty())
		{
			std::cout
			    << "ERROR\t无法读取图片\n"
			    << std::flush;
			continue;
		}

		const auto measurement = manager.process(
		    frame,
		    parseMode(parts[2]));

		if(!measurement.valid)
		{
			std::cout
			    << "LOST\n"
			    << std::flush;
			continue;
		}

		std::cout
		    << std::fixed
		    << std::setprecision(3)
		    << "OK\t"
		    << measurement.global_center.x << "\t"
		    << measurement.global_center.y << "\t"
		    << measurement.confidence << "\t"
		    << measurement.position_0p1mm / 10.0
		    << "\n"
		    << std::flush;
	}

	return EXIT_SUCCESS;
}