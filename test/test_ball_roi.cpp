#include "vision/roi_utils.hpp"

#include <cassert>
#include <iostream>
#include <string>

namespace
{

	int failures = 0;

	void check(const std::string& label, bool condition)
	{
		if(!condition)
		{
			std::cerr << "[FAIL] " << label << "\n";
			++failures;
		}
		else
		{
			std::cout << "[PASS] " << label << "\n";
		}
	}

	void test_full_roi_centered()
	{
		cv::Size frame(1280, 640);
		cv::Rect roi =
		    etest::vision::roi_utils::makeFullRoi(frame, 320);
		check("full roi x=0", roi.x == 0);
		check("full roi width=1280", roi.width == 1280);
		check("full roi height=320", roi.height == 320);
		// pipe_center_y=320 → y = 320 - 160 = 160
		check("full roi y=160", roi.y == 160);
		check("full roi within bounds",
		      roi.x >= 0 && roi.y >= 0
		          && roi.x + roi.width <= frame.width
		          && roi.y + roi.height <= frame.height);
	}

	void test_full_roi_top_edge()
	{
		cv::Size frame(1280, 640);
		// pipe_center_y=100 → y = 100-160 = -60 → clamp to 0
		cv::Rect roi =
		    etest::vision::roi_utils::makeFullRoi(frame, 100);
		check("full roi top edge y=0", roi.y == 0);
		check("full roi top edge within bounds",
		      roi.y >= 0 && roi.y + roi.height <= frame.height);
	}

	void test_full_roi_bottom_edge()
	{
		cv::Size frame(1280, 640);
		// pipe_center_y=600 → y = 600-160 = 440, but 440+320=760 > 640
		// → clamp to 640-320=320
		cv::Rect roi =
		    etest::vision::roi_utils::makeFullRoi(frame, 600);
		check("full roi bottom edge y=320", roi.y == 320);
		check("full roi bottom edge within bounds",
		      roi.y >= 0 && roi.y + roi.height <= frame.height);
	}

	void test_center_roi_normal()
	{
		cv::Size frame(1280, 640);
		cv::Rect roi = etest::vision::roi_utils::makeCenterRoi(
		    frame, 640, 320);
		// 640-224=416
		check("center roi x=416", roi.x == 416);
		check("center roi width=448", roi.width == 448);
		check("center roi height=320", roi.height == 320);
		check("center roi y=160", roi.y == 160);
		check("center roi within bounds",
		      roi.x >= 0 && roi.y >= 0
		          && roi.x + roi.width <= frame.width
		          && roi.y + roi.height <= frame.height);
	}

	void test_center_roi_left_edge()
	{
		cv::Size frame(1280, 640);
		// pipe_center_x=100 → x=100-224=-124 → clamp to 0
		cv::Rect roi = etest::vision::roi_utils::makeCenterRoi(
		    frame, 100, 320);
		check("center roi left edge x=0", roi.x == 0);
		check("center roi left edge width=448", roi.width == 448);
		check("center roi left edge within bounds",
		      roi.x >= 0 && roi.y >= 0
		          && roi.x + roi.width <= frame.width
		          && roi.y + roi.height <= frame.height);
	}

	void test_center_roi_right_edge()
	{
		cv::Size frame(1280, 640);
		// pipe_center_x=1200 → x=1200-224=976, clamp: 1280-448=832
		cv::Rect roi = etest::vision::roi_utils::makeCenterRoi(
		    frame, 1200, 320);
		check("center roi right edge x=832", roi.x == 832);
		check("center roi right edge within bounds",
		      roi.x >= 0 && roi.y >= 0
		          && roi.x + roi.width <= frame.width
		          && roi.y + roi.height <= frame.height);
	}

	void test_local_to_global()
	{
		// 中心 ROI 原点 x=416, 检测局部 x=224 → 全局 x=640
		cv::Rect center_roi(416, 160, 448, 320);
		cv::Point2f local(224.0F, 160.0F);
		cv::Point2f global =
		    etest::vision::roi_utils::localToGlobal(
		        local, center_roi);

		check("localToGlobal center x=640",
		      std::abs(global.x - 640.0F) < 1e-6F);
		check("localToGlobal center y=320",
		      std::abs(global.y - 320.0F) < 1e-6F);

		// 完整 ROI 原点 x=0, 检测局部 x=320 → 全局 x=320
		cv::Rect full_roi(0, 160, 1280, 320);
		global = etest::vision::roi_utils::localToGlobal(
		    cv::Point2f(320.0F, 80.0F), full_roi);
		check("localToGlobal full x=320",
		      std::abs(global.x - 320.0F) < 1e-6F);
		check("localToGlobal full y=240",
		      std::abs(global.y - 240.0F) < 1e-6F);
	}

} // namespace

int main()
{
	test_full_roi_centered();
	test_full_roi_top_edge();
	test_full_roi_bottom_edge();
	test_center_roi_normal();
	test_center_roi_left_edge();
	test_center_roi_right_edge();
	test_local_to_global();

	if(failures > 0)
	{
		std::cerr << "\n" << failures << " test(s) FAILED.\n";
		return 1;
	}

	std::cout << "\nAll ROI tests passed.\n";
	return 0;
}