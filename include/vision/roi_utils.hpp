#pragma once

#include <opencv2/core.hpp>

#include "core/config.hpp"

namespace etest::vision::roi_utils
{

	// 推理 ROI 描述
	struct InferenceRoi
	{
		cv::Rect rect;
		cv::Size model_input_size; // 对应网络的输入尺寸
		int pipe_center_x;         // 物理中心 O 的全局像素 x
		int pipe_center_y;         // 物理中心 O 的全局像素 y
	};

	// 构建完整水管 ROI
	// frame_size: 原始帧尺寸（1280×720）
	// roi_width, roi_height: 从 BallNcnnConfig::full_src_* 读取
	// pipe_center_y: 物理中心 O 的 y 坐标
	cv::Rect makeFullRoi(const cv::Size& frame_size, int roi_width,
	                     int roi_height, int pipe_center_y);

	// 构建中心水管 ROI
	// frame_size: 原始帧尺寸（1280×720）
	// roi_width, roi_height: 从 BallNcnnConfig::center_src_* 读取
	// pipe_center_x, pipe_center_y: 物理中心 O 的坐标
	cv::Rect makeCenterRoi(const cv::Size& frame_size, int roi_width,
	                       int roi_height, int pipe_center_x,
	                       int pipe_center_y);

	// ROI 局部像素坐标 → 原图全局像素坐标
	// local: 检测到的点在 ROI 裁剪图中的位置
	// roi: 对应 ROI 在原图中的位置
	inline cv::Point2f localToGlobal(const cv::Point2f& local,
	                                 const cv::Rect& roi)
	{
		return {local.x + static_cast<float>(roi.x),
		        local.y + static_cast<float>(roi.y)};
	}

	// 获取 InferenceRoi 对象
	// width: 1280
	// height: 320 (FULL) 或 320 (CENTER, 448 wide)
	InferenceRoi getFullInferenceRoi(const cv::Size& frame_size,
	                                 const BallNcnnConfig& config);

	InferenceRoi getCenterInferenceRoi(const cv::Size& frame_size,
	                                   const BallNcnnConfig& config);

	// ── 物理坐标标定（Commit 4）──

	// 全局像素 x → 物理毫米（相对固定中心 O）
	// 使用分段线性插值，points 至少需要 2 个点
	double pixelToMm(double pixel_global,
	                 const PipeAxisCalibration& cal);

	// 全局像素 x → 0.1 mm 整数（协议格式）
	int pixelTo0p1mm(double pixel_global,
	                 const PipeAxisCalibration& cal);

	// ── 越界检测 ──

	/// 带越界状态的像素→毫米换算结果。
	struct AxisPositionResult
	{
		bool valid = false;
		bool out_of_range = false;
		double position_mm = 0.0;
	};

	/// 全局像素 x → 物理毫米（带越界检测）。
	/// 像素超出标定点范围时 out_of_range=true, valid=false。
	AxisPositionResult pixelToMmChecked(
	    double pixel_global, const PipeAxisCalibration& cal) noexcept;

} // namespace etest::vision::roi_utils
