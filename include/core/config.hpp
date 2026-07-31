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
		int width = 1280;
		int height = 720;
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

	// 子配置：图传

	struct StreamConfig
	{
		bool enabled = false;

		// 接收树莓派地址
		std::string host = "127.0.0.1";
		int port = 5000;

		// RTP/JPEG 参数
		int payload_type = 26;
		int mtu = 1200;

		// 图传初始化失败时，是否退回普通 V4L2 摄像头
		bool allow_fallback = true;

		SourceInfo source_enabled;
		SourceInfo source_host;
		SourceInfo source_port;
		SourceInfo source_payload_type;
		SourceInfo source_mtu;
		SourceInfo source_allow_fallback;

		std::string to_string() const;
	};

	// 子配置：NCNN 小球检测（定义在 VisionConfig 之前）

	struct AxisPoint
	{
		double pixel = 0.0;
		double position_mm = 0.0;
	};

	struct PipeAxisCalibration
	{
		std::vector<AxisPoint> points;
		int image_right_sign = 1; // +1 或 -1
	};

	struct BallNcnnConfig
	{
		bool enabled = true;

		// ── 模型路径 ──
		std::string full_model_param;
		std::string center_model_param;

		// ── 输入尺寸 ──
		int full_input_width = 640;
		int full_input_height = 160;
		int center_input_width = 224;
		int center_input_height = 160;

		// ── ROI 原始尺寸 ──
		int full_src_width = 1280;
		int full_src_height = 320;
		int center_src_width = 448;
		int center_src_height = 320;

		// ── ROI 定位模式 ──
		// "center_point" = 通过 pipe_center_x/y 反算左上角（旧逻辑）
		// "topleft"      = 直接使用 full_roi_*/center_roi_* 左上角坐标
		std::string roi_location_mode = "center_point";

		// ── Full/Center ROI 左上角坐标（仅在 topleft 模式使用，1280×720 原图坐标）──
		int full_roi_x = 0;
		int full_roi_y = 160;
		int center_roi_x = 416;
		int center_roi_y = 160;

		// ── 中心线 y 坐标（水平参考线，贯穿全图宽度）──
		int center_line_y = 360;

		// ── 物理中心 O（全局像素坐标，center_point 模式使用）──
		int pipe_center_x = 640;
		int pipe_center_y = 360;

		// ── 跟踪阈值 ──
		int edge_guard_px = 24;
		int lost_frames_to_reacquire = 2;
		int stable_frames_to_center = 8;

		// ── Q3～Q5 启动标定线 ──

		// 原图全局像素 X，球心只需靠近此竖直线。
		// Y 坐标不参与任务启动标定。
		int calibration_line_x = 640;

		// 允许球心位于标定线左右多少像素。
		int calibration_line_tolerance_px = 20;

		// 连续标定帧内允许的最大 X 抖动。
		double calibration_max_jitter_px = 6.0;

		// ── 标定（旧字段，保留兼容）──
		int calibration_frames = 8;
		int calibration_timeout_ms = 1500;
		double initial_center_limit_mm = 15.0;
		double minimum_confidence = 0.45;

		// ── 坐标标定 ──
		PipeAxisCalibration axis_calibration;

		// ── NCNN 高级选项 ──
		int num_threads = 4;
		bool use_fp16_storage = true;
		bool use_fp16_arithmetic = true;

		SourceInfo source_enabled;
		SourceInfo source_full_model_param;
		SourceInfo source_center_model_param;
		SourceInfo source_full_input_width;
		SourceInfo source_full_input_height;
		SourceInfo source_center_input_width;
		SourceInfo source_center_input_height;
		SourceInfo source_full_src_width;
		SourceInfo source_full_src_height;
		SourceInfo source_center_src_width;
		SourceInfo source_center_src_height;
		SourceInfo source_roi_location_mode;
		SourceInfo source_full_roi_x;
		SourceInfo source_full_roi_y;
		SourceInfo source_center_roi_x;
		SourceInfo source_center_roi_y;
		SourceInfo source_center_line_y;
		SourceInfo source_pipe_center_x;
		SourceInfo source_pipe_center_y;
		SourceInfo source_edge_guard_px;
		SourceInfo source_lost_frames_to_reacquire;
		SourceInfo source_stable_frames_to_center;
		SourceInfo source_calibration_line_x;
		SourceInfo source_calibration_line_tolerance_px;
		SourceInfo source_calibration_max_jitter_px;
		SourceInfo source_calibration_frames;
		SourceInfo source_calibration_timeout_ms;
		SourceInfo source_initial_center_limit_mm;
		SourceInfo source_minimum_confidence;
		SourceInfo source_num_threads;
		SourceInfo source_use_fp16_storage;
		SourceInfo source_use_fp16_arithmetic;

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
		double axis_length_cm = 19.8;

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

		// ── 管道几何平滑 ──
		double pipe_geometry_alpha = 0.25;
		bool pipe_update_each_frame = true;

		// ── 管道锁定 ──
		int pipe_stable_frames = 3;
		int pipe_lost_timeout_frames = 30;
		double pipe_similarity_center_max_px = 25.0;
		double pipe_similarity_length_max_px = 40.0;

		// ── 管道短边约束 ──
		double pipe_min_short_side_px = 8.0;

		// ── 透视展开 ──
		int pipe_warp_width = 500;
		int pipe_warp_height = 80;
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

		// ── HoughCircles 参数 ──
		double hough_dp = 1.2;
		double hough_min_distance = 30.0;
		double hough_param1 = 80.0;
		double hough_param2 = 14.0;

		int ball_min_radius = 7;
		int ball_max_radius = 17;
		double ball_expected_radius = 11.0;

		double ball_min_center_y_ratio = 0.25;
		double ball_max_center_y_ratio = 0.75;
		double ball_expected_center_y_ratio = 0.50;

		double ball_max_inner_gray = 135.0;
		double ball_min_ring_contrast = 0.0;
		double ball_good_ring_contrast = 30.0;
		double ball_min_quality = 0.45;

		// ── alpha-beta 跟踪器 ──
		double tracker_alpha = 0.65;
		double tracker_beta = 0.08;
		double tracker_gate_ratio = 0.055;
		double tracker_gate_growth_per_lost_frame = 0.25;
		double tracker_max_gate_ratio = 0.18;
		double tracker_max_speed_ratio_per_second = 2.0;

		int tracker_max_predict_frames = 3;
		int tracker_global_reacquire_frames = 15;
		int reacquire_confirm_frames = 2;
		int acquire_confirm_frames = 3;

		// 重捕获：丢球超过此帧数后放弃旧参考点，进行全局搜索 (deprecated)
		int reacquire_after_lost_frames = 5;

		// 零点校准
		std::string zero_mode =
		    "startup"; // "startup" | "fixed" | "ratio"
		double zero_position_px =
		    240.5; // 轴线投影距离(px), 非图像x坐标
		double zero_position_ratio = 0.5;
		int zero_samples = 40;
		double zero_range_px = 4.0;

		// 位置低通滤波，越小越平滑但延迟越大
		double filter_alpha = 0.35;

		// ── 测试用视频输出路径（空字符串=不保存）──
		std::string video_output = "";

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

		SourceInfo source_pipe_geometry_alpha;
		SourceInfo source_pipe_update_each_frame;

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

		SourceInfo source_hough_dp;
		SourceInfo source_hough_min_distance;
		SourceInfo source_hough_param1;
		SourceInfo source_hough_param2;
		SourceInfo source_ball_min_radius;
		SourceInfo source_ball_max_radius;
		SourceInfo source_ball_expected_radius;
		SourceInfo source_ball_min_center_y_ratio;
		SourceInfo source_ball_max_center_y_ratio;
		SourceInfo source_ball_expected_center_y_ratio;
		SourceInfo source_ball_max_inner_gray;
		SourceInfo source_ball_min_ring_contrast;
		SourceInfo source_ball_good_ring_contrast;
		SourceInfo source_ball_min_quality;

		SourceInfo source_tracker_alpha;
		SourceInfo source_tracker_beta;
		SourceInfo source_tracker_gate_ratio;
		SourceInfo source_tracker_gate_growth_per_lost_frame;
		SourceInfo source_tracker_max_gate_ratio;
		SourceInfo source_tracker_max_speed_ratio_per_second;
		SourceInfo source_tracker_max_predict_frames;
		SourceInfo source_tracker_global_reacquire_frames;
		SourceInfo source_reacquire_confirm_frames;
		SourceInfo source_acquire_confirm_frames;

		SourceInfo source_zero_position_ratio;
		SourceInfo source_max_axis_distance_px;
		SourceInfo source_max_jump_px;
		SourceInfo source_reacquire_after_lost_frames;
		SourceInfo source_zero_mode;
		SourceInfo source_zero_position_px;
		SourceInfo source_zero_samples;
		SourceInfo source_zero_range_px;
		SourceInfo source_filter_alpha;
		SourceInfo source_video_output;

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
		BallNcnnConfig ball_ncnn;

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
		int protocol_version_major = 5;
		int protocol_version_minor = 2;
		// 旧版兼容字段
		int protocol_version = 5; // V5 (deprecated)

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
		SourceInfo source_protocol_version_major;
		SourceInfo source_protocol_version_minor;
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

		// ── 检测器选择 ──
		std::string detector = "traditional"; // "traditional" | "yolo"

		// ── YOLO 后端选择 ──
		std::string yolo_backend = "opencv"; // "opencv" | "ncnn"

		// ── 模型路径 ──
		std::string model_path = "model/yolov5s.onnx";
		std::string class_names_path = "model/coco.names";

		// ── NCNN 模型路径（仅 yolo_backend = "ncnn"）──
		std::string ncnn_param_path =
		    "model/ncnn_640x640/best_640x640_fp32.ncnn.param";

		double nn_confidence_threshold = 0.5;
		double nn_nms_threshold = 0.4;

		// ── YOLO 输入尺寸 ──
		int nn_input_width = 640;
		int nn_input_height = 640;
		int nn_threads = 4;

		// ── NCNN 高级选项 ──
		bool ncnn_use_fp16_storage = false;
		bool ncnn_use_fp16_arithmetic = false;
		bool ncnn_use_vulkan = false;
		std::string ncnn_input_blob = "in0";
		std::string ncnn_output_blob = "out0";

		// ── 原点标定 ──
		int zero_sample_count = 12;
		double zero_max_jitter_px = 5.0;
		int zero_max_wait_frames = 90;
		double zero_min_confidence = 0.55;

		// ── 跟踪与滤波 ──
		double max_target_jump_px = 80.0;
		double position_filter_alpha = 0.45;
		int max_hold_frames = 2;
		int lost_confirm_frames = 3;
		int reacquire_confirm_frames = 2;

		// ── 物理换算 ──
		double mm_per_pixel = 0.52;
		bool invert_offset = false;

		// ── 通信周期 ──
		int result_send_interval_ms = 40;
		int calib_status_interval_ms = 250;
		int vsession_retry_interval_ms = 500;

		// ── 模型恢复 ──
		int max_inference_errors = 5;
		int model_reload_interval_ms = 2000;

		// ── VSESSION ──
		std::string camera_id = "CAM0";
		int nominal_fps = 30;

		SourceInfo source_show_preview;
		SourceInfo source_enable_nn;
		SourceInfo source_detector;
		SourceInfo source_yolo_backend;
		SourceInfo source_model_path;
		SourceInfo source_class_names_path;
		SourceInfo source_ncnn_param_path;
		SourceInfo source_nn_confidence_threshold;
		SourceInfo source_nn_nms_threshold;
		SourceInfo source_nn_input_width;
		SourceInfo source_nn_input_height;
		SourceInfo source_nn_threads;
		SourceInfo source_ncnn_use_fp16_storage;
		SourceInfo source_ncnn_use_fp16_arithmetic;
		SourceInfo source_ncnn_use_vulkan;
		SourceInfo source_ncnn_input_blob;
		SourceInfo source_ncnn_output_blob;
		SourceInfo source_zero_sample_count;
		SourceInfo source_zero_max_jitter_px;
		SourceInfo source_zero_max_wait_frames;
		SourceInfo source_zero_min_confidence;
		SourceInfo source_max_target_jump_px;
		SourceInfo source_position_filter_alpha;
		SourceInfo source_max_hold_frames;
		SourceInfo source_lost_confirm_frames;
		SourceInfo source_reacquire_confirm_frames;
		SourceInfo source_mm_per_pixel;
		SourceInfo source_invert_offset;
		SourceInfo source_result_send_interval_ms;
		SourceInfo source_calib_status_interval_ms;
		SourceInfo source_vsession_retry_interval_ms;
		SourceInfo source_max_inference_errors;
		SourceInfo source_model_reload_interval_ms;
		SourceInfo source_camera_id;
		SourceInfo source_nominal_fps;

		std::string to_string() const;
	};

	// 应用总配置

	struct AppConfig
	{
		ModeConfig mode;
		RuntimeConfig runtime;
		LoggerConfig logger;
		CameraConfig camera;
		StreamConfig stream;
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