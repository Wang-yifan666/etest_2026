#pragma once

#include "core/config.hpp"

#include <opencv2/core.hpp>
#include <opencv2/dnn.hpp>

#include <string>
#include <vector>

namespace etest::vision
{

	enum class VisionMode
	{
		Preview = 0,
		ColorTarget = 1,
		Line = 2,
		Circle = 3,
		Tag = 4,
		NeuralNetwork = 5
	};

	struct VisionResult
	{
		bool valid = false;
		float x = -1.0F;
		float y = -1.0F;
		float angle_deg = 0.0F;
		float area = 0.0F;
	};

	class VisionProcessor
	{
	public:
		explicit VisionProcessor(VisionConfig config = {});

		VisionResult process(const cv::Mat& frame,
		                     VisionMode mode) noexcept;

		void drawDebugInfo(cv::Mat& frame,
		                   const VisionResult& result) noexcept;

		// 加载 ONNX 模型和类别文件。
		// 返回 true 表示成功。
		bool loadNnModel(const std::string& onnx_path,
		                 const std::string& class_names_path,
		                 double confidence_threshold,
		                 double nms_threshold) noexcept;

		// 对输入帧执行神经网络检测，返回绘制了检测框的帧。
		cv::Mat detectNn(const cv::Mat& frame) noexcept;

		// 神经网络是否已加载。
		bool isNnLoaded() const noexcept;

	private:
		VisionResult detectColorTarget(const cv::Mat& frame);

		VisionConfig config_;
		bool empty_frame_reported_ = false;

		// 神经网络相关。
		cv::dnn::Net nn_net_;
		bool nn_loaded_ = false;
		std::vector<std::string> nn_class_names_;
		double nn_confidence_threshold_ = 0.5;
		double nn_nms_threshold_ = 0.4;
		std::vector<std::string> nn_output_names_;
	};

} // namespace etest::vision
