#include "vision/roi_utils.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace etest::vision::roi_utils
{

	cv::Rect makeFullRoi(const cv::Size& frame_size,
	                     int pipe_center_y)
	{
		constexpr int kRoiWidth = 1280;
		constexpr int kRoiHeight = 320;

		const int y = std::clamp(
		    pipe_center_y - kRoiHeight / 2,
		    0,
		    std::max(0, frame_size.height - kRoiHeight));

		return {0, y, kRoiWidth, kRoiHeight};
	}

	cv::Rect makeCenterRoi(const cv::Size& frame_size,
	                       int pipe_center_x,
	                       int pipe_center_y)
	{
		constexpr int kRoiWidth = 448;
		constexpr int kRoiHeight = 320;

		const int x = std::clamp(
		    pipe_center_x - kRoiWidth / 2,
		    0,
		    std::max(0, frame_size.width - kRoiWidth));

		const int y = std::clamp(
		    pipe_center_y - kRoiHeight / 2,
		    0,
		    std::max(0, frame_size.height - kRoiHeight));

		return {x, y, kRoiWidth, kRoiHeight};
	}

	InferenceRoi getFullInferenceRoi(const cv::Size& frame_size,
	                                 const BallNcnnConfig& config)
	{
		InferenceRoi roi;
		roi.rect = makeFullRoi(frame_size, config.pipe_center_y);
		roi.model_input_size = cv::Size(config.full_input_width,
		                                config.full_input_height);
		roi.pipe_center_x = config.pipe_center_x;
		roi.pipe_center_y = config.pipe_center_y;
		return roi;
	}

	InferenceRoi getCenterInferenceRoi(
	    const cv::Size& frame_size,
	    const BallNcnnConfig& config)
	{
		InferenceRoi roi;
		roi.rect = makeCenterRoi(frame_size, config.pipe_center_x,
		                         config.pipe_center_y);
		roi.model_input_size = cv::Size(config.center_input_width,
		                                config.center_input_height);
		roi.pipe_center_x = config.pipe_center_x;
		roi.pipe_center_y = config.pipe_center_y;
		return roi;
	}

	// ── Commit 4：物理坐标标定 ──

	double pixelToMm(double pixel_global,
	                 const PipeAxisCalibration& cal)
	{
		const auto& pts = cal.points;
		if(pts.size() < 2)
			return 0.0;

		if(pixel_global <= pts.front().pixel)
			return pts.front().position_mm * cal.image_right_sign;

		if(pixel_global >= pts.back().pixel)
			return pts.back().position_mm * cal.image_right_sign;

		for(std::size_t i = 1; i < pts.size(); ++i)
		{
			if(pixel_global <= pts[i].pixel)
			{
				const auto& p0 = pts[i - 1];
				const auto& p1 = pts[i];
				const double ratio =
				    (pixel_global - p0.pixel)
				    / (p1.pixel - p0.pixel);
				return (p0.position_mm
				        + ratio
				            * (p1.position_mm
				               - p0.position_mm))
				    * cal.image_right_sign;
			}
		}

		return pts.back().position_mm * cal.image_right_sign;
	}

	int pixelTo0p1mm(double pixel_global,
	                 const PipeAxisCalibration& cal)
	{
		double mm = pixelToMm(pixel_global, cal);
		return static_cast<int>(std::lround(mm * 10.0));
	}

} // namespace etest::vision::roi_utils