#pragma once

#include "vision/vision.hpp"

#include <opencv2/core.hpp>

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace etest::vision
{

	// ── 预处理模式 ──
	enum class ResizeMode
	{
		STRETCH,   // 直接拉伸到模型输入尺寸（现有行为）
		LETTERBOX, // 保持宽高比缩放 + 补灰边
	};

	// ── 预处理变换信息（用于解码时反解坐标）──
	struct PreprocessTransform
	{
		ResizeMode mode = ResizeMode::STRETCH;

		int source_width = 0;
		int source_height = 0;

		int input_width = 0;
		int input_height = 0;

		// STRETCH 模式下的独立缩放系数
		float scale_x = 1.0F;
		float scale_y = 1.0F;

		// LETTERBOX 模式下的统一缩放和补边量
		float uniform_scale = 1.0F;

		int padding_left = 0;
		int padding_top = 0;
	};

	// 推理后端输出原始数据（[候选数, 5 + 类别数]）
	struct YoloRawOutput
	{
		std::vector<float> data;

		// 候选框数量，例如 25200
		int rows = 0;

		// 5 + 类别数，目前模型为 6
		int columns = 0;

		// 预处理变换信息（解码器用于正确反解坐标）
		PreprocessTransform transform;

		bool valid() const noexcept
		{
			return rows > 0 && columns >= 6
			    && data.size()
			    == static_cast<std::size_t>(rows * columns);
		}
	};

	// 后端配置
	struct YoloBackendConfig
	{
		int input_width = 640;
		int input_height = 640;
		int num_threads = 4;

		ResizeMode resize_mode = ResizeMode::STRETCH;

		bool use_fp16_storage = false;
		bool use_fp16_arithmetic = false;
		bool use_vulkan = false;

		std::string input_blob = "in0";
		std::string output_blob = "out0";
	};

	// 推理后端抽象接口
	class IYoloBackend
	{
	public:
		virtual ~IYoloBackend() = default;

		// 加载模型
		// model_path: ONNX 路径（OpenCV）或 param 文件前缀（NCNN）
		virtual bool load(const std::string& model_path,
		                  const YoloBackendConfig& config,
		                  std::string& error) noexcept = 0;

		// 执行前向推理
		// frame: BGR 彩色图，尺寸由摄像头决定
		// output: 填充原始输出数据
		// timing: 可选，填充各阶段耗时
		virtual bool forward(const cv::Mat& frame,
		                     YoloRawOutput& output, YoloTiming* timing,
		                     std::string& error) noexcept = 0;

		// 后端是否就绪
		virtual bool ready() const noexcept = 0;

		// 后端名称，用于日志和 benchmark
		virtual const char* name() const noexcept = 0;
	};

	// ── 工厂函数 ──

	// 创建 OpenCV 后端（定义在 yolo_backend_opencv.cpp）
	std::unique_ptr<IYoloBackend> createOpenCvBackend() noexcept;

#ifdef ETEST_HAS_NCNN
	// 创建 NCNN 后端（定义在 yolo_backend_ncnn.cpp）
	// param_path: 完整的 .param 文件路径
	std::unique_ptr<IYoloBackend> createNcnnBackend(
	    const std::string& param_path,
	    const YoloBackendConfig& config) noexcept;
#endif

} // namespace etest::vision
