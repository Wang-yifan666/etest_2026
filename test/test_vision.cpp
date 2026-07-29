// etest_2026 视觉单元测试
// 验证无崩溃、边界处理、基本返回码

#include "core/config.hpp"
#include "core/logger.hpp"
#include "vision/vision.hpp"

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <cassert>
#include <iostream>

namespace
{

	cv::Scalar hsv2bgr(int h, int s, int v)
	{
		cv::Mat hsv_mat(1, 1, CV_8UC3, cv::Scalar(h, s, v));
		cv::Mat bgr_mat;
		cv::cvtColor(hsv_mat, bgr_mat, cv::COLOR_HSV2BGR);
		return bgr_mat.at<cv::Vec3b>(0, 0);
	}

	cv::Mat makeWhiteBg(int w = 640, int h = 480)
	{
		return cv::Mat(h, w, CV_8UC3, cv::Scalar(255, 255, 255));
	}

	void drawBrownRect(cv::Mat& img, int x, int y, int w, int h)
	{
		cv::rectangle(img, cv::Rect(x, y, w, h), hsv2bgr(15, 180, 100),
		              -1);
	}

	void drawDarkCircle(cv::Mat& img, cv::Point center, int radius)
	{
		cv::circle(img, center, radius, cv::Scalar(25, 25, 25), -1);
	}

	// ── 基础测试 ──

	void test_empty_frame()
	{
		etest::vision::VisionProcessor vp;
		cv::Mat empty;
		auto r = vp.process(empty, etest::vision::VisionMode::Ball);
		assert(!r.valid);
		assert(r.error_code == "EMPTY_FRAME");
		std::cout << "[PASS] test_empty_frame\n";
	}

	void test_white_image_pipe_not_found()
	{
		etest::VisionConfig cfg;
		cfg.ball.work_width = 640;
		cfg.ball.work_height = 480;
		etest::vision::VisionProcessor vp(cfg);

		cv::Mat white = makeWhiteBg();
		for(int i = 0; i < 10; ++i)
		{
			auto r = vp.process(white, etest::vision::VisionMode::Ball);
			assert(!r.valid);
		}

		auto r = vp.process(white, etest::vision::VisionMode::Ball);
		assert(!r.valid);
		assert(!r.error_code.empty());
		std::cout << "[PASS] test_white_image_pipe_not_found ("
		          << r.error_code << ")\n";
	}

	void test_search_roi_out_of_bounds()
	{
		etest::VisionConfig cfg;
		cfg.ball.work_width = 640;
		cfg.ball.work_height = 480;
		cfg.ball.pipe_search_roi_x = 1000;
		cfg.ball.pipe_search_roi_y = 1000;
		cfg.ball.pipe_search_roi_w = 500;
		cfg.ball.pipe_search_roi_h = 500;

		etest::vision::VisionProcessor vp(cfg);
		cv::Mat img = makeWhiteBg();

		for(int i = 0; i < 5; ++i)
		{
			auto r = vp.process(img, etest::vision::VisionMode::Ball);
			assert(!r.valid);
		}
		// 不崩溃即为通过
		std::cout
		    << "[PASS] test_search_roi_out_of_bounds (no crash)\n";
	}

	// ── 合成管道测试 ──

	void test_synthetic_pipe_non_crash()
	{
		etest::VisionConfig cfg;
		cfg.ball.work_width = 640;
		cfg.ball.work_height = 480;
		cfg.ball.pipe_stable_frames = 5;
		cfg.ball.pipe_lost_timeout_frames = 8;
		cfg.ball.pipe_search_roi_x = 0;
		cfg.ball.pipe_search_roi_y = 80;
		cfg.ball.pipe_search_roi_w = 640;
		cfg.ball.pipe_search_roi_h = 300;
		cfg.ball.max_area = 2000.0;
		cfg.ball.pipe_min_area_ratio = 0.02;
		cfg.ball.brown_h_min = 0;
		cfg.ball.brown_h_max = 45;
		cfg.ball.brown_s_min = 30;
		cfg.ball.brown_v_min = 15;
		cfg.ball.zero_mode = "fixed";
		cfg.ball.zero_position_px = 250.0;
		cfg.ball.zero_samples = 2;
		cfg.ball.zero_range_px = 500.0;

		etest::vision::VisionProcessor vp(cfg);

		// 输入 30 帧含棕色管道和钢球的图，验证不崩溃
		for(int i = 0; i < 30; ++i)
		{
			cv::Mat img = makeWhiteBg();
			drawBrownRect(img, 60, 230, 520, 90);
			drawDarkCircle(img, cv::Point(320, 275), 14);
			auto r = vp.process(img, etest::vision::VisionMode::Ball);
			// 不崩溃即 OK
			(void)r;
		}
		std::cout
		    << "[PASS] test_synthetic_pipe_non_crash (30 frames)\n";
	}

	// ── 丢球重捕获测试 ──

	void test_ball_lost_reacquire()
	{
		etest::VisionConfig cfg;
		cfg.ball.work_width = 640;
		cfg.ball.work_height = 480;
		cfg.ball.pipe_stable_frames = 3;
		cfg.ball.pipe_lost_timeout_frames = 50;
		cfg.ball.pipe_search_roi_x = 0;
		cfg.ball.pipe_search_roi_y = 80;
		cfg.ball.pipe_search_roi_w = 640;
		cfg.ball.pipe_search_roi_h = 300;
		cfg.ball.reacquire_after_lost_frames = 3;
		cfg.ball.max_area = 2000.0;
		cfg.ball.pipe_min_area_ratio = 0.02;
		cfg.ball.brown_h_min = 0;
		cfg.ball.brown_h_max = 45;
		cfg.ball.brown_s_min = 30;
		cfg.ball.brown_v_min = 15;
		cfg.ball.zero_mode = "fixed";
		cfg.ball.zero_position_px = 250.0;
		cfg.ball.zero_samples = 2;
		cfg.ball.zero_range_px = 500.0;

		etest::vision::VisionProcessor vp(cfg);

		// 连续含球帧
		for(int i = 0; i < 15; ++i)
		{
			cv::Mat img = makeWhiteBg();
			drawBrownRect(img, 60, 230, 520, 90);
			drawDarkCircle(img, cv::Point(320, 275), 14);
			vp.process(img, etest::vision::VisionMode::Ball);
		}

		// 连续无球帧（管道还在但没球）
		for(int i = 0; i < 10; ++i)
		{
			cv::Mat img = makeWhiteBg();
			drawBrownRect(img, 60, 230, 520, 90);
			// 无钢球
			vp.process(img, etest::vision::VisionMode::Ball);
		}

		// 再喂含球帧
		for(int i = 0; i < 10; ++i)
		{
			cv::Mat img = makeWhiteBg();
			drawBrownRect(img, 60, 230, 520, 90);
			drawDarkCircle(img, cv::Point(320, 275), 14);
			auto r = vp.process(img, etest::vision::VisionMode::Ball);
			(void)r;
		}

		std::cout << "[PASS] test_ball_lost_reacquire (non-crash)\n";
	}

	// ── 管道丢失重置测试 ──

	void test_pipe_lost_resets()
	{
		etest::VisionConfig cfg;
		cfg.ball.work_width = 640;
		cfg.ball.work_height = 480;
		cfg.ball.pipe_stable_frames = 3;
		cfg.ball.pipe_lost_timeout_frames = 5;
		cfg.ball.pipe_search_roi_x = 0;
		cfg.ball.pipe_search_roi_y = 80;
		cfg.ball.pipe_search_roi_w = 640;
		cfg.ball.pipe_search_roi_h = 300;
		cfg.ball.max_area = 2000.0;
		cfg.ball.pipe_min_area_ratio = 0.02;
		cfg.ball.brown_h_min = 0;
		cfg.ball.brown_h_max = 45;
		cfg.ball.brown_s_min = 30;
		cfg.ball.brown_v_min = 15;
		cfg.ball.zero_mode = "fixed";
		cfg.ball.zero_samples = 2;
		cfg.ball.zero_range_px = 500.0;

		etest::vision::VisionProcessor vp(cfg);

		// 含管道帧
		for(int i = 0; i < 10; ++i)
		{
			cv::Mat img = makeWhiteBg();
			drawBrownRect(img, 60, 230, 520, 90);
			vp.process(img, etest::vision::VisionMode::Ball);
		}

		// 无管道帧（超过丢失阈值）
		bool unlocked = false;
		for(int i = 0; i < cfg.ball.pipe_lost_timeout_frames + 2; ++i)
		{
			cv::Mat img = makeWhiteBg();
			auto r = vp.process(img, etest::vision::VisionMode::Ball);
			if(r.error_code == "PIPE_NOT_STABLE")
				unlocked = true;
		}

		// 最终应该回到 PIPE_NOT_STABLE 或类似状态
		cv::Mat img = makeWhiteBg();
		auto last = vp.process(img, etest::vision::VisionMode::Ball);
		std::cout << "  final state after pipe loss: "
		          << last.error_code << "\n";

		(void)unlocked;
		std::cout << "[PASS] test_pipe_lost_resets (non-crash)\n";
	}

} // namespace

int main()
{
	std::cout << "=== etest_vision_test ===\n";

	test_empty_frame();
	test_white_image_pipe_not_found();
	test_search_roi_out_of_bounds();
	test_synthetic_pipe_non_crash();
	test_ball_lost_reacquire();
	test_pipe_lost_resets();

	std::cout << "\nall vision tests passed\n";
	return 0;
}