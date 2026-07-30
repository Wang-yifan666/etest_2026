// etest_2026 Ball 视频回归测试工具
// 用法: ./etest_ball_video_test /path/to/video.mp4 config/vision.toml [--output result.mp4]
// 不接受失败的硬编码，统计指标用于 CI/验收。

#include "core/config.hpp"
#include "core/logger.hpp"
#include "vision/vision.hpp"

#include <opencv2/core.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace
{

	struct TestStats
	{
		int total_frames = 0;
		int measured_frames = 0;
		int lost_frames = 0;
		int calibrating_frames = 0;
		int error_frames = 0;
		int max_consecutive_lost = 0;
		int current_consecutive_lost = 0;
		double average_confidence = 0.0;
		double max_position_jump_ratio = 0.0;
		double prev_valid_x = -1.0;
		int video_w = 0;
		int video_h = 0;

		void record(const etest::vision::VisionResult& res)
		{
			++total_frames;

			if(res.valid && res.calibrated)
			{
				++measured_frames;
				current_consecutive_lost = 0;
				average_confidence += res.confidence;

				if(prev_valid_x >= 0.0 && video_w > 0)
				{
					double jump =
					    std::abs(res.x - prev_valid_x) / video_w;
					if(jump > max_position_jump_ratio)
						max_position_jump_ratio = jump;
				}
				prev_valid_x = res.x;
			}
			else if(res.error_code == "BALL_LOST"
			        || res.error_code == "NO_CIRCLE_CANDIDATE"
			        || res.error_code == "ALL_CANDIDATES_REJECTED")
			{
				++lost_frames;
				++current_consecutive_lost;
				if(current_consecutive_lost > max_consecutive_lost)
					max_consecutive_lost = current_consecutive_lost;
				prev_valid_x = -1.0;
			}
			else if(res.error_code == "ZERO_CALIBRATING")
			{
				++calibrating_frames;
				current_consecutive_lost = 0;
			}
			else
			{
				++error_frames;
				current_consecutive_lost = 0;
				prev_valid_x = -1.0;
			}
		}

		void finish()
		{
			if(measured_frames > 0)
				average_confidence /= measured_frames;
		}
	};

} // namespace

int main(int argc, char** argv)
{
	if(argc < 3)
	{
		std::cerr << "usage: " << argv[0]
		          << " <video.mp4> <config.toml> [--output out.mp4]\n";
		return 1;
	}

	std::string video_path = argv[1];
	std::string config_path = argv[2];
	std::string output_path;
	bool save_video = false;
	for(int i = 3; i < argc; ++i)
	{
		std::string arg = argv[i];
		if(arg == "--output" && i + 1 < argc)
			output_path = argv[++i];
		else if(arg == "--save-video")
			save_video = true;
	}
	// 加载配置
	auto load_result = etest::ConfigLoader::load(config_path);
	if(!load_result.file_loaded)
	{
		std::cerr << "error: failed to load config: " << config_path
		          << "\n";
		return 1;
	}
	etest::VisionConfig vision_cfg;
	vision_cfg.ball = load_result.config.vision.ball;

	// 配置中的 video_output 优先（CLI --output 覆盖，--save-video 次之）
	if(output_path.empty() && !vision_cfg.ball.video_output.empty())
		output_path = vision_cfg.ball.video_output;
	// --save-video 自动生成输出路径
	if(save_video && output_path.empty())
	{
		auto slash = video_path.rfind('/');
		auto dot = video_path.rfind('.');
		std::string stem;
		if(slash != std::string::npos && dot != std::string::npos
		   && dot > slash)
			stem = video_path.substr(slash + 1, dot - slash - 1);
		else if(dot != std::string::npos)
			stem = video_path.substr(0, dot);
		else
			stem = "output";
		output_path = stem + "_debug.mp4";
	}

	// 打开视频
	cv::VideoCapture cap(video_path);
	if(!cap.isOpened())
	{
		std::cerr << "error: cannot open video: " << video_path << "\n";
		return 2;
	}
	int vid_w = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_WIDTH));
	int vid_h = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_HEIGHT));
	double vid_fps = cap.get(cv::CAP_PROP_FPS);
	if(vid_fps <= 0.0)
		vid_fps = 30.0;

	// 创建视觉处理器
	etest::vision::VisionProcessor vp(vision_cfg);

	// 确保输出路径有 .mp4 扩展名
	if(!output_path.empty())
	{
		auto ext = output_path.rfind('.');
		if(ext == std::string::npos || (output_path.size() - ext > 5))
		{
			output_path += ".mp4";
		}
	}

	// 可选输出视频（尺寸 = 原始视频尺寸，drawDebugInfo 原地替换）
	cv::VideoWriter writer;
	if(!output_path.empty())
	{
		writer.open(output_path,
		            cv::VideoWriter::fourcc('M', 'J', 'P', 'G'),
		            vid_fps, cv::Size(vid_w, vid_h));
		if(!writer.isOpened())
		{
			std::cerr << "warning: cannot open output video: "
			          << output_path << "\n";
		}
	}

	TestStats stats;
	stats.video_w = vid_w;
	stats.video_h = vid_h;
	cv::Mat frame;
	int frame_idx = 0;

	auto t0 = std::chrono::steady_clock::now();

	while(true)
	{
		try
		{
			if(!cap.read(frame) || frame.empty())
			{
				// 检查是否 EOF 或永久错误
				if(frame_idx == 0)
				{
					std::cerr << "error: failed to read first frame\n";
					return 3;
				}
				// 尝试再读一次
				if(!cap.read(frame) || frame.empty())
					break; // EOF
			}

			auto result =
			    vp.process(frame, etest::vision::VisionMode::Ball);
			stats.record(result);

			if(writer.isOpened())
			{
				cv::Mat display = frame.clone();
				vp.drawDebugInfo(display, result);
				writer.write(display);
			}

			++frame_idx;
		}
		catch(const std::exception& e)
		{
			std::cerr << "exception at frame " << frame_idx << ": "
			          << e.what() << "\n";
			++frame_idx;
			// 继续处理后续帧
		}
		catch(...)
		{
			std::cerr << "unknown exception at frame " << frame_idx
			          << "\n";
			++frame_idx;
		}
	}

	auto t1 = std::chrono::steady_clock::now();
	double elapsed = std::chrono::duration<double>(t1 - t0).count();

	stats.finish();

	double measurement_rate = stats.total_frames > 0
	    ? static_cast<double>(stats.measured_frames)
	        / stats.total_frames
	    : 0.0;
	double fps = elapsed > 0.0 ? stats.total_frames / elapsed : 0.0;
	double avg_conf =
	    stats.measured_frames > 0 ? stats.average_confidence : 0.0;

	// 输出统计
	std::cout << "======================================\n";
	std::cout << " Ball Video Regression Test\n";
	std::cout << "======================================\n";
	std::cout << "video:          " << video_path << "\n";
	std::cout << "resolution:     " << vid_w << "x" << vid_h << " @ "
	          << vid_fps << " fps\n";
	std::cout << "total_frames:   " << stats.total_frames << "\n";
	std::cout << "measured_frames:" << stats.measured_frames << "\n";
	std::cout << "lost_frames:    " << stats.lost_frames << "\n";
	std::cout << "calibrating:    " << stats.calibrating_frames << "\n";
	std::cout << "error_frames:   " << stats.error_frames << "\n";
	std::cout << "measurement_rate: " << measurement_rate << "\n";
	std::cout << "max_consecutive_lost: " << stats.max_consecutive_lost
	          << "\n";
	std::cout << "avg_confidence: " << avg_conf << "\n";
	std::cout << "max_position_jump_ratio: "
	          << stats.max_position_jump_ratio << "\n";
	std::cout << "processing_fps: " << fps << "\n";

	if(!output_path.empty())
		std::cout << "output:         " << output_path << "\n";
	std::cout << "======================================\n";

	// 验收阈值检查（非硬编码，仅打印）
	bool pass = true;
	if(measurement_rate < 0.90)
	{
		std::cout << "[FAIL] measurement_rate " << measurement_rate
		          << " < 0.90\n";
		pass = false;
	}
	if(stats.max_consecutive_lost > 5)
	{
		std::cout << "[FAIL] max_consecutive_lost "
		          << stats.max_consecutive_lost << " > 5\n";
		pass = false;
	}
	if(stats.max_position_jump_ratio > 0.12)
	{
		std::cout << "[FAIL] max_position_jump_ratio "
		          << stats.max_position_jump_ratio << " > 0.12\n";
		pass = false;
	}
	if(fps < 30.0)
	{
		std::cout << "[WARN] processing_fps " << fps
		          << " < 30 (performance notice)\n";
		// 不强制 fail，取决于硬件
	}

	if(pass)
		std::cout << "[PASS] all acceptance criteria met\n";
	else
		std::cout << "[FAIL] some acceptance criteria not met\n";

	cap.release();
	if(writer.isOpened())
		writer.release();

	return pass ? 0 : 4;
}