#pragma once

#include "core/config.hpp"
#include "state/task_session.hpp"
#include "vision/roi_utils.hpp"
#include "vision/yolo_detector.hpp"

#include <memory>
#include <string>

namespace etest::vision
{

	// 双模型管理器：持有完整模型（640×160）和中心模型（224×160）
	class BallNcnnManager
	{
	public:
		BallNcnnManager() = default;

		// 加载双模型 + 各预热 3 帧
		// 返回 false 时 error 填充具体原因
		bool initialize(const BallNcnnConfig& config,
		                std::string& error) noexcept;

		// 按跟踪模式执行推理
		// raw_frame: 原始 1280×640 BGR 帧
		// tracking_mode: 当前跟踪模式
		// 返回 BallMeasurement，包含局部和全局坐标
		BallMeasurement process(const cv::Mat& raw_frame,
		                        TrackingMode tracking_mode,
		                        YoloTiming* timing = nullptr) noexcept;

		// 重置跟踪状态（切换任务时调用）
		void resetTracking() noexcept;

		// 模型就绪状态
		bool fullModelReady() const noexcept;
		bool centerModelReady() const noexcept;

	private:
		BallNcnnConfig config_;

		std::unique_ptr<YoloDetector> full_detector_;
		std::unique_ptr<YoloDetector> center_detector_;

		bool full_ready_ = false;
		bool center_ready_ = false;

		cv::Point2f last_global_center_{-1.0F, -1.0F};
		bool tracking_initialized_ = false;

		// 按给定检测器执行推理（内部使用 ROI）
		BallMeasurement detectWithRoi(
		    const cv::Mat& raw_frame,
		    const roi_utils::InferenceRoi& roi,
		    YoloDetector& detector,
		    YoloTiming* timing) noexcept;

		// 创建单个检测器
		std::unique_ptr<YoloDetector> createDetector(
		    const std::string& param_path,
		    int input_width, int input_height,
		    int num_threads, bool fp16_storage, bool fp16_arithmetic,
		    std::string& error) const noexcept;
	};

} // namespace etest::vision