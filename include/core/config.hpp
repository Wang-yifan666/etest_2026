#pragma once

#include <string>
#include <vector>

namespace etest
{

	enum class LogLevel
	{
		DEBUG = 0,
		INFO,
		WARN,
		ERROR,
		FATAL
	};

	struct LoggerConfig
	{
		std::string directory = "data/log";
		bool file = true;
		bool terminal = true;
		bool flush_each_write = true;
		LogLevel min_level = LogLevel::INFO;
		int throttle_interval_ms = 0;
	};

	struct SearchConfig
	{
		bool show_preview = true;
		bool enable_nn = false;
		std::string model_path = "model/yolov5s.onnx";
		std::string class_names_path = "model/coco.names";
		double nn_confidence_threshold = 0.5;
		double nn_nms_threshold = 0.4;
	};

	namespace vision
	{

		struct CameraConfig
		{
			// "0"、"1" 表示摄像头编号，也可以填写 /dev/... 或视频文件路径。
			std::string source = "0";
			int width = 640;
			int height = 480;
			int fps = 30;
			std::string fourcc = "MJPG";
		};

		struct VisionConfig
		{
			int red_h1_min = 0;
			int red_h1_max = 10;
			int red_h2_min = 170;
			int red_h2_max = 180;
			int saturation_min = 100;
			int value_min = 80;

			int morphology_kernel = 5;
			double min_area = 300.0;
		};

	} // namespace vision

	struct UartConfig
	{
		std::string device = "/dev/ttyAMA0";
		int baudrate = 115200;

		// termios 的 VTIME 精度为 100 ms。
		int timeout_ms = 100;
	};

	struct AppConfig
	{
		LoggerConfig logger;
		vision::CameraConfig camera;
		vision::VisionConfig vision;
		UartConfig uart;
		SearchConfig search;
	};

	enum class ConfigMessageLevel
	{
		WARNING,
		ERROR
	};

	struct ConfigMessage
	{
		ConfigMessageLevel level = ConfigMessageLevel::WARNING;
		std::string source = "CONFIG";
		std::string description;
	};

	struct ConfigLoadResult
	{
		AppConfig config;
		std::vector<ConfigMessage> messages;
		bool file_loaded = false;
	};

	class ConfigLoader final
	{
	public:
		static ConfigLoadResult load(const std::string& path) noexcept;

		static ConfigLoadResult loadMultiple(
		    const std::vector<std::string>& paths) noexcept;

		ConfigLoader() = delete;
	};

} // namespace etest
