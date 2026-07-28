#pragma once

#include <opencv2/core.hpp>
#include <optional>
#include <vector>

namespace etest::vision::utils
{

	cv::Rect clampRoi(const cv::Rect& roi, const cv::Size& image_size);

	void makeDualRangeHsvMask(const cv::Mat& bgr, cv::Mat& mask,
	                          const cv::Scalar& lower1,
	                          const cv::Scalar& upper1,
	                          const cv::Scalar& lower2,
	                          const cv::Scalar& upper2);

	void cleanBinaryMask(cv::Mat& mask, int kernel_size,
	                     int open_iterations = 1,
	                     int close_iterations = 1);

	std::optional<std::size_t> findLargestContour(
	    const std::vector<std::vector<cv::Point>>& contours,
	    double min_area);

	std::optional<cv::Point2f> contourCenter(
	    const std::vector<cv::Point>& contour);

	double contourCircularity(const std::vector<cv::Point>& contour);

	double contourAngle(const std::vector<cv::Point>& contour);

} // namespace etest::vision::utils