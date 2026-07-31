#include "vision/roi_utils.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <stdexcept>

namespace etest::vision::roi_utils
{

	cv::Rect makeFullRoi(const cv::Size& frame_size,
	                     int roi_width, int roi_height,
	                     int pipe_center_y)
	{
		const int y = std::clamp(
		    pipe_center_y - roi_height / 2,
		    0,
		    std::max(0, frame_size.height - roi_height));

		return {0, y, roi_width, roi_height};
	}

	cv::Rect makeCenterRoi(const cv::Size& frame_size,
	                       int roi_width, int roi_height,
	                       int pipe_center_x,
	                       int pipe_center_y)
	{
		const int x = std::clamp(
		    pipe_center_x - roi_width / 2,
		    0,
		    std::max(0, frame_size.width - roi_width));

		const int y = std::clamp(
		    pipe_center_y - roi_height / 2,
		    0,
		    std::max(0, frame_size.height - roi_height));

		return {x, y, roi_width, roi_height};
	}

	InferenceRoi getFullInferenceRoi(const cv::Size& frame_size,
	                                 const BallNcnnConfig& config)
	{
		InferenceRoi roi;

		if(config.roi_location_mode == "topleft")
		{
			// 直接使用左上角坐标
			roi.rect = cv::Rect(config.full_roi_x,
			                    config.full_roi_y,
			                    config.full_src_width,
			                    config.full_src_height);
		}
		else
		{
			// 旧逻辑：通过中心点反算
			roi.rect = makeFullRoi(frame_size,
			                       config.full_src_width,
			                       config.full_src_height,
			                       config.pipe_center_y);
		}

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

		if(config.roi_location_mode == "topleft")
		{
			roi.rect = cv::Rect(config.center_roi_x,
			                    config.center_roi_y,
			                    config.center_src_width,
			                    config.center_src_height);
		}
		else
		{
			roi.rect = makeCenterRoi(frame_size,
			                         config.center_src_width,
			                         config.center_src_height,
			                         config.pipe_center_x,
			                         config.pipe_center_y);
		}

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

		if(cal.image_right_sign != -1
		   && cal.image_right_sign != 1)
		{
			return 0.0;
		}

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

				// 配置应在加载阶段被拒绝，此处为防御性兜底
				if(p1.pixel <= p0.pixel)
				{
					return 0.0;
				}

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