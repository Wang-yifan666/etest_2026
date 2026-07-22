#include "vision/vision.hpp"

#include <algorithm>
#include <vector>

namespace etest::vision
{

	VisionResult VisionProcessor::process(const cv::Mat& frame,
	                                      VisionMode mode)
	{
		if(frame.empty())
		{
			return {};
		}

		switch(mode)
		{
		case VisionMode::ColorTarget:
			return detectColorTarget(frame);

		case VisionMode::Preview:
		default:
			return {};
		}
	}

	VisionResult VisionProcessor::detectColorTarget(
	    const cv::Mat& frame)
	{
		VisionResult result;

		cv::Mat hsv;
		cv::cvtColor(frame, hsv, cv::COLOR_BGR2HSV);

		// 红色 HSV 跨越 0°
		cv::Mat mask1;
		cv::Mat mask2;
		cv::Mat mask;

		cv::inRange(hsv, cv::Scalar(0, 100, 80),
		            cv::Scalar(10, 255, 255), mask1);

		cv::inRange(hsv, cv::Scalar(170, 100, 80),
		            cv::Scalar(180, 255, 255), mask2);

		mask = mask1 | mask2;

		// 基础形态学处理
		const cv::Mat kernel = cv::getStructuringElement(
		    cv::MORPH_ELLIPSE, cv::Size(5, 5));

		cv::morphologyEx(mask, mask, cv::MORPH_OPEN, kernel);

		cv::morphologyEx(mask, mask, cv::MORPH_CLOSE, kernel);

		std::vector<std::vector<cv::Point>> contours;

		cv::findContours(mask, contours, cv::RETR_EXTERNAL,
		                 cv::CHAIN_APPROX_SIMPLE);

		if(contours.empty())
		{
			return result;
		}

		auto largest_it = std::max_element(
		    contours.begin(), contours.end(),
		    [](const auto& a, const auto& b) {
			    return cv::contourArea(a) < cv::contourArea(b);
		    });

		const double area = cv::contourArea(*largest_it);

		// 过滤小噪声
		if(area < 300.0)
		{
			return result;
		}

		const cv::Moments moments = cv::moments(*largest_it);

		if(moments.m00 == 0.0)
		{
			return result;
		}

		result.valid = true;

		result.x = static_cast<float>(moments.m10 / moments.m00);

		result.y = static_cast<float>(moments.m01 / moments.m00);

		result.area = static_cast<float>(area);

		const cv::RotatedRect rect = cv::minAreaRect(*largest_it);

		result.angle_deg = rect.angle;

		return result;
	}

	void VisionProcessor::drawDebugInfo(cv::Mat& frame,
	                                    const VisionResult& result)
	{
		// 画图像中心
		const cv::Point image_center(frame.cols / 2, frame.rows / 2);

		cv::drawMarker(frame, image_center, cv::Scalar(255, 0, 0),
		               cv::MARKER_CROSS, 20, 2);

		if(!result.valid)
		{
			cv::putText(frame, "Target: LOST", cv::Point(20, 30),
			            cv::FONT_HERSHEY_SIMPLEX, 0.7,
			            cv::Scalar(0, 0, 255), 2);

			return;
		}

		const cv::Point target(static_cast<int>(result.x),
		                       static_cast<int>(result.y));

		cv::circle(frame, target, 8, cv::Scalar(0, 255, 0), 2);

		cv::line(frame, image_center, target, cv::Scalar(0, 255, 0), 2);

		cv::putText(frame, "Target: FOUND", cv::Point(20, 30),
		            cv::FONT_HERSHEY_SIMPLEX, 0.7,
		            cv::Scalar(0, 255, 0), 2);
	}

} // namespace etest::vision
