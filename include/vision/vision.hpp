#pragma once

#include <opencv2/opencv.hpp>

namespace etest::vision
{

	enum class VisionMode
	{
		Preview = 0,
		ColorTarget = 1,

		// 后续扩展
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

		// 根据任务可以表示轮廓面积、目标框面积等
		float area = 0.0F;
	};

	class VisionProcessor
	{
	public:
		VisionResult process(const cv::Mat& frame, VisionMode mode);

		void drawDebugInfo(cv::Mat& frame, const VisionResult& result);

	private:
		VisionResult detectColorTarget(const cv::Mat& frame);
	};

} // namespace etest::vision
