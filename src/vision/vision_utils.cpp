#include "vision/vision_utils.hpp"

#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <cmath>

namespace etest::vision::utils
{

cv::Rect clampRoi(const cv::Rect& roi,
                  const cv::Size& image_size)
{
	if(image_size.width <= 0 || image_size.height <= 0)
	{
		return cv::Rect{};
	}

	int x = std::max(0, roi.x);
	int y = std::max(0, roi.y);

	int w = roi.width;
	int h = roi.height;

	if(x + w > image_size.width)
	{
		w = image_size.width - x;
	}

	if(y + h > image_size.height)
	{
		h = image_size.height - y;
	}

	if(w <= 0 || h <= 0)
	{
		return cv::Rect{};
	}

	return cv::Rect{x, y, w, h};
}

void makeDualRangeHsvMask(
    const cv::Mat& bgr,
    cv::Mat& mask,
    const cv::Scalar& lower1,
    const cv::Scalar& upper1,
    const cv::Scalar& lower2,
    const cv::Scalar& upper2)
{
	if(bgr.empty())
	{
		mask = cv::Mat{};
		return;
	}

	cv::Mat hsv;
	cv::cvtColor(bgr, hsv, cv::COLOR_BGR2HSV);

	cv::Mat mask1;
	cv::Mat mask2;

	cv::inRange(hsv, lower1, upper1, mask1);
	cv::inRange(hsv, lower2, upper2, mask2);

	mask = mask1 | mask2;
}

void cleanBinaryMask(cv::Mat& mask,
                     int kernel_size,
                     int open_iterations,
                     int close_iterations)
{
	if(mask.empty() || kernel_size <= 0)
	{
		return;
	}

	const cv::Mat kernel = cv::getStructuringElement(
	    cv::MORPH_ELLIPSE,
	    cv::Size(kernel_size, kernel_size));

	if(open_iterations > 0)
	{
		cv::morphologyEx(mask, mask, cv::MORPH_OPEN, kernel,
		                 cv::Point(-1, -1), open_iterations);
	}

	if(close_iterations > 0)
	{
		cv::morphologyEx(mask, mask, cv::MORPH_CLOSE, kernel,
		                 cv::Point(-1, -1), close_iterations);
	}
}

std::optional<std::size_t> findLargestContour(
    const std::vector<std::vector<cv::Point>>& contours,
    double min_area)
{
	if(contours.empty())
	{
		return std::nullopt;
	}

	std::size_t best_index = 0;
	double best_area = 0.0;
	bool found = false;

	for(std::size_t i = 0; i < contours.size(); ++i)
	{
		const double area = cv::contourArea(contours[i]);

		if(area >= min_area && area > best_area)
		{
			best_area = area;
			best_index = i;
			found = true;
		}
	}

	if(!found)
	{
		return std::nullopt;
	}

	return best_index;
}

std::optional<cv::Point2f> contourCenter(
    const std::vector<cv::Point>& contour)
{
	if(contour.empty())
	{
		return std::nullopt;
	}

	const cv::Moments moments = cv::moments(contour);

	if(moments.m00 == 0.0)
	{
		return std::nullopt;
	}

	return cv::Point2f{
	    static_cast<float>(moments.m10 / moments.m00),
	    static_cast<float>(moments.m01 / moments.m00)};
}

double contourCircularity(
    const std::vector<cv::Point>& contour)
{
	if(contour.size() < 3)
	{
		return 0.0;
	}

	const double area = cv::contourArea(contour);

	if(area <= 0.0)
	{
		return 0.0;
	}

	const double perimeter = cv::arcLength(contour, true);

	if(perimeter <= 0.0)
	{
		return 0.0;
	}

	// 圆度 = 4π * area / perimeter²，正圆为 1
	return (4.0 * M_PI * area) / (perimeter * perimeter);
}

double contourAngle(
    const std::vector<cv::Point>& contour)
{
	if(contour.size() < 3)
	{
		return 0.0;
	}

	const cv::RotatedRect rectangle = cv::minAreaRect(contour);

	return static_cast<double>(rectangle.angle);
}

} // namespace etest::vision::utils