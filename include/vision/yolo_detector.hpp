#pragma once

#include "vision/vision.hpp"
#include "vision/yolo_backend.hpp"

#include <opencv2/core.hpp>

#include <memory>
#include <string>
#include <vector>

namespace etest::vision
{

	// YOLO 检测器：持有推理后端 + 公共解码/NMS
	class YoloDetector
	{
	public:
		YoloDetector() = default;

		// 初始化后端
		bool initialize(std::unique_ptr<IYoloBackend> backend,
		                const YoloBackendConfig& backend_config,
		                std::vector<std::string> class_names,
		                float confidence_threshold, float nms_threshold,
		                std::string& error) noexcept;

		// 执行完整推理：预处理 → 前向 → 解码 → NMS
		std::vector<YoloDetection> infer(
		    const cv::Mat& frame,
		    YoloTiming* timing = nullptr) noexcept;

		bool ready() const noexcept;
		const char* backendName() const noexcept;

	private:
		// 公共解码：原始输出 → boxes/confidences/class_ids，再 NMS
		std::vector<YoloDetection> decode(const YoloRawOutput& raw,
		                                  const cv::Size& original_size,
		                                  YoloTiming* timing) const;

		std::unique_ptr<IYoloBackend> backend_;
		YoloBackendConfig backend_config_;
		std::vector<std::string> class_names_;

		float confidence_threshold_ = 0.45F;
		float nms_threshold_ = 0.45F;

		bool first_inference_ = true;
	};

} // namespace etest::vision