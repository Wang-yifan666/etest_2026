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

	// 子配置：滚球视觉

	struct BallConfig
	{
		// ── 工作分辨率 ──
		int work_width = 640;
		int work_height = 360;

		// ── 管道模式 ──
		std::string pipe_mode = "fixed"; // "auto" | "fixed"

		// ── 固定管道四点（工作分辨率坐标，pipe_mode=fixed 时使用）──
		double pipe_fixed_tl_x = 20.0;
		double pipe_fixed_tl_y = 30.0;
		double pipe_fixed_tr_x = 620.0;
		double pipe_fixed_tr_y = 30.0;
		double pipe_fixed_br_x = 620.0;
		double pipe_fixed_br_y = 170.0;
		double pipe_fixed_bl_x = 20.0;
		double pipe_fixed_bl_y = 170.0;

		// ── 管道搜索区域（工作分辨率内坐标）──
		int pipe_search_roi_x = 10;
		int pipe_search_roi_y = 20;
		int pipe_search_roi_w = 540;
		int pipe_search_roi_h = 160;

		// 摆杆中心线两端（整幅图像坐标），P1→P2 为正方向
		double axis_x1 = 80.0;
		double axis_y1 = 230.0;
		double axis_x2 = 560.0;
		double axis_y2 = 230.0;
		double axis_length_cm = 20.0;

		// ── 棕色管道 HSV 阈值 ──
		int brown_h_min = 5;
		int brown_h_max = 30;
		int brown_s_min = 60;
		int brown_s_max = 255;
		int brown_v_min = 30;
		int brown_v_max = 230;

		// ── 管道形态学 ──
		int pipe_close_kernel_w = 31;
		int pipe_close_kernel_h = 9;
		int pipe_open_kernel = 3;

		// ── 管道几何约束 ──
		double pipe_min_area_ratio = 0.05;
		double pipe_min_aspect_ratio = 3.0;
		double pipe_min_fill_ratio = 0.30;
		double pipe_horizontal_angle_max = 25.0;

		// ── 管道锁定 ──
		int pipe_stable_frames = 3;
		int pipe_lost_timeout_frames = 30;
		double pipe_similarity_center_max_px = 25.0;
		double pipe_similarity_length_max_px = 40.0;

		// ── 管道短边约束 ──
		double pipe_min_short_side_px = 8.0;

		// ── 透视展开 ──
		int pipe_warp_width = 500;
		int pipe_warp_height = 120;
		double pipe_inner_margin_x_ratio = 0.03;
		double pipe_inner_margin_y_ratio = 0.10;

		// ── 毫米换算：管道实际长度(mm) ──
		double pipe_length_mm = 250.0;

		// 局部对比度检测参数
		int bg_kernel = 31;
		int threshold = 18;
		int morph_kernel = 3;

		// 候选轮廓约束
		double min_area = 40.0;
		double max_area = 3000.0;
		double min_circularity = 0.35;
		double max_axis_distance_px = 30.0;
		double max_jump_px = 50.0;

		// 钢球长宽比约束
		double ball_min_aspect = 0.65;
		double ball_max_aspect = 1.45;

		// 局部对比度约束
		double ball_min_local_contrast = 8.0;

		// 到管道中心线最大距离
		double ball_max_centerline_distance_px = 35.0;

		// 重捕获：丢球超过此帧数后放弃旧参考点，进行全局搜索
		int reacquire_after_lost_frames = 5;

		// 零点校准
		std::string zero_mode = "startup"; // "startup" | "fixed"
		double zero_position_px =
		    240.5; // 轴线投影距离(px), 非图像x坐标
		int zero_samples = 40;
		double zero_range_px = 4.0;

		// 位置低通滤波，越小越平滑但延迟越大
		double filter_alpha = 0.35;

		// ── SourceInfo ──
		SourceInfo source_work_width;
		SourceInfo source_work_height;
		SourceInfo source_pipe_mode;
		SourceInfo source_pipe_fixed_tl_x;
		SourceInfo source_pipe_fixed_tl_y;
		SourceInfo source_pipe_fixed_tr_x;
		SourceInfo source_pipe_fixed_tr_y;
		SourceInfo source_pipe_fixed_br_x;
		SourceInfo source_pipe_fixed_br_y;
		SourceInfo source_pipe_fixed_bl_x;
		SourceInfo source_pipe_fixed_bl_y;
		SourceInfo source_pipe_search_roi_x;
		SourceInfo source_pipe_search_roi_y;
		SourceInfo source_pipe_search_roi_w;
		SourceInfo source_pipe_search_roi_h;
		SourceInfo source_axis_x1;
		SourceInfo source_axis_y1;
		SourceInfo source_axis_x2;
		SourceInfo source_axis_y2;
		SourceInfo source_axis_length_cm;

		SourceInfo source_brown_h_min;
		SourceInfo source_brown_h_max;
		SourceInfo source_brown_s_min;
		SourceInfo source_brown_s_max;
		SourceInfo source_brown_v_min;
		SourceInfo source_brown_v_max;

		SourceInfo source_pipe_close_kernel_w;
		SourceInfo source_pipe_close_kernel_h;
		SourceInfo source_pipe_open_kernel;

		SourceInfo source_pipe_min_area_ratio;
		SourceInfo source_pipe_min_aspect_ratio;
		SourceInfo source_pipe_min_fill_ratio;
		SourceInfo source_pipe_horizontal_angle_max;

		SourceInfo source_pipe_stable_frames;
		SourceInfo source_pipe_lost_timeout_frames;

		SourceInfo source_pipe_warp_width;
		SourceInfo source_pipe_warp_height;
		SourceInfo source_pipe_inner_margin_x_ratio;
		SourceInfo source_pipe_inner_margin_y_ratio;

		SourceInfo source_pipe_length_mm;

		SourceInfo source_bg_kernel;
		SourceInfo source_threshold;
		SourceInfo source_morph_kernel;
		SourceInfo source_min_area;
		SourceInfo source_max_area;
		SourceInfo source_min_circularity;
		SourceInfo source_ball_min_aspect;
		SourceInfo source_ball_max_aspect;
		SourceInfo source_ball_min_local_contrast;
		SourceInfo source_ball_max_centerline_distance_px;
		SourceInfo source_max_axis_distance_px;
		SourceInfo source_max_jump_px;
		SourceInfo source_reacquire_after_lost_frames;
		SourceInfo source_zero_mode;
		SourceInfo source_zero_position_px;
		SourceInfo source_zero_samples;
		SourceInfo source_zero_range_px;
		SourceInfo source_filter_alpha;

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

		BallConfig ball;

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
		int protocol_version = 5; // V5

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