#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace etest
{

	// 键来源追踪

	struct SourceInfo
	{
		std::string path;
		int line = 0;
	};

	// 日志记录级别

	enum class LogLevel
	{
		DEBUG = 0,
		INFO = 1,
		WARN = 2,
		ERROR = 3, // 可恢复错误
		FATAL = 4  // 不可恢复错误，触发退出
	};

	// 配置消息

	enum class ConfigMessageLevel
	{
		WARN,
		ERROR
	};

	struct ConfigMessage
	{
		ConfigMessageLevel level = ConfigMessageLevel::WARN;
		std::string source = "CONFIG";
		std::string description;
	};

	// 子配置：模式

	struct ModeConfig
	{
		bool enabled = false;
		std::string name = "competition";

		SourceInfo source_enabled;
		SourceInfo source_name;

		std::string to_string() const;
	};

	// 子配置：运行时

	struct RuntimeConfig
	{
		bool headless = false;
		bool allow_keyboard_exit = true;
		bool enable_self_check = true;
		bool enable_auto_recovery = true;
		int camera_retry_interval_ms = 500;
		int uart_retry_interval_ms = 1000;

		SourceInfo source_headless;
		SourceInfo source_allow_keyboard_exit;
		SourceInfo source_enable_self_check;
		SourceInfo source_enable_auto_recovery;
		SourceInfo source_camera_retry_interval_ms;
		SourceInfo source_uart_retry_interval_ms;

		std::string to_string() const;
	};

	// 子配置：日志

	struct LoggerConfig
	{
		std::string directory = "data/log";
		bool file = false;
		bool terminal = true;
		LogLevel min_level = LogLevel::INFO;
		bool flush_each_write = false;
		int throttle_interval_ms = 1000;

		SourceInfo source_directory;
		SourceInfo source_file;
		SourceInfo source_terminal;
		SourceInfo source_min_level;
		SourceInfo source_flush_each_write;
		SourceInfo source_throttle_interval_ms;

		std::string to_string() const;
	};

	// 子配置：摄像头

	struct CameraConfig
	{
		std::string source = "/dev/video0";
		int width = 640;
		int height = 480;
		int fps = 30;
		std::string fourcc = "MJPG";
		bool loop_video = false;
		bool realtime_playback = true;
		int playback_fps = 30;

		SourceInfo source_source;
		SourceInfo source_width;
		SourceInfo source_height;
		SourceInfo source_fps;
		SourceInfo source_fourcc;
		SourceInfo source_loop_video;
		SourceInfo source_realtime_playback;
		SourceInfo source_playback_fps;

		std::string to_string() const;
	};

	// 子配置：视觉处理

	struct VisionConfig
	{
		int red_h1_min = 0;
		int red_h1_max = 10;
		int red_h2_min = 170;
		int red_h2_max = 180;
		int saturation_min = 100;
		int value_min = 80;

		int morphology_kernel = 5;
		double min_area = 200.0;

		SourceInfo source_red_h1_min;
		SourceInfo source_red_h1_max;
		SourceInfo source_red_h2_min;
		SourceInfo source_red_h2_max;
		SourceInfo source_saturation_min;
		SourceInfo source_value_min;
		SourceInfo source_morphology_kernel;
		SourceInfo source_min_area;

		std::string to_string() const;
	};

	// 子配置：UART

	struct UartConfig
	{
		std::string device = "/dev/ttyAMA0";
		int baudrate = 115200;
		int timeout_ms = 1000;
		int write_timeout_ms = 500;
		int reconnect_interval_ms = 2000;
		bool auto_reconnect = true;
		int max_line_length = 256;
		int queue_capacity = 100;

		// 协议握手与心跳
		int handshake_timeout_ms = 1500;
		int heartbeat_interval_ms = 500;
		int heartbeat_timeout_ms = 2000;
		int protocol_version = 4; // V4 only

		SourceInfo source_device;
		SourceInfo source_baudrate;
		SourceInfo source_timeout_ms;
		SourceInfo source_write_timeout_ms;
		SourceInfo source_reconnect_interval_ms;
		SourceInfo source_auto_reconnect;
		SourceInfo source_max_line_length;
		SourceInfo source_queue_capacity;
		SourceInfo source_handshake_timeout_ms;
		SourceInfo source_heartbeat_interval_ms;
		SourceInfo source_heartbeat_timeout_ms;
		SourceInfo source_protocol_version;

		std::string to_string() const;
	};

	// 子配置：录像

	struct RecordConfig
	{
		bool enabled = false;
		bool save_raw = true;
		bool save_debug = false;
		std::string directory = "data/video";
		std::string fourcc = "MJPG";
		int fps = 30;
		int segment_seconds = 300;

		SourceInfo source_enabled;
		SourceInfo source_save_raw;
		SourceInfo source_save_debug;
		SourceInfo source_directory;
		SourceInfo source_fourcc;
		SourceInfo source_fps;
		SourceInfo source_segment_seconds;

		std::string to_string() const;
	};

	// 子配置：搜索

	struct SearchConfig
	{
		bool show_preview = true;
		bool enable_nn = false;
		std::string model_path = "model/yolov5s.onnx";
		std::string class_names_path = "model/coco.names";
		double nn_confidence_threshold = 0.5;
		double nn_nms_threshold = 0.4;

		SourceInfo source_show_preview;
		SourceInfo source_enable_nn;
		SourceInfo source_model_path;
		SourceInfo source_class_names_path;
		SourceInfo source_nn_confidence_threshold;
		SourceInfo source_nn_nms_threshold;

		std::string to_string() const;
	};

	// 应用总配置

	struct AppConfig
	{
		ModeConfig mode;
		RuntimeConfig runtime;
		LoggerConfig logger;
		CameraConfig camera;
		VisionConfig vision;
		UartConfig uart;
		RecordConfig record;
		SearchConfig search;
	};

	// 配置加载结果

	struct ConfigLoadResult
	{
		AppConfig config;
		bool file_loaded = false;
		std::vector<ConfigMessage> messages;

		// 加载统计
		std::string config_dir;
		std::vector<std::string> loaded_files;
		std::vector<std::string> failed_files;
		std::string mode_name;
		bool mode_applied = false;
		std::string mode_file_path;
	};

	// 配置加载器

	struct ConfigLoader
	{
		static ConfigLoadResult load(const std::string& path) noexcept;
		static ConfigLoadResult loadMultiple(
		    const std::vector<std::string>& paths) noexcept;
		ConfigLoader() = delete;
	};

	ConfigLoadResult loadAppConfigFromDir(
	    const std::string& config_dir);
	ConfigLoadResult loadAppConfigFromMainFile(
	    const std::string& main_toml_path);

	bool validateConfig(
	    const AppConfig& config,
	    std::vector<ConfigMessage>* messages_out = nullptr);

} // namespace etest