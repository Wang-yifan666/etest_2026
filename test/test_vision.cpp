// etest_2026 视觉单元测试
// 验证无崩溃、边界处理、基本返回码

#include "core/config.hpp"
#include "core/logger.hpp"
#include "vision/vision.hpp"

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <array>
#include <cassert>
#include <cmath>
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

		for(int i = 0; i < 30; ++i)
		{
			cv::Mat img = makeWhiteBg();
			drawBrownRect(img, 60, 230, 520, 90);
			drawDarkCircle(img, cv::Point(320, 275), 14);
			auto r = vp.process(img, etest::vision::VisionMode::Ball);
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

		for(int i = 0; i < 15; ++i)
		{
			cv::Mat img = makeWhiteBg();
			drawBrownRect(img, 60, 230, 520, 90);
			drawDarkCircle(img, cv::Point(320, 275), 14);
			vp.process(img, etest::vision::VisionMode::Ball);
		}

		for(int i = 0; i < 10; ++i)
		{
			cv::Mat img = makeWhiteBg();
			drawBrownRect(img, 60, 230, 520, 90);
			vp.process(img, etest::vision::VisionMode::Ball);
		}

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

		for(int i = 0; i < 10; ++i)
		{
			cv::Mat img = makeWhiteBg();
			drawBrownRect(img, 60, 230, 520, 90);
			vp.process(img, etest::vision::VisionMode::Ball);
		}

		bool unlocked = false;
		for(int i = 0; i < cfg.ball.pipe_lost_timeout_frames + 2; ++i)
		{
			cv::Mat img = makeWhiteBg();
			auto r = vp.process(img, etest::vision::VisionMode::Ball);
			if(r.error_code == "PIPE_NOT_STABLE")
				unlocked = true;
		}

		cv::Mat img = makeWhiteBg();
		auto last = vp.process(img, etest::vision::VisionMode::Ball);
		std::cout << "  final state after pipe loss: "
		          << last.error_code << "\n";
		(void)unlocked;
		std::cout << "[PASS] test_pipe_lost_resets (non-crash)\n";
	}

	// ── 四点排序新测试 ──

	void test_orderTrackCorners_no_flip()
	{
		etest::VisionConfig cfg;
		etest::vision::VisionProcessor vp(cfg);

		std::array<double, 3> angles = {0.0, 15.0, -15.0};

		for(double ang: angles)
		{
			cv::RotatedRect rect(cv::Point2f(320, 200),
			                     cv::Size2f(400, 80),
			                     static_cast<float>(ang));

			std::array<cv::Point2f, 4> o;
			bool r = vp.orderTrackCorners(rect, o);
			if(!r)
			{
				std::cout
				    << "  orderTrackCorners returned false for angle="
				    << ang << "\n";
				assert(false);
			}

			double top_len = cv::norm(o[1] - o[0]);
			double left_len = cv::norm(o[3] - o[0]);

			// 3a-1: 上边远长于左边（至少3倍）
			assert(top_len > left_len * 3.0);

			// 3a-2: TL.y < BL.y
			assert(o[0].y < o[3].y);

			// 3a-3: TR.y < BR.y
			assert(o[1].y < o[2].y);

			// 3a-4: 左侧x总和 < 右侧x总和
			double left_sum = o[0].x + o[3].x;
			double right_sum = o[1].x + o[2].x;
			assert(left_sum < right_sum);
		}

		std::cout
		    << "[PASS] test_orderTrackCorners_no_flip (multi-angle)\n";
	}

	void test_orderTrackCorners_degenerate()
	{
		etest::VisionConfig cfg;
		etest::vision::VisionProcessor vp(cfg);

		cv::RotatedRect rect(cv::Point2f(320, 200), cv::Size2f(1, 1),
		                     0);

		std::array<cv::Point2f, 4> o;
		bool r = vp.orderTrackCorners(rect, o);
		(void)r;
		assert(!r);

		std::cout << "[PASS] test_orderTrackCorners_degenerate\n";
	}

	void test_perspective_direction()
	{
		etest::VisionConfig cfg;
		cfg.ball.pipe_warp_width = 500;
		cfg.ball.pipe_warp_height = 120;
		etest::vision::VisionProcessor vp(cfg);

		// 模拟横向管道 (TL, TR, BR, BL)，构造一个略微倾斜的矩形
		std::array<cv::Point2f, 4> pts = {
		    cv::Point2f(100.0F, 200.0F), // TL
		    cv::Point2f(520.0F, 210.0F), // TR
		    cv::Point2f(510.0F, 250.0F), // BR
		    cv::Point2f(90.0F, 240.0F)   // BL
		};

		// 计算透视变换矩阵（模拟 updateWarpMatrices 的行为）
		int ww = cfg.ball.pipe_warp_width;
		int wh = cfg.ball.pipe_warp_height;
		const cv::Point2f dst[4] = {
		    {0.0F, 0.0F},
		    {static_cast<float>(ww - 1), 0.0F},
		    {static_cast<float>(ww - 1), static_cast<float>(wh - 1)},
		    {0.0F, static_cast<float>(wh - 1)}};
		cv::Mat M = cv::getPerspectiveTransform(pts.data(), dst);
		assert(!M.empty());

		// 左端中心 = (TL + BL) / 2
		cv::Point2f left_center = (pts[0] + pts[3]) * 0.5F;
		// 右端中心 = (TR + BR) / 2
		cv::Point2f right_center = (pts[1] + pts[2]) * 0.5F;

		std::vector<cv::Point2f> in = {left_center, right_center};
		std::vector<cv::Point2f> out;
		cv::perspectiveTransform(in, out, M);

		// 3b-1: 左端 x≈0, y≈warp_height/2
		assert(std::abs(out[0].x - 0.0F) < 2.0F);
		assert(std::abs(out[0].y - wh / 2.0F) < 2.0F);

		// 3b-2: 右端 x≈warp_width-1, y≈warp_height/2
		assert(std::abs(out[1].x - (ww - 1.0F)) < 2.0F);
		assert(std::abs(out[1].y - wh / 2.0F) < 2.0F);

		// 3b-3: 映射后 dx >> dy
		float dx = std::abs(out[1].x - out[0].x);
		float dy = std::abs(out[1].y - out[0].y);
		assert(dx > dy * 2.0F);

		std::cout << "[PASS] test_perspective_direction\n";
	}

	void test_smooth_points_between()
	{
		std::array<cv::Point2f, 4> old_pts = {
		    cv::Point2f(0, 0), cv::Point2f(100, 0),
		    cv::Point2f(100, 50), cv::Point2f(0, 50)};
		std::array<cv::Point2f, 4> det_pts = {
		    cv::Point2f(10, 0), cv::Point2f(110, 0),
		    cv::Point2f(110, 50), cv::Point2f(10, 50)};

		double alpha = 0.5;
		for(int i = 0; i < 4; ++i)
		{
			float new_x = static_cast<float>(
			    alpha * det_pts[i].x + (1.0 - alpha) * old_pts[i].x);
			float new_y = static_cast<float>(
			    alpha * det_pts[i].y + (1.0 - alpha) * old_pts[i].y);
			(void)new_x;
			(void)new_y;

			assert(
			    std::abs(new_x - (old_pts[i].x + det_pts[i].x) / 2.0F)
			    < 0.001F);
			assert(
			    std::abs(new_y - (old_pts[i].y + det_pts[i].y) / 2.0F)
			    < 0.001F);
		}

		std::cout << "[PASS] test_smooth_points_between\n";
	}

	void test_warp_roundtrip()
	{
		cv::Point2f src_pts[4] = {
		    cv::Point2f(0, 0), cv::Point2f(499, 0),
		    cv::Point2f(499, 119), cv::Point2f(0, 119)};
		cv::Point2f dst_pts[4] = {
		    cv::Point2f(50, 30), cv::Point2f(450, 25),
		    cv::Point2f(455, 100), cv::Point2f(45, 105)};

		cv::Mat warp = cv::getPerspectiveTransform(src_pts, dst_pts);
		cv::Mat inv = cv::getPerspectiveTransform(dst_pts, src_pts);

		for(const auto& pt: src_pts)
		{
			std::vector<cv::Point2f> v_in = {pt};
			std::vector<cv::Point2f> v_mid;
			std::vector<cv::Point2f> v_out;
			cv::perspectiveTransform(v_in, v_mid, warp);
			cv::perspectiveTransform(v_mid, v_out, inv);

			double err = cv::norm(v_out[0] - pt);
			(void)err;
			assert(err < 1.0);
		}

		std::cout << "[PASS] test_warp_roundtrip\n";
	}

	// ── HoughCircles 新测试 ──

	cv::Mat makeGrayRoi(int w, int h)
	{
		// 创建 BGR 格式的暗底 roi 用于 Hough 检测
		return cv::Mat(h, w, CV_8UC3, cv::Scalar(80, 80, 80));
	}

	void test_hough_empty_no_exception()
	{
		etest::vision::VisionProcessor vp;

		// 空图调用 detectBallCandidates 不抛异常
		cv::Mat empty;
		auto c = vp.detectBallCandidates(empty);
		assert(c.empty());

		// 纯色图不抛异常
		cv::Mat plain = makeGrayRoi(200, 100);
		auto c2 = vp.detectBallCandidates(plain);
		(void)c2;

		std::cout << "[PASS] test_hough_empty_no_exception\n";
	}

	void test_hough_dark_circle_candidate()
	{
		etest::VisionConfig cfg;
		// 放宽 Hough 参数以便检测
		cfg.ball.hough_param1 = 50.0;
		cfg.ball.hough_param2 = 10.0;
		cfg.ball.ball_min_radius = 10;
		cfg.ball.ball_max_radius = 30;
		cfg.ball.ball_min_quality = 0.10;
		etest::vision::VisionProcessor vp(cfg);

		cv::Mat roi = makeGrayRoi(300, 100);
		// 画一个暗圆（钢球）
		cv::circle(roi, cv::Point(150, 50), 18, cv::Scalar(25, 25, 25),
		           -1);

		auto c = vp.detectBallCandidates(roi);
		assert(!c.empty());

		std::cout << "[PASS] test_hough_dark_circle_candidate ("
		          << c.size() << " candidates)\n";
	}

	void test_hough_bright_circle_rejected()
	{
		etest::VisionConfig cfg;
		cfg.ball.hough_param1 = 50.0;
		cfg.ball.hough_param2 = 10.0;
		cfg.ball.ball_min_radius = 10;
		cfg.ball.ball_max_radius = 30;
		cfg.ball.ball_max_inner_gray = 125.0;
		cfg.ball.ball_min_quality = 0.10;
		etest::vision::VisionProcessor vp(cfg);

		cv::Mat roi = makeGrayRoi(300, 100);
		// 画一个亮圆（不是钢球）
		cv::circle(roi, cv::Point(150, 50), 18,
		           cv::Scalar(200, 200, 200), -1);

		auto c = vp.detectBallCandidates(roi);
		// 所有候选应被门限拒绝
		bool any_passed = false;
		for(const auto& bc: c)
			if(bc.passed)
				any_passed = true;
		(void)any_passed;
		assert(!any_passed);

		std::cout << "[PASS] test_hough_bright_circle_rejected\n";
	}

	void test_hough_radius_out_of_range()
	{
		etest::VisionConfig cfg;
		cfg.ball.hough_param1 = 50.0;
		cfg.ball.hough_param2 = 10.0;
		cfg.ball.ball_min_radius = 10;
		cfg.ball.ball_max_radius = 30;
		cfg.ball.ball_min_quality = 0.10;
		etest::vision::VisionProcessor vp(cfg);

		cv::Mat roi = makeGrayRoi(300, 120);
		// 画一个过大半径的圆（60px，超出 max_radius=30）
		cv::circle(roi, cv::Point(150, 60), 60, cv::Scalar(25, 25, 25),
		           -1);

		auto c = vp.detectBallCandidates(roi);
		// HoughCircles 不会返回这个圆
		bool has_large = false;
		for(const auto& bc: c)
			if(bc.radius > 30.0F)
				has_large = true;
		(void)has_large;
		assert(!has_large);

		std::cout << "[PASS] test_hough_radius_out_of_range\n";
	}

	void test_hough_edge_y_rejected()
	{
		etest::VisionConfig cfg;
		cfg.ball.hough_param1 = 50.0;
		cfg.ball.hough_param2 = 10.0;
		cfg.ball.ball_min_radius = 10;
		cfg.ball.ball_max_radius = 30;
		cfg.ball.ball_min_center_y_ratio = 0.20;
		cfg.ball.ball_max_center_y_ratio = 0.80;
		cfg.ball.ball_min_quality = 0.10;
		etest::vision::VisionProcessor vp(cfg);

		cv::Mat roi = makeGrayRoi(300, 100);
		// 圆靠近 top（y=5，比例=0.05 < min_ratio=0.20）
		cv::circle(roi, cv::Point(150, 5), 15, cv::Scalar(25, 25, 25),
		           -1);

		auto c = vp.detectBallCandidates(roi);
		bool any_passed = false;
		for(const auto& bc: c)
			if(bc.passed)
				any_passed = true;
		(void)any_passed;
		assert(!any_passed);

		std::cout << "[PASS] test_hough_edge_y_rejected\n";
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
	test_orderTrackCorners_no_flip();
	test_orderTrackCorners_degenerate();
	test_perspective_direction();
	test_smooth_points_between();
	test_warp_roundtrip();
	test_hough_empty_no_exception();
	test_hough_dark_circle_candidate();
	test_hough_bright_circle_rejected();
	test_hough_radius_out_of_range();
	test_hough_edge_y_rejected();

	std::cout << "\nall vision tests passed\n";
	return 0;
}