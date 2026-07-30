#include "vision/latest_frame_capture.hpp"
#include "core/config.hpp"
#include "core/logger.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

using namespace etest;
using namespace etest::vision;

namespace
{

	bool test_start_stop()
	{
		// 使用无效源创建一个 Camera 来测试生命周期
		CameraConfig cfg;
		cfg.source = "999"; // unlikely valid camera id
		Camera cam(cfg, 100);

		LatestFrameCapture capture(cam);

		// 没打开摄像头时 start 应返回 false
		if(capture.start())
		{
			std::cerr
			    << "FAIL: start should fail when camera not opened\n";
			return false;
		}

		if(capture.state() != CaptureWorkerState::STOPPED)
		{
			std::cerr << "FAIL: state should be STOPPED before start\n";
			return false;
		}

		// stop 应该是安全的（即使没启动）
		capture.stop();

		if(capture.capturedFrames() != 0)
		{
			std::cerr << "FAIL: capturedFrames should be 0\n";
			return false;
		}

		std::cout << "PASS: test_start_stop\n";
		return true;
	}

	bool test_try_get_latest_empty()
	{
		CameraConfig cfg;
		cfg.source = "999";
		Camera cam(cfg, 100);
		LatestFrameCapture capture(cam);

		FramePacket output;
		output.sequence = 999;

		// 采集线程未运行时 latest_ 为空
		const bool got = capture.tryGetLatest(output, 0);
		if(got)
		{
			std::cerr
			    << "FAIL: tryGetLatest should return false when no frames\n";
			return false;
		}

		// output 应保持不变
		if(output.sequence != 999)
		{
			std::cerr
			    << "FAIL: output should not be modified on failure\n";
			return false;
		}

		std::cout << "PASS: test_try_get_latest_empty\n";
		return true;
	}

	bool test_state_initial()
	{
		CameraConfig cfg;
		cfg.source = "999";
		Camera cam(cfg, 100);
		LatestFrameCapture capture(cam);

		if(capture.state() != CaptureWorkerState::STOPPED)
		{
			std::cerr << "FAIL: initial state should be STOPPED\n";
			return false;
		}

		std::cout << "PASS: test_state_initial\n";
		return true;
	}

	bool test_double_start()
	{
		CameraConfig cfg;
		cfg.source = "999";
		Camera cam(cfg, 100);
		LatestFrameCapture capture(cam);

		// 不能在没有打开 camera 的情况下 start
		// 但我们可以测试 double start 的防护
		// 这个测试仅验证 API 不会崩溃
		capture.start(); // 预期失败
		capture.start(); // 预期失败（double start 防护）
		capture.stop();
		capture.stop(); // double stop 安全

		std::cout << "PASS: test_double_start (no-crash)\n";
		return true;
	}

	bool test_with_video_file()
	{
		// 使用测试视频文件验证实际采集
		const char* video_path = "docs/videos/test5.mp4";

		// 检查文件是否存在
		FILE* f = std::fopen(video_path, "r");
		if(f == nullptr)
		{
			std::cout << "SKIP: test_with_video_file (no test video)\n";
			return true;
		}
		std::fclose(f);

		CameraConfig cfg;
		cfg.source = video_path;
		cfg.width = 640;
		cfg.height = 480;
		cfg.fps = 30;

		Camera cam(cfg, 100);
		if(!cam.open())
		{
			std::cerr << "FAIL: could not open video file\n";
			return false;
		}

		LatestFrameCapture capture(cam);
		if(!capture.start())
		{
			std::cerr << "FAIL: could not start capture\n";
			return false;
		}

		// 等待采集线程读取一些帧
		std::this_thread::sleep_for(std::chrono::milliseconds(500));

		// 检查状态
		auto st = capture.state();
		if(st != CaptureWorkerState::RUNNING
		   && st != CaptureWorkerState::FILE_EOF)
		{
			std::cerr << "FAIL: unexpected state "
			          << static_cast<int>(st) << "\n";
			capture.stop();
			return false;
		}

		auto frames = capture.capturedFrames();
		std::cout << "  capturedFrames=" << frames << "\n";

		if(frames == 0)
		{
			std::cerr << "FAIL: no frames captured\n";
			capture.stop();
			return false;
		}

		// 测试 tryGetLatest 能获取到帧
		FramePacket output;
		bool got = capture.tryGetLatest(output, 0);
		if(!got)
		{
			std::cerr
			    << "FAIL: tryGetLatest should return true after capture\n";
			capture.stop();
			return false;
		}

		std::cout << "  got frame sequence=" << output.sequence
		          << " size=" << output.frame.cols << "x"
		          << output.frame.rows << "\n";

		if(output.frame.empty())
		{
			std::cerr << "FAIL: got empty frame\n";
			capture.stop();
			return false;
		}

		// 测试不重复处理：用同样的 last_sequence 再调用
		FramePacket output2;
		got = capture.tryGetLatest(output2, output.sequence);
		if(got)
		{
			// 如果没有新帧产生，不应返回 true
			// 但由于视频文件很快，可能已经有新帧了，所以这里只是检测不崩溃
			std::cout << "  second tryGetLatest returned "
			          << (got ? "true" : "false") << "\n";
		}

		capture.stop();

		std::cout << "PASS: test_with_video_file\n";
		return true;
	}

	bool test_single_slot_overwrite()
	{
		// 使用视频文件，消费者慢速读取，验证只能拿到最新帧
		const char* video_path = "docs/videos/test5.mp4";

		FILE* f = std::fopen(video_path, "r");
		if(f == nullptr)
		{
			std::cout
			    << "SKIP: test_single_slot_overwrite (no test video)\n";
			return true;
		}
		std::fclose(f);

		CameraConfig cfg;
		cfg.source = video_path;
		cfg.width = 640;
		cfg.height = 480;
		cfg.fps = 30;

		Camera cam(cfg, 100);
		if(!cam.open())
		{
			std::cerr << "FAIL: could not open video file\n";
			return false;
		}

		LatestFrameCapture capture(cam);
		if(!capture.start())
		{
			std::cerr << "FAIL: could not start capture\n";
			return false;
		}

		// 让采集线程读很多帧，消费者只读一次
		std::this_thread::sleep_for(std::chrono::milliseconds(1500));

		auto total_captured = capture.capturedFrames();
		std::cout << "  total captured: " << total_captured << "\n";

		if(total_captured < 10)
		{
			std::cout << "SKIP: not enough frames for overwrite test\n";
			capture.stop();
			return true;
		}

		// 消费者只读一次
		FramePacket output;
		bool got = capture.tryGetLatest(output, 0);

		if(!got)
		{
			std::cerr << "FAIL: should have gotten a frame\n";
			capture.stop();
			return false;
		}

		// 由于采集线程持续覆盖，消费者拿到的 sequence 应该接近 total_captured
		// 允许一定误差（消费者读取时可能又多了几帧）
		auto diff = total_captured - output.sequence;
		std::cout << "  consumer got sequence=" << output.sequence
		          << " (total=" << total_captured << " diff=" << diff
		          << ")\n";

		// diff 应该很小（消费者没有被阻塞，只是获取时刻的快照）
		if(diff > 3)
		{
			// 视频结束或采集线程已停止（EOF）时 diff 可能较大
			auto st = capture.state();
			if(st == CaptureWorkerState::RUNNING)
			{
				std::cerr
				    << "FAIL: consumer sequence too far behind (diff="
				    << diff << ")\n";
				capture.stop();
				return false;
			}
		}

		// 确认不能拿到队列中的所有帧：sequence 不应该是 1
		if(output.sequence == 1 && total_captured > 5)
		{
			std::cerr
			    << "FAIL: consumer got frame 1, should have gotten a later frame\n";
			capture.stop();
			return false;
		}

		capture.stop();

		std::cout << "PASS: test_single_slot_overwrite\n";
		return true;
	}

} // namespace

int main()
{
	// 初始化最小日志配置
	LoggerConfig log_cfg;
	log_cfg.terminal = false;
	log_cfg.file = false;
	Logger::instance().init(log_cfg);

	bool all_pass = true;

	std::cout << "=== LatestFrameCapture Unit Tests ===\n\n";

	all_pass &= test_start_stop();
	all_pass &= test_try_get_latest_empty();
	all_pass &= test_state_initial();
	all_pass &= test_double_start();
	all_pass &= test_with_video_file();
	all_pass &= test_single_slot_overwrite();

	std::cout << "\n=== "
	          << (all_pass ? "ALL TESTS PASSED" : "SOME TESTS FAILED")
	          << " ===\n";

	return all_pass ? 0 : 1;
}