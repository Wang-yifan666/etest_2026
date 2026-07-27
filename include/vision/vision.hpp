#pragma once

#include "core/config.hpp"

#include <opencv2/core.hpp>
#include <opencv2/dnn.hpp>

#include <cstdint>
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
	double x = 0.0;
	double y = 0.0;
	double angle = 0.0;
	double distance = 0.0;
	double confidence = 0.0;
	std::uint64_t frame_id = 0;
	std::int64_t timestamp_ms = 0;
	std::string target_type;  // "RED_TARGET" etc.
	std::string error_code;   // "" if valid, else reason
};

// 单次神经网络检测中识别出的一个物品。
struct DetectionInfo
{
	std::string class_name;
	float confidence = 0.0F;
	int x1 = 0;
	int y1 = 0;
	int x2 = 0;
	int y2 = 0;
	int x3 = 0;
	int y3 = 0;
	int x4 = 0;
	int y4 = 0;
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
	bool loadNnModel(const std::string& onnx_path,
	                 const std::string& class_names_path,
	                 double confidence_threshold,
	                 double nms_threshold) noexcept;

	// 对输入帧执行神经网络检测，返回绘制了检测框的帧。
	cv::Mat detectNn(const cv::Mat& frame) noexcept;

	// 返回最近一次 detectNn() 检测到的物品列表。
	const std::vector<DetectionInfo>& getLastDetections()
	    const noexcept;

	// 神经网络是否已加载。
	bool isNnLoaded() const noexcept;

private:
	VisionResult detectColorTarget(const cv::Mat& frame);

	VisionConfig config_;
	bool empty_frame_reported_ = false;
	std::uint64_t frame_id_counter_ = 0;

	// 神经网络相关。
	cv::dnn::Net nn_net_;
	bool nn_loaded_ = false;
	std::vector<std::string> nn_class_names_;
	double nn_confidence_threshold_ = 0.5;
	double nn_nms_threshold_ = 0.4;
	std::vector<std::string> nn_output_names_;

	// 最近一次 detectNn() 的检测结果。
	std::vector<DetectionInfo> last_detections_;
};

} // namespace etest::vision