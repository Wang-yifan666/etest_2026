#include "core/config.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace
{

	// RawValue — 带来源路径的键值

	struct RawValue
	{
		std::string text;
		std::size_t line = 0;
		std::string source_path;
	};

	using RawConfig = std::unordered_map<std::string, RawValue>;

	// 字符串工具

	std::string trim(std::string text)
	{
		const auto not_space = [](unsigned char ch) {
			return !std::isspace(ch);
		};

		text.erase(text.begin(),
		           std::find_if(text.begin(), text.end(), not_space));

		text.erase(
		    std::find_if(text.rbegin(), text.rend(), not_space).base(),
		    text.end());

		return text;
	}

	std::string upper(std::string text)
	{
		std::transform(text.begin(), text.end(), text.begin(),
		               [](unsigned char ch) {
			               return static_cast<char>(std::toupper(ch));
		               });

		return text;
	}

	std::string stripComment(const std::string& line)
	{
		bool in_string = false;
		bool escaped = false;

		for(std::size_t i = 0; i < line.size(); ++i)
		{
			const char ch = line[i];

			if(in_string)
			{
				if(escaped)
				{
					escaped = false;
				}
				else if(ch == '\\')
				{
					escaped = true;
				}
				else if(ch == '"')
				{
					in_string = false;
				}

				continue;
			}

			if(ch == '"')
			{
				in_string = true;
			}
			else if(ch == '#')
			{
				return line.substr(0, i);
			}
		}

		return line;
	}

	// 配置消息辅助

	void addMessage(etest::ConfigLoadResult& result,
	                etest::ConfigMessageLevel level,
	                const std::string& description)
	{
		result.messages.push_back(
		    etest::ConfigMessage{level, "CONFIG", description});
	}

	// 解析器

	bool parseString(const std::string& raw, std::string& value)
	{
		const std::string text = trim(raw);

		if(text.size() < 2 || text.front() != '"' || text.back() != '"')
		{
			return false;
		}

		std::string parsed;
		parsed.reserve(text.size() - 2);

		bool escaped = false;

		for(std::size_t i = 1; i + 1 < text.size(); ++i)
		{
			const char ch = text[i];

			if(!escaped)
			{
				if(ch == '\\')
				{
					escaped = true;
				}
				else
				{
					parsed.push_back(ch);
				}

				continue;
			}

			switch(ch)
			{
			case '\\':
				parsed.push_back('\\');
				break;

			case '"':
				parsed.push_back('"');
				break;

			case 'n':
				parsed.push_back('\n');
				break;

			case 'r':
				parsed.push_back('\r');
				break;

			case 't':
				parsed.push_back('\t');
				break;

			default:
				return false;
			}

			escaped = false;
		}

		if(escaped)
		{
			return false;
		}

		value = std::move(parsed);
		return true;
	}

	bool parseBool(const std::string& raw, bool& value)
	{
		const std::string text = upper(trim(raw));

		if(text == "TRUE")
		{
			value = true;
			return true;
		}

		if(text == "FALSE")
		{
			value = false;
			return true;
		}

		return false;
	}

	bool parseInt(const std::string& raw, int& value)
	{
		const std::string text = trim(raw);

		if(text.empty())
		{
			return false;
		}

		int parsed = 0;

		const auto result = std::from_chars(
		    text.data(), text.data() + text.size(), parsed);

		if(result.ec != std::errc{}
		   || result.ptr != text.data() + text.size())
		{
			return false;
		}

		value = parsed;
		return true;
	}

	bool parseDouble(const std::string& raw, double& value)
	{
		const std::string text = trim(raw);

		if(text.empty())
		{
			return false;
		}

		char* end = nullptr;
		const double parsed = std::strtod(text.c_str(), &end);

		if(end != text.c_str() + text.size() || !std::isfinite(parsed))
		{
			return false;
		}

		value = parsed;
		return true;
	}

	// 查找辅助

	const RawValue* findValue(const RawConfig& raw_config,
	                          const std::string& key)
	{
		const auto it = raw_config.find(key);

		if(it == raw_config.end())
		{
			return nullptr;
		}

		return &it->second;
	}

	// 逐层取值函数
	// 关键设计：参数 current_value 是上一层合并后的有效值。
	// 如果本层解析失败，保持 current_value 不变（而非退回 C++ 默认值）。

	std::string getString(const RawConfig& raw_config,
	                      const std::string& key,
	                      const std::string& current_value,
	                      bool allow_empty,
	                      etest::ConfigLoadResult& result,
	                      etest::SourceInfo* source_out = nullptr)
	{
		const RawValue* raw = findValue(raw_config, key);

		if(raw == nullptr)
		{
			return current_value;
		}

		std::string value;

		if(!parseString(raw->text, value)
		   || (!allow_empty && value.empty()))
		{
			addMessage(result, etest::ConfigMessageLevel::ERROR,
			           key + " is invalid at " + raw->source_path + ":"
			               + std::to_string(raw->line)
			               + "; keeping current value \""
			               + current_value + "\"");

			return current_value;
		}

		if(source_out != nullptr)
		{
			source_out->path = raw->source_path;
			source_out->line = static_cast<int>(raw->line);
		}

		return value;
	}

	bool getBool(const RawConfig& raw_config, const std::string& key,
	             bool current_value, etest::ConfigLoadResult& result,
	             etest::SourceInfo* source_out = nullptr)
	{
		const RawValue* raw = findValue(raw_config, key);

		if(raw == nullptr)
		{
			return current_value;
		}

		bool value = false;

		if(!parseBool(raw->text, value))
		{
			addMessage(result, etest::ConfigMessageLevel::ERROR,
			           key + " is invalid at " + raw->source_path + ":"
			               + std::to_string(raw->line)
			               + "; keeping current value "
			               + (current_value ? "true" : "false"));

			return current_value;
		}

		if(source_out != nullptr)
		{
			source_out->path = raw->source_path;
			source_out->line = static_cast<int>(raw->line);
		}

		return value;
	}

	int getInt(const RawConfig& raw_config, const std::string& key,
	           int current_value, int minimum, int maximum,
	           etest::ConfigLoadResult& result,
	           etest::SourceInfo* source_out = nullptr)
	{
		const RawValue* raw = findValue(raw_config, key);

		if(raw == nullptr)
		{
			return current_value;
		}

		int value = 0;

		if(!parseInt(raw->text, value))
		{
			addMessage(result, etest::ConfigMessageLevel::ERROR,
			           key + " is not an integer at " + raw->source_path
			               + ":" + std::to_string(raw->line)
			               + "; keeping current value "
			               + std::to_string(current_value));

			return current_value;
		}

		if(value < minimum || value > maximum)
		{
			addMessage(result, etest::ConfigMessageLevel::ERROR,
			           key + " is outside [" + std::to_string(minimum)
			               + ", " + std::to_string(maximum) + "] at "
			               + raw->source_path + ":"
			               + std::to_string(raw->line)
			               + "; keeping current value "
			               + std::to_string(current_value));

			return current_value;
		}

		if(source_out != nullptr)
		{
			source_out->path = raw->source_path;
			source_out->line = static_cast<int>(raw->line);
		}

		return value;
	}

	double getDouble(const RawConfig& raw_config,
	                 const std::string& key, double current_value,
	                 double minimum, double maximum,
	                 etest::ConfigLoadResult& result,
	                 etest::SourceInfo* source_out = nullptr)
	{
		const RawValue* raw = findValue(raw_config, key);

		if(raw == nullptr)
		{
			return current_value;
		}

		double value = 0.0;

		if(!parseDouble(raw->text, value))
		{
			addMessage(result, etest::ConfigMessageLevel::ERROR,
			           key + " is not a number at " + raw->source_path
			               + ":" + std::to_string(raw->line)
			               + "; keeping current value "
			               + std::to_string(current_value));

			return current_value;
		}

		if(value < minimum || value > maximum)
		{
			addMessage(result, etest::ConfigMessageLevel::ERROR,
			           key + " is outside the allowed range at "
			               + raw->source_path + ":"
			               + std::to_string(raw->line)
			               + "; keeping current value "
			               + std::to_string(current_value));

			return current_value;
		}

		if(source_out != nullptr)
		{
			source_out->path = raw->source_path;
			source_out->line = static_cast<int>(raw->line);
		}

		return value;
	}

	etest::LogLevel getLogLevel(const RawConfig& raw_config,
	                            const std::string& key,
	                            etest::LogLevel current_value,
	                            etest::ConfigLoadResult& result,
	                            etest::SourceInfo* source_out = nullptr)
	{
		const RawValue* raw = findValue(raw_config, key);

		if(raw == nullptr)
		{
			return current_value;
		}

		std::string value;

		if(!parseString(raw->text, value))
		{
			addMessage(result, etest::ConfigMessageLevel::ERROR,
			           key + " is invalid at " + raw->source_path + ":"
			               + std::to_string(raw->line)
			               + "; keeping current value");

			return current_value;
		}

		value = upper(trim(value));

		if(value == "DEBUG")
		{
			if(source_out != nullptr)
			{
				source_out->path = raw->source_path;
				source_out->line = static_cast<int>(raw->line);
			}

			return etest::LogLevel::DEBUG;
		}

		if(value == "INFO")
		{
			if(source_out != nullptr)
			{
				source_out->path = raw->source_path;
				source_out->line = static_cast<int>(raw->line);
			}

			return etest::LogLevel::INFO;
		}

		if(value == "WARN" || value == "WARNING")
		{
			if(source_out != nullptr)
			{
				source_out->path = raw->source_path;
				source_out->line = static_cast<int>(raw->line);
			}

			return etest::LogLevel::WARN;
		}

		if(value == "ERROR")
		{
			if(source_out != nullptr)
			{
				source_out->path = raw->source_path;
				source_out->line = static_cast<int>(raw->line);
			}

			return etest::LogLevel::ERROR;
		}

		if(value == "FATAL")
		{
			if(source_out != nullptr)
			{
				source_out->path = raw->source_path;
				source_out->line = static_cast<int>(raw->line);
			}

			return etest::LogLevel::FATAL;
		}

		addMessage(result, etest::ConfigMessageLevel::ERROR,
		           key + " has unknown level \"" + value + "\" at "
		               + raw->source_path + ":"
		               + std::to_string(raw->line)
		               + "; keeping current value");

		return current_value;
	}

	// TOML 文件解析 → RawConfig

	RawConfig parseFile(std::ifstream& input,
	                    const std::string& source_path,
	                    etest::ConfigLoadResult& result)
	{
		RawConfig raw_config;

		std::string section;
		std::string line;
		std::size_t line_number = 0;

		while(std::getline(input, line))
		{
			++line_number;

			line = trim(stripComment(line));

			if(line.empty())
			{
				continue;
			}

			if(line.front() == '[')
			{
				if(line.size() < 3 || line.back() != ']')
				{
					addMessage(result, etest::ConfigMessageLevel::ERROR,
					           "invalid section declaration at "
					               + source_path + ":"
					               + std::to_string(line_number));

					continue;
				}

				section = trim(line.substr(1, line.size() - 2));

				if(section.empty())
				{
					addMessage(result, etest::ConfigMessageLevel::ERROR,
					           "empty section name at " + source_path
					               + ":" + std::to_string(line_number));
				}

				continue;
			}

			const auto equal = line.find('=');

			if(equal == std::string::npos)
			{
				addMessage(result, etest::ConfigMessageLevel::ERROR,
				           "missing '=' at " + source_path + ":"
				               + std::to_string(line_number));

				continue;
			}

			const std::string key = trim(line.substr(0, equal));

			const std::string value = trim(line.substr(equal + 1));

			if(key.empty() || value.empty())
			{
				addMessage(result, etest::ConfigMessageLevel::ERROR,
				           "empty key or value at " + source_path + ":"
				               + std::to_string(line_number));

				continue;
			}

			const std::string full_key =
			    section.empty() ? key : section + "." + key;

			const auto existing = raw_config.find(full_key);

			if(existing != raw_config.end())
			{
				addMessage(result, etest::ConfigMessageLevel::WARN,
				           full_key + " is duplicated at " + source_path
				               + ":" + std::to_string(line_number)
				               + " (last value used; previous was at "
				               + existing->second.source_path + ":"
				               + std::to_string(existing->second.line)
				               + ")");
			}

			raw_config[full_key] =
			    RawValue{value, line_number, source_path};
		}

		return raw_config;
	}

	// 未知键检测

	const std::unordered_set<std::string>& knownKeysSet()
	{
		static const std::unordered_set<std::string> s{
		    "mode.enabled",
		    "mode.name",

		    "runtime.headless",
		    "runtime.allow_keyboard_exit",
		    "runtime.enable_self_check",
		    "runtime.enable_auto_recovery",
		    "runtime.camera_retry_interval_ms",
		    "runtime.uart_retry_interval_ms",

		    "logger.directory",
		    "logger.file",
		    "logger.terminal",
		    "logger.min_level",
		    "logger.flush_each_write",
		    "logger.throttle_interval_ms",

		    "camera.source",
		    "camera.width",
		    "camera.height",
		    "camera.fps",
		    "camera.fourcc",
		    "camera.loop_video",
		    "camera.realtime_playback",
		    "camera.playback_fps",

		    "vision.red_h1_min",
		    "vision.red_h1_max",
		    "vision.red_h2_min",
		    "vision.red_h2_max",
		    "vision.saturation_min",
		    "vision.value_min",
		    "vision.morphology_kernel",
		    "vision.min_area",

		    "vision.ball.work_width",
		    "vision.ball.work_height",

		    "vision.ball.pipe_mode",
		    "vision.ball.pipe_fixed_tl_x",
		    "vision.ball.pipe_fixed_tl_y",
		    "vision.ball.pipe_fixed_tr_x",
		    "vision.ball.pipe_fixed_tr_y",
		    "vision.ball.pipe_fixed_br_x",
		    "vision.ball.pipe_fixed_br_y",
		    "vision.ball.pipe_fixed_bl_x",
		    "vision.ball.pipe_fixed_bl_y",

		    "vision.ball.pipe_search_roi_x",
		    "vision.ball.pipe_search_roi_y",
		    "vision.ball.pipe_search_roi_w",
		    "vision.ball.pipe_search_roi_h",
		    "vision.ball.axis_x1",
		    "vision.ball.axis_y1",
		    "vision.ball.axis_x2",
		    "vision.ball.axis_y2",
		    "vision.ball.axis_length_cm",

		    "vision.ball.brown_h_min",
		    "vision.ball.brown_h_max",
		    "vision.ball.brown_s_min",
		    "vision.ball.brown_s_max",
		    "vision.ball.brown_v_min",
		    "vision.ball.brown_v_max",

		    "vision.ball.pipe_close_kernel_w",
		    "vision.ball.pipe_close_kernel_h",
		    "vision.ball.pipe_open_kernel",

		    "vision.ball.pipe_min_area_ratio",
		    "vision.ball.pipe_min_aspect_ratio",
		    "vision.ball.pipe_min_fill_ratio",
		    "vision.ball.pipe_horizontal_angle_max",

		    "vision.ball.pipe_geometry_alpha",
		    "vision.ball.pipe_update_each_frame",

		    "vision.ball.pipe_stable_frames",
		    "vision.ball.pipe_lost_timeout_frames",
		    "vision.ball.pipe_min_short_side_px",
		    "vision.ball.pipe_similarity_center_max_px",
		    "vision.ball.pipe_similarity_length_max_px",

		    "vision.ball.pipe_warp_width",
		    "vision.ball.pipe_warp_height",
		    "vision.ball.pipe_inner_margin_x_ratio",
		    "vision.ball.pipe_inner_margin_y_ratio",

		    "vision.ball.pipe_length_mm",

		    "vision.ball.bg_kernel",
		    "vision.ball.threshold",
		    "vision.ball.morph_kernel",
		    "vision.ball.min_area",
		    "vision.ball.max_area",
		    "vision.ball.min_circularity",
		    "vision.ball.ball_min_aspect",
		    "vision.ball.ball_max_aspect",
		    "vision.ball.ball_min_local_contrast",
		    "vision.ball.ball_max_centerline_distance_px",

		    "vision.ball.hough_dp",
		    "vision.ball.hough_min_distance",
		    "vision.ball.hough_param1",
		    "vision.ball.hough_param2",
		    "vision.ball.ball_min_radius",
		    "vision.ball.ball_max_radius",
		    "vision.ball.ball_expected_radius",
		    "vision.ball.ball_min_center_y_ratio",
		    "vision.ball.ball_max_center_y_ratio",
		    "vision.ball.ball_expected_center_y_ratio",
		    "vision.ball.ball_max_inner_gray",
		    "vision.ball.ball_min_ring_contrast",
		    "vision.ball.ball_good_ring_contrast",
		    "vision.ball.ball_min_quality",

		    "vision.ball.tracker_alpha",
		    "vision.ball.tracker_beta",
		    "vision.ball.tracker_gate_ratio",
		    "vision.ball.tracker_gate_growth_per_lost_frame",
		    "vision.ball.tracker_max_gate_ratio",
		    "vision.ball.tracker_max_speed_ratio_per_second",
		    "vision.ball.tracker_max_predict_frames",
		    "vision.ball.tracker_global_reacquire_frames",
		    "vision.ball.reacquire_confirm_frames",
		    "vision.ball.acquire_confirm_frames",

		    "vision.ball.zero_position_ratio",

		    "vision.ball.max_axis_distance_px",
		    "vision.ball.max_jump_px",
		    "vision.ball.reacquire_after_lost_frames",
		    "vision.ball.zero_mode",
		    "vision.ball.zero_position_px",
		    "vision.ball.zero_samples",
		    "vision.ball.zero_range_px",
		    "vision.ball.filter_alpha",
		    "vision.ball.video_output",

		    "uart.device",
		    "uart.baudrate",
		    "uart.timeout_ms",
		    "uart.write_timeout_ms",
		    "uart.reconnect_interval_ms",
		    "uart.auto_reconnect",
		    "uart.max_line_length",
		    "uart.queue_capacity",
		    "uart.handshake_timeout_ms",
		    "uart.heartbeat_interval_ms",
		    "uart.heartbeat_timeout_ms",
		    "uart.protocol_version",
		    "uart.protocol_version_major",
		    "uart.protocol_version_minor",

		    "record.enabled",
		    "record.save_raw",
		    "record.save_debug",
		    "record.directory",
		    "record.fourcc",
		    "record.fps",
		    "record.segment_seconds",

		    "search.show_preview",
		    "search.enable_nn",
		    "search.detector",
		    "search.yolo_backend",
		    "search.model_path",
		    "search.class_names_path",
		    "search.ncnn_param_path",
		    "search.nn_confidence_threshold",
		    "search.nn_nms_threshold",
		    "search.nn_input_width",
		    "search.nn_input_height",
		    "search.nn_threads",
		    "search.ncnn_use_fp16_storage",
		    "search.ncnn_use_fp16_arithmetic",
		    "search.ncnn_use_vulkan",
		    "search.ncnn_input_blob",
		    "search.ncnn_output_blob",
		    "search.zero_sample_count",
		    "search.zero_max_jitter_px",
		    "search.zero_max_wait_frames",
		    "search.zero_min_confidence",
		    "search.max_target_jump_px",
		    "search.position_filter_alpha",
		    "search.max_hold_frames",
		    "search.lost_confirm_frames",
		    "search.reacquire_confirm_frames",
		    "search.mm_per_pixel",
		    "search.invert_offset",
		    "search.result_send_interval_ms",
		    "search.calib_status_interval_ms",
		    "search.vsession_retry_interval_ms",
		    "search.max_inference_errors",
		    "search.model_reload_interval_ms",
		    "vision.ball_ncnn.enabled",
		    "vision.ball_ncnn.full_model_param",
		    "vision.ball_ncnn.center_model_param",
		    "vision.ball_ncnn.full_input_width",
		    "vision.ball_ncnn.full_input_height",
		    "vision.ball_ncnn.center_input_width",
		    "vision.ball_ncnn.center_input_height",
		    "vision.ball_ncnn.full_src_width",
		    "vision.ball_ncnn.full_src_height",
		    "vision.ball_ncnn.center_src_width",
		    "vision.ball_ncnn.center_src_height",
		    "vision.ball_ncnn.pipe_center_x",
		    "vision.ball_ncnn.pipe_center_y",
		    "vision.ball_ncnn.edge_guard_px",
		    "vision.ball_ncnn.lost_frames_to_reacquire",
		    "vision.ball_ncnn.stable_frames_to_center",
		    "vision.ball_ncnn.calibration_frames",
		    "vision.ball_ncnn.calibration_timeout_ms",
		    "vision.ball_ncnn.initial_center_limit_mm",
		    "vision.ball_ncnn.minimum_confidence",
		    "vision.ball_ncnn.num_threads",
		    "vision.ball_ncnn.use_fp16_storage",
		    "vision.ball_ncnn.use_fp16_arithmetic",
		    "vision.ball_ncnn.axis_calibration.image_right_sign",
		    "vision.ball_ncnn.axis_calibration.pixels",
		    "vision.ball_ncnn.axis_calibration.positions_mm",

		    "search.camera_id",
		    "search.nominal_fps",
		};

		return s;
	}

	void warnUnknownKeys(const RawConfig& raw_config,
	                     etest::ConfigLoadResult& result)
	{
		const auto& known = knownKeysSet();

		for(const auto& item: raw_config)
		{
			if(known.find(item.first) == known.end())
			{
				addMessage(result, etest::ConfigMessageLevel::WARN,
				           "unknown key " + item.first + " at "
				               + item.second.source_path + ":"
				               + std::to_string(item.second.line));
			}
		}
	}

	// 从 RawConfig 提取到 AppConfig（逐层应用，保留当前有效值）

	void extractAppConfig(etest::AppConfig& config,
	                      const RawConfig& raw_config,
	                      etest::ConfigLoadResult& result)
	{
		config.mode.enabled =
		    getBool(raw_config, "mode.enabled", config.mode.enabled,
		            result, &config.mode.source_enabled);

		config.mode.name =
		    getString(raw_config, "mode.name", config.mode.name, false,
		              result, &config.mode.source_name);

		config.runtime.headless = getBool(
		    raw_config, "runtime.headless", config.runtime.headless,
		    result, &config.runtime.source_headless);

		config.runtime.allow_keyboard_exit =
		    getBool(raw_config, "runtime.allow_keyboard_exit",
		            config.runtime.allow_keyboard_exit, result,
		            &config.runtime.source_allow_keyboard_exit);

		config.runtime.enable_self_check =
		    getBool(raw_config, "runtime.enable_self_check",
		            config.runtime.enable_self_check, result,
		            &config.runtime.source_enable_self_check);

		config.runtime.enable_auto_recovery =
		    getBool(raw_config, "runtime.enable_auto_recovery",
		            config.runtime.enable_auto_recovery, result,
		            &config.runtime.source_enable_auto_recovery);

		config.runtime.camera_retry_interval_ms = getInt(
		    raw_config, "runtime.camera_retry_interval_ms",
		    config.runtime.camera_retry_interval_ms, 50, 60000, result,
		    &config.runtime.source_camera_retry_interval_ms);

		config.runtime.uart_retry_interval_ms = getInt(
		    raw_config, "runtime.uart_retry_interval_ms",
		    config.runtime.uart_retry_interval_ms, 50, 60000, result,
		    &config.runtime.source_uart_retry_interval_ms);

		config.logger.directory = getString(
		    raw_config, "logger.directory", config.logger.directory,
		    false, result, &config.logger.source_directory);

		config.logger.file =
		    getBool(raw_config, "logger.file", config.logger.file,
		            result, &config.logger.source_file);

		config.logger.terminal = getBool(
		    raw_config, "logger.terminal", config.logger.terminal,
		    result, &config.logger.source_terminal);

		config.logger.flush_each_write =
		    getBool(raw_config, "logger.flush_each_write",
		            config.logger.flush_each_write, result,
		            &config.logger.source_flush_each_write);

		config.logger.min_level = getLogLevel(
		    raw_config, "logger.min_level", config.logger.min_level,
		    result, &config.logger.source_min_level);

		if(!config.logger.file && !config.logger.terminal)
		{
			config.logger.terminal = true;

			addMessage(
			    result, etest::ConfigMessageLevel::WARN,
			    "logger.file and logger.terminal cannot both be false; "
			    "terminal output is forced on");
		}

		config.logger.throttle_interval_ms =
		    getInt(raw_config, "logger.throttle_interval_ms",
		           config.logger.throttle_interval_ms, 0, 60000, result,
		           &config.logger.source_throttle_interval_ms);

		config.camera.source =
		    getString(raw_config, "camera.source", config.camera.source,
		              false, result, &config.camera.source_source);

		config.camera.width =
		    getInt(raw_config, "camera.width", config.camera.width, 1,
		           4096, result, &config.camera.source_width);

		config.camera.height =
		    getInt(raw_config, "camera.height", config.camera.height, 1,
		           2160, result, &config.camera.source_height);

		config.camera.fps =
		    getInt(raw_config, "camera.fps", config.camera.fps, 1, 240,
		           result, &config.camera.source_fps);

		config.camera.fourcc =
		    getString(raw_config, "camera.fourcc", config.camera.fourcc,
		              false, result, &config.camera.source_fourcc);

		config.camera.loop_video = getBool(
		    raw_config, "camera.loop_video", config.camera.loop_video,
		    result, &config.camera.source_loop_video);

		config.camera.realtime_playback =
		    getBool(raw_config, "camera.realtime_playback",
		            config.camera.realtime_playback, result,
		            &config.camera.source_realtime_playback);

		config.camera.playback_fps =
		    getInt(raw_config, "camera.playback_fps",
		           config.camera.playback_fps, 1, 240, result,
		           &config.camera.source_playback_fps);

		config.vision.red_h1_min = getInt(
		    raw_config, "vision.red_h1_min", config.vision.red_h1_min,
		    0, 180, result, &config.vision.source_red_h1_min);

		config.vision.red_h1_max = getInt(
		    raw_config, "vision.red_h1_max", config.vision.red_h1_max,
		    0, 180, result, &config.vision.source_red_h1_max);

		config.vision.red_h2_min = getInt(
		    raw_config, "vision.red_h2_min", config.vision.red_h2_min,
		    0, 180, result, &config.vision.source_red_h2_min);

		config.vision.red_h2_max = getInt(
		    raw_config, "vision.red_h2_max", config.vision.red_h2_max,
		    0, 180, result, &config.vision.source_red_h2_max);

		config.vision.saturation_min =
		    getInt(raw_config, "vision.saturation_min",
		           config.vision.saturation_min, 0, 255, result,
		           &config.vision.source_saturation_min);

		config.vision.value_min = getInt(
		    raw_config, "vision.value_min", config.vision.value_min, 0,
		    255, result, &config.vision.source_value_min);

		config.vision.morphology_kernel =
		    getInt(raw_config, "vision.morphology_kernel",
		           config.vision.morphology_kernel, 1, 31, result,
		           &config.vision.source_morphology_kernel);

		if(config.vision.morphology_kernel % 2 == 0)
		{
			config.vision.morphology_kernel = 5;

			addMessage(
			    result, etest::ConfigMessageLevel::WARN,
			    "vision.morphology_kernel must be odd; using default 5");
		}

		config.vision.min_area = getDouble(
		    raw_config, "vision.min_area", config.vision.min_area, 0.0,
		    1000000000.0, result, &config.vision.source_min_area);

		{
			auto& ball = config.vision.ball;

			ball.pipe_mode = getString(
			    raw_config, "vision.ball.pipe_mode", ball.pipe_mode,
			    false, result, &ball.source_pipe_mode);
			if(ball.pipe_mode != "auto" && ball.pipe_mode != "fixed")
			{
				addMessage(result, etest::ConfigMessageLevel::ERROR,
				           "vision.ball.pipe_mode must be \"auto\" or "
				           "\"fixed\"; keeping \""
				               + ball.pipe_mode + "\"");
			}

			ball.pipe_fixed_tl_x =
			    getDouble(raw_config, "vision.ball.pipe_fixed_tl_x",
			              ball.pipe_fixed_tl_x, 0.0, 4096.0, result,
			              &ball.source_pipe_fixed_tl_x);
			ball.pipe_fixed_tl_y =
			    getDouble(raw_config, "vision.ball.pipe_fixed_tl_y",
			              ball.pipe_fixed_tl_y, 0.0, 2160.0, result,
			              &ball.source_pipe_fixed_tl_y);
			ball.pipe_fixed_tr_x =
			    getDouble(raw_config, "vision.ball.pipe_fixed_tr_x",
			              ball.pipe_fixed_tr_x, 0.0, 4096.0, result,
			              &ball.source_pipe_fixed_tr_x);
			ball.pipe_fixed_tr_y =
			    getDouble(raw_config, "vision.ball.pipe_fixed_tr_y",
			              ball.pipe_fixed_tr_y, 0.0, 2160.0, result,
			              &ball.source_pipe_fixed_tr_y);
			ball.pipe_fixed_br_x =
			    getDouble(raw_config, "vision.ball.pipe_fixed_br_x",
			              ball.pipe_fixed_br_x, 0.0, 4096.0, result,
			              &ball.source_pipe_fixed_br_x);
			ball.pipe_fixed_br_y =
			    getDouble(raw_config, "vision.ball.pipe_fixed_br_y",
			              ball.pipe_fixed_br_y, 0.0, 2160.0, result,
			              &ball.source_pipe_fixed_br_y);
			ball.pipe_fixed_bl_x =
			    getDouble(raw_config, "vision.ball.pipe_fixed_bl_x",
			              ball.pipe_fixed_bl_x, 0.0, 4096.0, result,
			              &ball.source_pipe_fixed_bl_x);
			ball.pipe_fixed_bl_y =
			    getDouble(raw_config, "vision.ball.pipe_fixed_bl_y",
			              ball.pipe_fixed_bl_y, 0.0, 2160.0, result,
			              &ball.source_pipe_fixed_bl_y);

			ball.pipe_search_roi_x =
			    getInt(raw_config, "vision.ball.pipe_search_roi_x",
			           ball.pipe_search_roi_x, 0, 4096, result,
			           &ball.source_pipe_search_roi_x);

			ball.pipe_search_roi_y =
			    getInt(raw_config, "vision.ball.pipe_search_roi_y",
			           ball.pipe_search_roi_y, 0, 2160, result,
			           &ball.source_pipe_search_roi_y);

			ball.pipe_search_roi_w =
			    getInt(raw_config, "vision.ball.pipe_search_roi_w",
			           ball.pipe_search_roi_w, 1, 4096, result,
			           &ball.source_pipe_search_roi_w);

			ball.pipe_search_roi_h =
			    getInt(raw_config, "vision.ball.pipe_search_roi_h",
			           ball.pipe_search_roi_h, 1, 2160, result,
			           &ball.source_pipe_search_roi_h);

			ball.axis_x1 = getDouble(raw_config, "vision.ball.axis_x1",
			                         ball.axis_x1, 0.0, 4096.0, result,
			                         &ball.source_axis_x1);

			ball.axis_y1 = getDouble(raw_config, "vision.ball.axis_y1",
			                         ball.axis_y1, 0.0, 2160.0, result,
			                         &ball.source_axis_y1);

			ball.axis_x2 = getDouble(raw_config, "vision.ball.axis_x2",
			                         ball.axis_x2, 0.0, 4096.0, result,
			                         &ball.source_axis_x2);

			ball.axis_y2 = getDouble(raw_config, "vision.ball.axis_y2",
			                         ball.axis_y2, 0.0, 2160.0, result,
			                         &ball.source_axis_y2);

			ball.axis_length_cm =
			    getDouble(raw_config, "vision.ball.axis_length_cm",
			              ball.axis_length_cm, 0.1, 200.0, result,
			              &ball.source_axis_length_cm);

			// bg_kernel: 必须奇数，非法时保留上一层值
			{
				const int prev = ball.bg_kernel;
				etest::SourceInfo candidate_source;
				const int candidate =
				    getInt(raw_config, "vision.ball.bg_kernel", prev, 3,
				           255, result, &candidate_source);

				if(candidate % 2 == 1)
				{
					ball.bg_kernel = candidate;
					ball.source_bg_kernel = candidate_source;
				}
				else
				{
					addMessage(
					    result, etest::ConfigMessageLevel::ERROR,
					    "vision.ball.bg_kernel must be odd; keeping "
					    "current value "
					        + std::to_string(prev));
				}
			}

			ball.threshold = getInt(raw_config, "vision.ball.threshold",
			                        ball.threshold, 0, 255, result,
			                        &ball.source_threshold);

			// morph_kernel: 必须奇数，非法时保留上一层值
			{
				const int prev = ball.morph_kernel;
				etest::SourceInfo candidate_source;
				const int candidate =
				    getInt(raw_config, "vision.ball.morph_kernel", prev,
				           1, 31, result, &candidate_source);

				if(candidate % 2 == 1)
				{
					ball.morph_kernel = candidate;
					ball.source_morph_kernel = candidate_source;
				}
				else
				{
					addMessage(
					    result, etest::ConfigMessageLevel::ERROR,
					    "vision.ball.morph_kernel must be odd; keeping "
					    "current value "
					        + std::to_string(prev));
				}
			}

			ball.min_area = getDouble(
			    raw_config, "vision.ball.min_area", ball.min_area, 0.1,
			    1000000.0, result, &ball.source_min_area);

			ball.max_area =
			    getDouble(raw_config, "vision.ball.max_area",
			              ball.max_area, ball.min_area + 0.1, 1000000.0,
			              result, &ball.source_max_area);

			ball.min_circularity =
			    getDouble(raw_config, "vision.ball.min_circularity",
			              ball.min_circularity, 0.0, 1.0, result,
			              &ball.source_min_circularity);

			ball.max_axis_distance_px = getDouble(
			    raw_config, "vision.ball.max_axis_distance_px",
			    ball.max_axis_distance_px, 0.1, 500.0, result,
			    &ball.source_max_axis_distance_px);

			ball.max_jump_px = getDouble(
			    raw_config, "vision.ball.max_jump_px", ball.max_jump_px,
			    0.1, 500.0, result, &ball.source_max_jump_px);

			ball.reacquire_after_lost_frames = getInt(
			    raw_config, "vision.ball.reacquire_after_lost_frames",
			    ball.reacquire_after_lost_frames, 1, 1000, result,
			    &ball.source_reacquire_after_lost_frames);

			ball.zero_mode = getString(
			    raw_config, "vision.ball.zero_mode", ball.zero_mode,
			    false, result, &ball.source_zero_mode);

			if(ball.zero_mode != "startup" && ball.zero_mode != "fixed"
			   && ball.zero_mode != "ratio")
			{
				addMessage(result, etest::ConfigMessageLevel::ERROR,
				           "vision.ball.zero_mode must be \"startup\", "
				           "\"fixed\", or \"ratio\"; keeping \""
				               + ball.zero_mode + "\"");
			}

			ball.zero_position_px =
			    getDouble(raw_config, "vision.ball.zero_position_px",
			              ball.zero_position_px, 0.0, 4096.0, result,
			              &ball.source_zero_position_px);

			ball.zero_samples =
			    getInt(raw_config, "vision.ball.zero_samples",
			           ball.zero_samples, 2, 500, result,
			           &ball.source_zero_samples);

			ball.zero_range_px =
			    getDouble(raw_config, "vision.ball.zero_range_px",
			              ball.zero_range_px, 0.01, 100.0, result,
			              &ball.source_zero_range_px);

			ball.filter_alpha =
			    getDouble(raw_config, "vision.ball.filter_alpha",
			              ball.filter_alpha, 0.01, 1.0, result,
			              &ball.source_filter_alpha);

			// ── 工作分辨率 ──
			ball.work_width = getInt(
			    raw_config, "vision.ball.work_width", ball.work_width,
			    1, 4096, result, &ball.source_work_width);

			ball.work_height = getInt(
			    raw_config, "vision.ball.work_height", ball.work_height,
			    1, 2160, result, &ball.source_work_height);

			// ── 棕色管道 HSV ──
			ball.brown_h_min = getInt(
			    raw_config, "vision.ball.brown_h_min", ball.brown_h_min,
			    0, 180, result, &ball.source_brown_h_min);

			ball.brown_h_max = getInt(
			    raw_config, "vision.ball.brown_h_max", ball.brown_h_max,
			    0, 180, result, &ball.source_brown_h_max);

			ball.brown_s_min = getInt(
			    raw_config, "vision.ball.brown_s_min", ball.brown_s_min,
			    0, 255, result, &ball.source_brown_s_min);

			ball.brown_s_max = getInt(
			    raw_config, "vision.ball.brown_s_max", ball.brown_s_max,
			    0, 255, result, &ball.source_brown_s_max);

			ball.brown_v_min = getInt(
			    raw_config, "vision.ball.brown_v_min", ball.brown_v_min,
			    0, 255, result, &ball.source_brown_v_min);

			ball.brown_v_max = getInt(
			    raw_config, "vision.ball.brown_v_max", ball.brown_v_max,
			    0, 255, result, &ball.source_brown_v_max);

			// ── 管道形态学 ──
			ball.pipe_close_kernel_w =
			    getInt(raw_config, "vision.ball.pipe_close_kernel_w",
			           ball.pipe_close_kernel_w, 1, 255, result,
			           &ball.source_pipe_close_kernel_w);

			ball.pipe_close_kernel_h =
			    getInt(raw_config, "vision.ball.pipe_close_kernel_h",
			           ball.pipe_close_kernel_h, 1, 255, result,
			           &ball.source_pipe_close_kernel_h);

			ball.pipe_open_kernel =
			    getInt(raw_config, "vision.ball.pipe_open_kernel",
			           ball.pipe_open_kernel, 1, 31, result,
			           &ball.source_pipe_open_kernel);

			// ── 管道几何约束 ──
			ball.pipe_min_area_ratio =
			    getDouble(raw_config, "vision.ball.pipe_min_area_ratio",
			              ball.pipe_min_area_ratio, 0.001, 1.0, result,
			              &ball.source_pipe_min_area_ratio);

			ball.pipe_min_aspect_ratio = getDouble(
			    raw_config, "vision.ball.pipe_min_aspect_ratio",
			    ball.pipe_min_aspect_ratio, 1.0, 50.0, result,
			    &ball.source_pipe_min_aspect_ratio);

			ball.pipe_min_fill_ratio =
			    getDouble(raw_config, "vision.ball.pipe_min_fill_ratio",
			              ball.pipe_min_fill_ratio, 0.01, 1.0, result,
			              &ball.source_pipe_min_fill_ratio);

			ball.pipe_horizontal_angle_max = getDouble(
			    raw_config, "vision.ball.pipe_horizontal_angle_max",
			    ball.pipe_horizontal_angle_max, 0.0, 90.0, result,
			    &ball.source_pipe_horizontal_angle_max);

			// ── 管道几何平滑 ──
			ball.pipe_geometry_alpha =
			    getDouble(raw_config, "vision.ball.pipe_geometry_alpha",
			              ball.pipe_geometry_alpha, 0.01, 1.0, result,
			              &ball.source_pipe_geometry_alpha);

			ball.pipe_update_each_frame = getBool(
			    raw_config, "vision.ball.pipe_update_each_frame",
			    ball.pipe_update_each_frame, result,
			    &ball.source_pipe_update_each_frame);

			// ── 管道锁定 ──
			ball.pipe_stable_frames =
			    getInt(raw_config, "vision.ball.pipe_stable_frames",
			           ball.pipe_stable_frames, 3, 120, result,
			           &ball.source_pipe_stable_frames);

			ball.pipe_lost_timeout_frames = getInt(
			    raw_config, "vision.ball.pipe_lost_timeout_frames",
			    ball.pipe_lost_timeout_frames, 5, 600, result,
			    &ball.source_pipe_lost_timeout_frames);

			// ── 透视展开 ──
			ball.pipe_warp_width =
			    getInt(raw_config, "vision.ball.pipe_warp_width",
			           ball.pipe_warp_width, 10, 4096, result,
			           &ball.source_pipe_warp_width);

			ball.pipe_warp_height =
			    getInt(raw_config, "vision.ball.pipe_warp_height",
			           ball.pipe_warp_height, 10, 2160, result,
			           &ball.source_pipe_warp_height);

			ball.pipe_inner_margin_x_ratio = getDouble(
			    raw_config, "vision.ball.pipe_inner_margin_x_ratio",
			    ball.pipe_inner_margin_x_ratio, 0.0, 0.45, result,
			    &ball.source_pipe_inner_margin_x_ratio);

			ball.pipe_inner_margin_y_ratio = getDouble(
			    raw_config, "vision.ball.pipe_inner_margin_y_ratio",
			    ball.pipe_inner_margin_y_ratio, 0.0, 0.45, result,
			    &ball.source_pipe_inner_margin_y_ratio);

			// ── 毫米换算 ──
			ball.pipe_length_mm =
			    getDouble(raw_config, "vision.ball.pipe_length_mm",
			              ball.pipe_length_mm, 1.0, 10000.0, result,
			              &ball.source_pipe_length_mm);

			// ── 钢球约束 ──
			ball.ball_min_aspect =
			    getDouble(raw_config, "vision.ball.ball_min_aspect",
			              ball.ball_min_aspect, 0.1, 1.0, result,
			              &ball.source_ball_min_aspect);

			ball.ball_max_aspect =
			    getDouble(raw_config, "vision.ball.ball_max_aspect",
			              ball.ball_max_aspect, 1.0, 10.0, result,
			              &ball.source_ball_max_aspect);

			ball.ball_min_local_contrast = getDouble(
			    raw_config, "vision.ball.ball_min_local_contrast",
			    ball.ball_min_local_contrast, 0.0, 255.0, result,
			    &ball.source_ball_min_local_contrast);

			ball.ball_max_centerline_distance_px = getDouble(
			    raw_config,
			    "vision.ball.ball_max_centerline_distance_px",
			    ball.ball_max_centerline_distance_px, 0.1, 500.0,
			    result, &ball.source_ball_max_centerline_distance_px);

			// ── HoughCircles ──
			ball.hough_dp = getDouble(
			    raw_config, "vision.ball.hough_dp", ball.hough_dp, 1.0,
			    2.0, result, &ball.source_hough_dp);
			ball.hough_min_distance =
			    getDouble(raw_config, "vision.ball.hough_min_distance",
			              ball.hough_min_distance, 5.0, 500.0, result,
			              &ball.source_hough_min_distance);
			ball.hough_param1 =
			    getDouble(raw_config, "vision.ball.hough_param1",
			              ball.hough_param1, 10.0, 255.0, result,
			              &ball.source_hough_param1);
			ball.hough_param2 =
			    getDouble(raw_config, "vision.ball.hough_param2",
			              ball.hough_param2, 1.0, 255.0, result,
			              &ball.source_hough_param2);

			ball.ball_min_radius =
			    getInt(raw_config, "vision.ball.ball_min_radius",
			           ball.ball_min_radius, 1, 500, result,
			           &ball.source_ball_min_radius);
			ball.ball_max_radius =
			    getInt(raw_config, "vision.ball.ball_max_radius",
			           ball.ball_max_radius, ball.ball_min_radius + 1,
			           500, result, &ball.source_ball_max_radius);
			ball.ball_expected_radius = getDouble(
			    raw_config, "vision.ball.ball_expected_radius",
			    ball.ball_expected_radius, 1.0, 500.0, result,
			    &ball.source_ball_expected_radius);

			ball.ball_min_center_y_ratio = getDouble(
			    raw_config, "vision.ball.ball_min_center_y_ratio",
			    ball.ball_min_center_y_ratio, 0.0, 1.0, result,
			    &ball.source_ball_min_center_y_ratio);
			ball.ball_max_center_y_ratio = getDouble(
			    raw_config, "vision.ball.ball_max_center_y_ratio",
			    ball.ball_max_center_y_ratio, 0.0, 1.0, result,
			    &ball.source_ball_max_center_y_ratio);
			ball.ball_expected_center_y_ratio = getDouble(
			    raw_config, "vision.ball.ball_expected_center_y_ratio",
			    ball.ball_expected_center_y_ratio, 0.0, 1.0, result,
			    &ball.source_ball_expected_center_y_ratio);

			ball.ball_max_inner_gray =
			    getDouble(raw_config, "vision.ball.ball_max_inner_gray",
			              ball.ball_max_inner_gray, 0.0, 255.0, result,
			              &ball.source_ball_max_inner_gray);
			ball.ball_min_ring_contrast = getDouble(
			    raw_config, "vision.ball.ball_min_ring_contrast",
			    ball.ball_min_ring_contrast, -255.0, 255.0, result,
			    &ball.source_ball_min_ring_contrast);
			ball.ball_good_ring_contrast = getDouble(
			    raw_config, "vision.ball.ball_good_ring_contrast",
			    ball.ball_good_ring_contrast, 0.1, 255.0, result,
			    &ball.source_ball_good_ring_contrast);
			ball.ball_min_quality =
			    getDouble(raw_config, "vision.ball.ball_min_quality",
			              ball.ball_min_quality, 0.0, 1.0, result,
			              &ball.source_ball_min_quality);

			// ── alpha-beta 跟踪器 ──
			ball.tracker_alpha =
			    getDouble(raw_config, "vision.ball.tracker_alpha",
			              ball.tracker_alpha, 0.01, 1.0, result,
			              &ball.source_tracker_alpha);
			ball.tracker_beta =
			    getDouble(raw_config, "vision.ball.tracker_beta",
			              ball.tracker_beta, 0.001, 1.0, result,
			              &ball.source_tracker_beta);
			ball.tracker_gate_ratio =
			    getDouble(raw_config, "vision.ball.tracker_gate_ratio",
			              ball.tracker_gate_ratio, 0.01, 1.0, result,
			              &ball.source_tracker_gate_ratio);
			ball.tracker_gate_growth_per_lost_frame = getDouble(
			    raw_config,
			    "vision.ball.tracker_gate_growth_per_lost_frame",
			    ball.tracker_gate_growth_per_lost_frame, 0.0, 1.0,
			    result,
			    &ball.source_tracker_gate_growth_per_lost_frame);
			ball.tracker_max_gate_ratio = getDouble(
			    raw_config, "vision.ball.tracker_max_gate_ratio",
			    ball.tracker_max_gate_ratio, 0.01, 1.0, result,
			    &ball.source_tracker_max_gate_ratio);
			ball.tracker_max_speed_ratio_per_second = getDouble(
			    raw_config,
			    "vision.ball.tracker_max_speed_ratio_per_second",
			    ball.tracker_max_speed_ratio_per_second, 0.01, 10.0,
			    result,
			    &ball.source_tracker_max_speed_ratio_per_second);
			ball.tracker_max_predict_frames = getInt(
			    raw_config, "vision.ball.tracker_max_predict_frames",
			    ball.tracker_max_predict_frames, 1, 120, result,
			    &ball.source_tracker_max_predict_frames);
			ball.tracker_global_reacquire_frames = getInt(
			    raw_config,
			    "vision.ball.tracker_global_reacquire_frames",
			    ball.tracker_global_reacquire_frames, 1, 600, result,
			    &ball.source_tracker_global_reacquire_frames);
			ball.reacquire_confirm_frames = getInt(
			    raw_config, "vision.ball.reacquire_confirm_frames",
			    ball.reacquire_confirm_frames, 1, 10, result,
			    &ball.source_reacquire_confirm_frames);

			ball.acquire_confirm_frames =
			    getInt(raw_config, "vision.ball.acquire_confirm_frames",
			           ball.acquire_confirm_frames, 1, 10, result,
			           &ball.source_acquire_confirm_frames);

			ball.zero_position_ratio =
			    getDouble(raw_config, "vision.ball.zero_position_ratio",
			              ball.zero_position_ratio, 0.0, 1.0, result,
			              &ball.source_zero_position_ratio);

			ball.video_output =
			    getString(raw_config, "vision.ball.video_output",
			              ball.video_output, true, result,
			              &ball.source_video_output);
		}

		// ── ball_ncnn 双模型配置 ──
		{
			auto& bn = config.vision.ball_ncnn;

			bn.enabled = getBool(raw_config, "vision.ball_ncnn.enabled",
			                     bn.enabled, result, &bn.source_enabled);

			bn.full_model_param = getString(
			    raw_config, "vision.ball_ncnn.full_model_param",
			    bn.full_model_param, false, result,
			    &bn.source_full_model_param);

			bn.center_model_param = getString(
			    raw_config, "vision.ball_ncnn.center_model_param",
			    bn.center_model_param, false, result,
			    &bn.source_center_model_param);

			bn.full_input_width =
			    getInt(raw_config, "vision.ball_ncnn.full_input_width",
			           bn.full_input_width, 32, 1920, result,
			           &bn.source_full_input_width);

			bn.full_input_height =
			    getInt(raw_config, "vision.ball_ncnn.full_input_height",
			           bn.full_input_height, 32, 1920, result,
			           &bn.source_full_input_height);

			bn.center_input_width =
			    getInt(raw_config,
			           "vision.ball_ncnn.center_input_width",
			           bn.center_input_width, 32, 1920, result,
			           &bn.source_center_input_width);

			bn.center_input_height =
			    getInt(raw_config,
			           "vision.ball_ncnn.center_input_height",
			           bn.center_input_height, 32, 1920, result,
			           &bn.source_center_input_height);

			bn.full_src_width =
			    getInt(raw_config, "vision.ball_ncnn.full_src_width",
			           bn.full_src_width, 1, 4096, result,
			           &bn.source_full_src_width);

			bn.full_src_height =
			    getInt(raw_config, "vision.ball_ncnn.full_src_height",
			           bn.full_src_height, 1, 2160, result,
			           &bn.source_full_src_height);

			bn.center_src_width =
			    getInt(raw_config, "vision.ball_ncnn.center_src_width",
			           bn.center_src_width, 1, 4096, result,
			           &bn.source_center_src_width);

			bn.center_src_height =
			    getInt(raw_config, "vision.ball_ncnn.center_src_height",
			           bn.center_src_height, 1, 2160, result,
			           &bn.source_center_src_height);

			bn.pipe_center_x =
			    getInt(raw_config, "vision.ball_ncnn.pipe_center_x",
			           bn.pipe_center_x, 0, 4096, result,
			           &bn.source_pipe_center_x);

			bn.pipe_center_y =
			    getInt(raw_config, "vision.ball_ncnn.pipe_center_y",
			           bn.pipe_center_y, 0, 2160, result,
			           &bn.source_pipe_center_y);

			bn.edge_guard_px =
			    getInt(raw_config, "vision.ball_ncnn.edge_guard_px",
			           bn.edge_guard_px, 0, 200, result,
			           &bn.source_edge_guard_px);

			bn.lost_frames_to_reacquire =
			    getInt(raw_config,
			           "vision.ball_ncnn.lost_frames_to_reacquire",
			           bn.lost_frames_to_reacquire, 1, 60, result,
			           &bn.source_lost_frames_to_reacquire);

			bn.stable_frames_to_center =
			    getInt(raw_config,
			           "vision.ball_ncnn.stable_frames_to_center",
			           bn.stable_frames_to_center, 1, 60, result,
			           &bn.source_stable_frames_to_center);

			bn.calibration_frames =
			    getInt(raw_config,
			           "vision.ball_ncnn.calibration_frames",
			           bn.calibration_frames, 1, 60, result,
			           &bn.source_calibration_frames);

			bn.calibration_timeout_ms =
			    getInt(raw_config,
			           "vision.ball_ncnn.calibration_timeout_ms",
			           bn.calibration_timeout_ms, 100, 10000, result,
			           &bn.source_calibration_timeout_ms);

			bn.initial_center_limit_mm =
			    getDouble(raw_config,
			              "vision.ball_ncnn.initial_center_limit_mm",
			              bn.initial_center_limit_mm, 0.0, 100.0,
			              result, &bn.source_initial_center_limit_mm);

			bn.minimum_confidence =
			    getDouble(raw_config,
			              "vision.ball_ncnn.minimum_confidence",
			              bn.minimum_confidence, 0.0, 1.0, result,
			              &bn.source_minimum_confidence);

			bn.num_threads =
			    getInt(raw_config, "vision.ball_ncnn.num_threads",
			           bn.num_threads, 1, 8, result,
			           &bn.source_num_threads);

			bn.use_fp16_storage =
			    getBool(raw_config,
			            "vision.ball_ncnn.use_fp16_storage",
			            bn.use_fp16_storage, result,
			            &bn.source_use_fp16_storage);

			bn.use_fp16_arithmetic =
			    getBool(raw_config,
			            "vision.ball_ncnn.use_fp16_arithmetic",
			            bn.use_fp16_arithmetic, result,
			            &bn.source_use_fp16_arithmetic);

			// 轴标定
			bn.axis_calibration.image_right_sign =
			    getInt(raw_config,
			           "vision.ball_ncnn.axis_calibration.image_right_sign",
			           bn.axis_calibration.image_right_sign, -1, 1,
			           result, nullptr);

			// 标定点：逗号分隔的浮点字符串
			std::string pixels_str = getString(
			    raw_config,
			    "vision.ball_ncnn.axis_calibration.pixels",
			    "", true, result, nullptr);

			std::string positions_str = getString(
			    raw_config,
			    "vision.ball_ncnn.axis_calibration.positions_mm",
			    "", true, result, nullptr);

			if(!pixels_str.empty() && !positions_str.empty())
			{
				// 解析逗号分隔列表
				auto splitDoubles =
				    [](const std::string& s) -> std::vector<double> {
					std::vector<double> out;
					std::istringstream iss(s);
					std::string token;
					while(std::getline(iss, token, ','))
					{
						char* end = nullptr;
						double v = std::strtod(token.c_str(), &end);
						if(end != token.c_str()
						   && std::isfinite(v))
							out.push_back(v);
					}
					return out;
				};

				auto pixels = splitDoubles(pixels_str);
				auto positions = splitDoubles(positions_str);

				std::size_t count =
				    std::min(pixels.size(), positions.size());

				if(count >= 2)
				{
					bn.axis_calibration.points.clear();
					for(std::size_t i = 0; i < count; ++i)
					{
						bn.axis_calibration.points.push_back(
						    {pixels[i], positions[i]});
					}
				}
				else if(!pixels_str.empty() || !positions_str.empty())
				{
					addMessage(
					    result, etest::ConfigMessageLevel::ERROR,
					    "axis_calibration.pixels and positions_mm "
					    "need at least 2 points; keeping defaults");
				}
			}
		}

		config.uart.device =
		    getString(raw_config, "uart.device", config.uart.device,
		              false, result, &config.uart.source_device);

		config.uart.baudrate =
		    getInt(raw_config, "uart.baudrate", config.uart.baudrate,
		           1200, 3000000, result, &config.uart.source_baudrate);

		config.uart.timeout_ms = getInt(
		    raw_config, "uart.timeout_ms", config.uart.timeout_ms, 0,
		    10000, result, &config.uart.source_timeout_ms);

		config.uart.write_timeout_ms =
		    getInt(raw_config, "uart.write_timeout_ms",
		           config.uart.write_timeout_ms, 0, 60000, result,
		           &config.uart.source_write_timeout_ms);

		config.uart.reconnect_interval_ms =
		    getInt(raw_config, "uart.reconnect_interval_ms",
		           config.uart.reconnect_interval_ms, 50, 60000, result,
		           &config.uart.source_reconnect_interval_ms);

		config.uart.auto_reconnect =
		    getBool(raw_config, "uart.auto_reconnect",
		            config.uart.auto_reconnect, result,
		            &config.uart.source_auto_reconnect);

		config.uart.max_line_length =
		    getInt(raw_config, "uart.max_line_length",
		           config.uart.max_line_length, 32, 4096, result,
		           &config.uart.source_max_line_length);

		config.uart.queue_capacity =
		    getInt(raw_config, "uart.queue_capacity",
		           config.uart.queue_capacity, 1, 65536, result,
		           &config.uart.source_queue_capacity);

		config.uart.handshake_timeout_ms =
		    getInt(raw_config, "uart.handshake_timeout_ms",
		           config.uart.handshake_timeout_ms, 100, 10000, result,
		           &config.uart.source_handshake_timeout_ms);

		config.uart.heartbeat_interval_ms =
		    getInt(raw_config, "uart.heartbeat_interval_ms",
		           config.uart.heartbeat_interval_ms, 100, 10000,
		           result, &config.uart.source_heartbeat_interval_ms);

		config.uart.heartbeat_timeout_ms =
		    getInt(raw_config, "uart.heartbeat_timeout_ms",
		           config.uart.heartbeat_timeout_ms, 300, 60000, result,
		           &config.uart.source_heartbeat_timeout_ms);

		config.uart.protocol_version =
		    getInt(raw_config, "uart.protocol_version",
		           config.uart.protocol_version, 4, 5, result,
		           &config.uart.source_protocol_version);

		config.uart.protocol_version_major =
		    getInt(raw_config, "uart.protocol_version_major",
		           config.uart.protocol_version_major, 1, 10, result,
		           &config.uart.source_protocol_version_major);

		config.uart.protocol_version_minor =
		    getInt(raw_config, "uart.protocol_version_minor",
		           config.uart.protocol_version_minor, 0, 10, result,
		           &config.uart.source_protocol_version_minor);

		config.record.enabled =
		    getBool(raw_config, "record.enabled", config.record.enabled,
		            result, &config.record.source_enabled);

		config.record.save_raw = getBool(
		    raw_config, "record.save_raw", config.record.save_raw,
		    result, &config.record.source_save_raw);

		config.record.save_debug = getBool(
		    raw_config, "record.save_debug", config.record.save_debug,
		    result, &config.record.source_save_debug);

		config.record.directory = getString(
		    raw_config, "record.directory", config.record.directory,
		    false, result, &config.record.source_directory);

		config.record.fourcc =
		    getString(raw_config, "record.fourcc", config.record.fourcc,
		              false, result, &config.record.source_fourcc);

		config.record.fps =
		    getInt(raw_config, "record.fps", config.record.fps, 1, 240,
		           result, &config.record.source_fps);

		config.record.segment_seconds =
		    getInt(raw_config, "record.segment_seconds",
		           config.record.segment_seconds, 10, 3600, result,
		           &config.record.source_segment_seconds);

		config.search.show_preview =
		    getBool(raw_config, "search.show_preview",
		            config.search.show_preview, result,
		            &config.search.source_show_preview);

		config.search.enable_nn = getBool(
		    raw_config, "search.enable_nn", config.search.enable_nn,
		    result, &config.search.source_enable_nn);

		config.search.model_path = getString(
		    raw_config, "search.model_path", config.search.model_path,
		    false, result, &config.search.source_model_path);

		config.search.class_names_path =
		    getString(raw_config, "search.class_names_path",
		              config.search.class_names_path, true, result,
		              &config.search.source_class_names_path);

		config.search.nn_confidence_threshold = getDouble(
		    raw_config, "search.nn_confidence_threshold",
		    config.search.nn_confidence_threshold, 0.0, 1.0, result,
		    &config.search.source_nn_confidence_threshold);

		config.search.nn_nms_threshold =
		    getDouble(raw_config, "search.nn_nms_threshold",
		              config.search.nn_nms_threshold, 0.0, 1.0, result,
		              &config.search.source_nn_nms_threshold);

		// ── 检测器选择 ──
		config.search.detector = getString(
		    raw_config, "search.detector", config.search.detector,
		    false, result, &config.search.source_detector);
		if(config.search.detector != "traditional"
		   && config.search.detector != "yolo")
		{
			addMessage(result, etest::ConfigMessageLevel::ERROR,
			           "search.detector must be \"traditional\" or "
			           "\"yolo\"; keeping \""
			               + config.search.detector + "\"");
		}

		// ── YOLO 后端选择 ──
		config.search.yolo_backend = getString(
		    raw_config, "search.yolo_backend",
		    config.search.yolo_backend, false, result,
		    &config.search.source_yolo_backend);
		if(config.search.yolo_backend != "opencv"
		   && config.search.yolo_backend != "ncnn")
		{
			addMessage(result, etest::ConfigMessageLevel::ERROR,
			           "search.yolo_backend must be \"opencv\" or "
			           "\"ncnn\"; keeping \""
			               + config.search.yolo_backend + "\"");
		}

		// ── NCNN 模型路径 ──
		config.search.ncnn_param_path = getString(
		    raw_config, "search.ncnn_param_path",
		    config.search.ncnn_param_path, false, result,
		    &config.search.source_ncnn_param_path);

		// ── 线程数 ──
		config.search.nn_threads =
		    getInt(raw_config, "search.nn_threads",
		           config.search.nn_threads, 1, 8, result,
		           &config.search.source_nn_threads);

		// ── NCNN 高级选项 ──
		config.search.ncnn_use_fp16_storage =
		    getBool(raw_config, "search.ncnn_use_fp16_storage",
		            config.search.ncnn_use_fp16_storage, result,
		            &config.search.source_ncnn_use_fp16_storage);
		config.search.ncnn_use_fp16_arithmetic =
		    getBool(raw_config, "search.ncnn_use_fp16_arithmetic",
		            config.search.ncnn_use_fp16_arithmetic, result,
		            &config.search.source_ncnn_use_fp16_arithmetic);
		config.search.ncnn_use_vulkan =
		    getBool(raw_config, "search.ncnn_use_vulkan",
		            config.search.ncnn_use_vulkan, result,
		            &config.search.source_ncnn_use_vulkan);
		config.search.ncnn_input_blob = getString(
		    raw_config, "search.ncnn_input_blob",
		    config.search.ncnn_input_blob, false, result,
		    &config.search.source_ncnn_input_blob);
		config.search.ncnn_output_blob = getString(
		    raw_config, "search.ncnn_output_blob",
		    config.search.ncnn_output_blob, false, result,
		    &config.search.source_ncnn_output_blob);

		// ── YOLO 输入尺寸 ──
		config.search.nn_input_width =
		    getInt(raw_config, "search.nn_input_width",
		           config.search.nn_input_width, 32, 1920, result,
		           &config.search.source_nn_input_width);
		config.search.nn_input_height =
		    getInt(raw_config, "search.nn_input_height",
		           config.search.nn_input_height, 32, 1920, result,
		           &config.search.source_nn_input_height);

		// ── 原点标定 ──
		config.search.zero_sample_count =
		    getInt(raw_config, "search.zero_sample_count",
		           config.search.zero_sample_count, 3, 500, result,
		           &config.search.source_zero_sample_count);
		config.search.zero_max_jitter_px =
		    getDouble(raw_config, "search.zero_max_jitter_px",
		              config.search.zero_max_jitter_px, 0.1, 100.0,
		              result, &config.search.source_zero_max_jitter_px);
		config.search.zero_max_wait_frames =
		    getInt(raw_config, "search.zero_max_wait_frames",
		           config.search.zero_max_wait_frames, 10, 6000, result,
		           &config.search.source_zero_max_wait_frames);
		config.search.zero_min_confidence = getDouble(
		    raw_config, "search.zero_min_confidence",
		    config.search.zero_min_confidence, 0.0, 1.0, result,
		    &config.search.source_zero_min_confidence);

		// ── 跟踪与滤波 ──
		config.search.max_target_jump_px =
		    getDouble(raw_config, "search.max_target_jump_px",
		              config.search.max_target_jump_px, 1.0, 500.0,
		              result, &config.search.source_max_target_jump_px);
		config.search.position_filter_alpha = getDouble(
		    raw_config, "search.position_filter_alpha",
		    config.search.position_filter_alpha, 0.01, 1.0, result,
		    &config.search.source_position_filter_alpha);
		config.search.max_hold_frames =
		    getInt(raw_config, "search.max_hold_frames",
		           config.search.max_hold_frames, 0, 30, result,
		           &config.search.source_max_hold_frames);
		config.search.lost_confirm_frames =
		    getInt(raw_config, "search.lost_confirm_frames",
		           config.search.lost_confirm_frames, 1, 60, result,
		           &config.search.source_lost_confirm_frames);
		config.search.reacquire_confirm_frames = getInt(
		    raw_config, "search.reacquire_confirm_frames",
		    config.search.reacquire_confirm_frames, 1, 10, result,
		    &config.search.source_reacquire_confirm_frames);

		// ── 物理换算 ──
		config.search.mm_per_pixel =
		    getDouble(raw_config, "search.mm_per_pixel",
		              config.search.mm_per_pixel, 0.001, 100.0, result,
		              &config.search.source_mm_per_pixel);
		config.search.invert_offset =
		    getBool(raw_config, "search.invert_offset",
		            config.search.invert_offset, result,
		            &config.search.source_invert_offset);

		// ── 通信周期 ──
		config.search.result_send_interval_ms = getInt(
		    raw_config, "search.result_send_interval_ms",
		    config.search.result_send_interval_ms, 10, 500, result,
		    &config.search.source_result_send_interval_ms);
		config.search.calib_status_interval_ms = getInt(
		    raw_config, "search.calib_status_interval_ms",
		    config.search.calib_status_interval_ms, 50, 2000, result,
		    &config.search.source_calib_status_interval_ms);
		config.search.vsession_retry_interval_ms = getInt(
		    raw_config, "search.vsession_retry_interval_ms",
		    config.search.vsession_retry_interval_ms, 100, 5000, result,
		    &config.search.source_vsession_retry_interval_ms);

		// ── 模型恢复 ──
		config.search.max_inference_errors =
		    getInt(raw_config, "search.max_inference_errors",
		           config.search.max_inference_errors, 1, 100, result,
		           &config.search.source_max_inference_errors);
		config.search.model_reload_interval_ms = getInt(
		    raw_config, "search.model_reload_interval_ms",
		    config.search.model_reload_interval_ms, 500, 60000, result,
		    &config.search.source_model_reload_interval_ms);

		// ── VSESSION ──
		config.search.camera_id = getString(
		    raw_config, "search.camera_id", config.search.camera_id,
		    false, result, &config.search.source_camera_id);
		config.search.nominal_fps = getInt(
		    raw_config, "search.nominal_fps", config.search.nominal_fps,
		    1, 240, result, &config.search.source_nominal_fps);
	}

	// 模式名称校验

	bool isValidModeName(const std::string& name)
	{
		if(name.empty())
		{
			return false;
		}

		// 拒绝路径分隔符和父目录引用
		if(name.find('/') != std::string::npos)
		{
			return false;
		}

		if(name.find('\\') != std::string::npos)
		{
			return false;
		}

		if(name.find("..") != std::string::npos)
		{
			return false;
		}

		// 绝对路径
		if(name.front() == '/')
		{
			return false;
		}

		// 白名单
		if(name != "competition" && name != "debug")
		{
			return false;
		}

		return true;
	}

	// 加载单个配置文件并逐层应用到 config

	bool loadFileLayer(etest::AppConfig& config,
	                   const std::string& path,
	                   etest::ConfigLoadResult& result)
	{
		try
		{
			std::ifstream input(path);

			if(!input.is_open())
			{
				result.failed_files.push_back(path);

				addMessage(result, etest::ConfigMessageLevel::ERROR,
				           "cannot open " + path + "; skipping");

				return false;
			}

			const RawConfig raw_config = parseFile(input, path, result);

			warnUnknownKeys(raw_config, result);

			extractAppConfig(config, raw_config, result);

			result.loaded_files.push_back(path);
			result.file_loaded = true;

			return true;
		}
		catch(const std::exception& error)
		{
			result.failed_files.push_back(path);

			addMessage(result, etest::ConfigMessageLevel::ERROR,
			           std::string("exception while reading ") + path
			               + ": " + error.what() + "; skipping");

			return false;
		}
		catch(...)
		{
			result.failed_files.push_back(path);

			addMessage(result, etest::ConfigMessageLevel::ERROR,
			           "unknown exception while reading " + path
			               + "; skipping");

			return false;
		}
	}

	// 加载模式文件并覆盖 config

	bool applyModeFile(etest::AppConfig& config,
	                   const std::string& path,
	                   etest::ConfigLoadResult& result)
	{
		try
		{
			std::ifstream input(path);

			if(!input.is_open())
			{
				addMessage(
				    result, etest::ConfigMessageLevel::ERROR,
				    "cannot open mode file " + path
				        + "; mode not applied, using base config");

				return false;
			}

			const RawConfig raw_config = parseFile(input, path, result);

			// 检测模式文件中是否试图设置 mode.enabled 或 mode.name
			const RawValue* mode_enabled_raw =
			    findValue(raw_config, "mode.enabled");

			if(mode_enabled_raw != nullptr)
			{
				addMessage(result, etest::ConfigMessageLevel::WARN,
				           "mode.enabled in mode file " + path + ":"
				               + std::to_string(mode_enabled_raw->line)
				               + " is ignored");
			}

			const RawValue* mode_name_raw =
			    findValue(raw_config, "mode.name");

			if(mode_name_raw != nullptr)
			{
				addMessage(result, etest::ConfigMessageLevel::WARN,
				           "mode.name in mode file " + path + ":"
				               + std::to_string(mode_name_raw->line)
				               + " is ignored");
			}

			// 移除 mode.enabled 和 mode.name 后再应用
			RawConfig filtered = raw_config;
			filtered.erase("mode.enabled");
			filtered.erase("mode.name");

			warnUnknownKeys(filtered, result);

			extractAppConfig(config, filtered, result);

			return true;
		}
		catch(const std::exception& error)
		{
			addMessage(
			    result, etest::ConfigMessageLevel::ERROR,
			    std::string("exception while applying mode file ")
			        + path + ": " + error.what()
			        + "; mode not applied");

			return false;
		}
		catch(...)
		{
			addMessage(result, etest::ConfigMessageLevel::ERROR,
			           "unknown exception while applying mode file "
			               + path + "; mode not applied");

			return false;
		}
	}

	// 公共入口：从目录加载

	etest::ConfigLoadResult loadAppConfigInternal(
	    const std::string& config_dir)
	{
		etest::ConfigLoadResult result;
		result.config_dir = config_dir;
		result.mode_name = "";

		// 从 C++ 默认值开始（protocol_version=4）
		etest::AppConfig config;

		// 第1层：main.toml（仅 [mode]）
		loadFileLayer(config, config_dir + "/main.toml", result);

		// 第2层：runtime.toml
		loadFileLayer(config, config_dir + "/runtime.toml", result);

		// 第3层：logger.toml
		loadFileLayer(config, config_dir + "/logger.toml", result);

		// 第4层：camera.toml
		loadFileLayer(config, config_dir + "/camera.toml", result);

		// 第5层：vision.toml
		loadFileLayer(config, config_dir + "/vision.toml", result);

		// 第6层：uart.toml
		loadFileLayer(config, config_dir + "/uart.toml", result);

		// 第7层：record.toml
		loadFileLayer(config, config_dir + "/record.toml", result);

		// 第8层：search.toml
		loadFileLayer(config, config_dir + "/search.toml", result);

		// 模式处理
		result.mode_name = config.mode.name;

		if(config.mode.enabled)
		{
			if(!isValidModeName(config.mode.name))
			{
				addMessage(
				    result, etest::ConfigMessageLevel::ERROR,
				    "invalid mode name \"" + config.mode.name
				        + "\"; mode disabled, using base config");

				config.mode.enabled = false;
				result.mode_applied = false;
			}
			else
			{
				const std::string mode_path =
				    config_dir + "/modes/" + config.mode.name + ".toml";

				result.mode_file_path = mode_path;

				const bool applied =
				    applyModeFile(config, mode_path, result);

				result.mode_applied = applied;

				if(applied)
				{
					addMessage(result, etest::ConfigMessageLevel::WARN,
					           "applied mode " + config.mode.name
					               + " from " + mode_path);
				}
			}
		}
		else
		{
			result.mode_applied = false;
		}

		// 后处理：headless 强制规则
		if(config.runtime.headless)
		{
			if(config.search.show_preview)
			{
				config.search.show_preview = false;

				addMessage(result, etest::ConfigMessageLevel::WARN,
				           "runtime.headless=true forces "
				           "search.show_preview=false");
			}

			if(config.runtime.allow_keyboard_exit)
			{
				config.runtime.allow_keyboard_exit = false;

				addMessage(result, etest::ConfigMessageLevel::WARN,
				           "runtime.headless=true forces "
				           "runtime.allow_keyboard_exit=false");
			}
		}

		// 交叉校验：心跳超时必须大于心跳间隔
		if(config.uart.heartbeat_timeout_ms
		   <= config.uart.heartbeat_interval_ms)
		{
			config.uart.heartbeat_timeout_ms =
			    std::min(60000, config.uart.heartbeat_interval_ms * 3);

			addMessage(
			    result, etest::ConfigMessageLevel::ERROR,
			    "heartbeat_timeout_ms must be greater than "
			    "heartbeat_interval_ms; adjusted to "
			        + std::to_string(config.uart.heartbeat_timeout_ms));
		}

		result.config = std::move(config);

		return result;
	}

} // namespace

// 公共接口实现

namespace etest
{

	ConfigLoadResult ConfigLoader::load(
	    const std::string& path) noexcept
	{
		ConfigLoadResult result;

		try
		{
			std::ifstream input(path);

			if(!input.is_open())
			{
				addMessage(result, ConfigMessageLevel::ERROR,
				           "cannot open " + path
				               + "; all modules use default values");

				return result;
			}

			result.file_loaded = true;

			const RawConfig raw_config = parseFile(input, path, result);

			warnUnknownKeys(raw_config, result);

			extractAppConfig(result.config, raw_config, result);

			return result;
		}
		catch(const std::exception& error)
		{
			addMessage(result, ConfigMessageLevel::ERROR,
			           std::string("unexpected config exception: ")
			               + error.what()
			               + "; remaining values use defaults");

			return result;
		}
		catch(...)
		{
			addMessage(
			    result, ConfigMessageLevel::ERROR,
			    "unknown config exception; remaining values use defaults");

			return result;
		}
	}

	ConfigLoadResult ConfigLoader::loadMultiple(
	    const std::vector<std::string>& paths) noexcept
	{
		ConfigLoadResult result;

		bool any_loaded = false;

		for(const auto& path: paths)
		{
			const bool loaded =
			    loadFileLayer(result.config, path, result);

			if(loaded)
			{
				any_loaded = true;
			}
		}

		result.file_loaded = any_loaded;

		return result;
	}

	ConfigLoadResult loadAppConfigFromDir(const std::string& config_dir)
	{
		return loadAppConfigInternal(config_dir);
	}

	ConfigLoadResult loadAppConfigFromMainFile(
	    const std::string& main_toml_path)
	{
		// 提取 main.toml 所在目录
		std::string dir = main_toml_path;

		// 找到最后一个 '/'
		const auto pos = dir.rfind('/');

		if(pos != std::string::npos)
		{
			dir = dir.substr(0, pos);
		}
		else
		{
			dir = ".";
		}

		if(dir.empty())
		{
			dir = ".";
		}

		return loadAppConfigInternal(dir);
	}

	// to_string 实现

	std::string ModeConfig::to_string() const
	{
		return "enabled=" + std::string(enabled ? "true" : "false")
		    + ", name=" + name;
	}

	std::string RuntimeConfig::to_string() const
	{
		return "headless=" + std::string(headless ? "true" : "false")
		    + ", allow_keyboard_exit="
		    + std::string(allow_keyboard_exit ? "true" : "false")
		    + ", enable_self_check="
		    + std::string(enable_self_check ? "true" : "false")
		    + ", enable_auto_recovery="
		    + std::string(enable_auto_recovery ? "true" : "false")
		    + ", camera_retry_interval_ms="
		    + std::to_string(camera_retry_interval_ms)
		    + ", uart_retry_interval_ms="
		    + std::to_string(uart_retry_interval_ms);
	}

	std::string LoggerConfig::to_string() const
	{
		return "directory=" + directory
		    + ", file=" + std::string(file ? "true" : "false")
		    + ", terminal=" + std::string(terminal ? "true" : "false")
		    + ", min_level=" +
		    [](LogLevel level) {
			    switch(level)
			    {
			    case LogLevel::DEBUG:
				    return "DEBUG";

			    case LogLevel::INFO:
				    return "INFO";

			    case LogLevel::WARN:
				    return "WARN";

			    case LogLevel::ERROR:
				    return "ERROR";

			    case LogLevel::FATAL:
				    return "FATAL";
			    }

			    return "UNKNOWN";
		    }(min_level)
		    + ", flush_each_write="
		    + std::string(flush_each_write ? "true" : "false")
		    + ", throttle_interval_ms="
		    + std::to_string(throttle_interval_ms);
	}

	std::string CameraConfig::to_string() const
	{
		return "source=" + source + ", width=" + std::to_string(width)
		    + ", height=" + std::to_string(height)
		    + ", fps=" + std::to_string(fps) + ", fourcc=" + fourcc;
	}

	std::string BallConfig::to_string() const
	{
		return "work=" + std::to_string(work_width) + "x"
		    + std::to_string(work_height)
		    + ", pipe_roi=" + std::to_string(pipe_search_roi_x) + ","
		    + std::to_string(pipe_search_roi_y) + ","
		    + std::to_string(pipe_search_roi_w) + "x"
		    + std::to_string(pipe_search_roi_h) + ", axis=("
		    + std::to_string(axis_x1) + "," + std::to_string(axis_y1)
		    + ")->(" + std::to_string(axis_x2) + ","
		    + std::to_string(axis_y2)
		    + "), axis_cm=" + std::to_string(axis_length_cm)
		    + ", brown_h=" + std::to_string(brown_h_min) + "-"
		    + std::to_string(brown_h_max)
		    + ", brown_s=" + std::to_string(brown_s_min) + "-"
		    + std::to_string(brown_s_max)
		    + ", brown_v=" + std::to_string(brown_v_min) + "-"
		    + std::to_string(brown_v_max) + ", pipe_close_kernel="
		    + std::to_string(pipe_close_kernel_w) + "x"
		    + std::to_string(pipe_close_kernel_h)
		    + ", pipe_open_kernel=" + std::to_string(pipe_open_kernel)
		    + ", pipe_min_area=" + std::to_string(pipe_min_area_ratio)
		    + ", pipe_min_aspect="
		    + std::to_string(pipe_min_aspect_ratio)
		    + ", pipe_min_fill=" + std::to_string(pipe_min_fill_ratio)
		    + ", pipe_h_angle_max="
		    + std::to_string(pipe_horizontal_angle_max)
		    + ", pipe_geom_alpha=" + std::to_string(pipe_geometry_alpha)
		    + ", pipe_update_each="
		    + std::to_string(pipe_update_each_frame)
		    + ", pipe_stable=" + std::to_string(pipe_stable_frames)
		    + ", pipe_lost_timeout="
		    + std::to_string(pipe_lost_timeout_frames)
		    + ", warp=" + std::to_string(pipe_warp_width) + "x"
		    + std::to_string(pipe_warp_height) + ", inner_margin_xy="
		    + std::to_string(pipe_inner_margin_x_ratio) + "/"
		    + std::to_string(pipe_inner_margin_y_ratio)
		    + ", pipe_len_mm=" + std::to_string(pipe_length_mm)
		    + ", bg_kernel=" + std::to_string(bg_kernel)
		    + ", threshold=" + std::to_string(threshold)
		    + ", morph=" + std::to_string(morph_kernel) + ", area="
		    + std::to_string(min_area) + "~" + std::to_string(max_area)
		    + ", circ>=" + std::to_string(min_circularity)
		    + ", aspect=" + std::to_string(ball_min_aspect) + "~"
		    + std::to_string(ball_max_aspect) + ", local_contrast>="
		    + std::to_string(ball_min_local_contrast)
		    + ", centerline_dist<="
		    + std::to_string(ball_max_centerline_distance_px)
		    + ", max_dist=" + std::to_string(max_axis_distance_px)
		    + ", max_jump=" + std::to_string(max_jump_px)
		    + ", reacquire="
		    + std::to_string(reacquire_after_lost_frames)
		    + ", zero_mode=" + zero_mode
		    + ", zero_px=" + std::to_string(zero_position_px)
		    + ", zero_samples=" + std::to_string(zero_samples)
		    + ", zero_range=" + std::to_string(zero_range_px)
		    + ", filter_alpha=" + std::to_string(filter_alpha);
	}

	std::string VisionConfig::to_string() const
	{
		return "red_h1=" + std::to_string(red_h1_min) + "-"
		    + std::to_string(red_h1_max)
		    + ", red_h2=" + std::to_string(red_h2_min) + "-"
		    + std::to_string(red_h2_max)
		    + ", saturation_min=" + std::to_string(saturation_min)
		    + ", value_min=" + std::to_string(value_min)
		    + ", morphology_kernel=" + std::to_string(morphology_kernel)
		    + ", min_area=" + std::to_string(min_area) + ", ball=["
		    + ball.to_string() + "]";
	}

	std::string UartConfig::to_string() const
	{
		return "device=" + device
		    + ", baudrate=" + std::to_string(baudrate)
		    + ", timeout_ms=" + std::to_string(timeout_ms)
		    + ", write_timeout_ms=" + std::to_string(write_timeout_ms)
		    + ", reconnect_interval_ms="
		    + std::to_string(reconnect_interval_ms)
		    + ", auto_reconnect="
		    + std::string(auto_reconnect ? "true" : "false")
		    + ", max_line_length=" + std::to_string(max_line_length)
		    + ", queue_capacity=" + std::to_string(queue_capacity)
		    + ", handshake_timeout_ms="
		    + std::to_string(handshake_timeout_ms)
		    + ", heartbeat_interval_ms="
		    + std::to_string(heartbeat_interval_ms)
		    + ", heartbeat_timeout_ms="
		    + std::to_string(heartbeat_timeout_ms)
		    + ", protocol_version_major="
		    + std::to_string(protocol_version_major)
		    + ", protocol_version_minor="
		    + std::to_string(protocol_version_minor);
	}

	std::string RecordConfig::to_string() const
	{
		return "enabled=" + std::string(enabled ? "true" : "false")
		    + ", save_raw=" + std::string(save_raw ? "true" : "false")
		    + ", save_debug="
		    + std::string(save_debug ? "true" : "false")
		    + ", directory=" + directory + ", fourcc=" + fourcc
		    + ", fps=" + std::to_string(fps)
		    + ", segment_seconds=" + std::to_string(segment_seconds);
	}

	std::string SearchConfig::to_string() const
	{
		return "show_preview="
		    + std::string(show_preview ? "true" : "false")
		    + ", enable_nn=" + std::string(enable_nn ? "true" : "false")
		    + ", detector=" + detector + ", model_path=" + model_path
		    + ", class_names_path=" + class_names_path
		    + ", nn_confidence_threshold="
		    + std::to_string(nn_confidence_threshold)
		    + ", nn_nms_threshold=" + std::to_string(nn_nms_threshold)
		    + ", nn_input=" + std::to_string(nn_input_width) + "x"
		    + std::to_string(nn_input_height)
		    + ", zero_samples=" + std::to_string(zero_sample_count)
		    + ", zero_jitter=" + std::to_string(zero_max_jitter_px)
		    + "px, zero_wait=" + std::to_string(zero_max_wait_frames)
		    + "fr, zero_min_conf=" + std::to_string(zero_min_confidence)
		    + ", max_jump=" + std::to_string(max_target_jump_px)
		    + "px, filt_alpha=" + std::to_string(position_filter_alpha)
		    + ", max_hold=" + std::to_string(max_hold_frames)
		    + ", lost_confirm=" + std::to_string(lost_confirm_frames)
		    + ", reacquire_confirm="
		    + std::to_string(reacquire_confirm_frames)
		    + ", mm_per_pixel=" + std::to_string(mm_per_pixel)
		    + ", invert_offset="
		    + std::string(invert_offset ? "true" : "false")
		    + ", send_int=" + std::to_string(result_send_interval_ms)
		    + "ms, calib_int="
		    + std::to_string(calib_status_interval_ms)
		    + "ms, vsession_retry="
		    + std::to_string(vsession_retry_interval_ms)
		    + "ms, max_infer_err="
		    + std::to_string(max_inference_errors)
		    + ", reload_int=" + std::to_string(model_reload_interval_ms)
		    + "ms, camera_id=" + camera_id
		    + ", nominal_fps=" + std::to_string(nominal_fps);
	}

	// 配置校验

	bool validateConfig(const AppConfig& config,
	                    std::vector<ConfigMessage>* messages_out)
	{
		bool valid = true;

		auto warn = [&](const std::string& desc) {
			if(messages_out != nullptr)
			{
				messages_out->push_back(
				    {ConfigMessageLevel::WARN, "VALIDATE", desc});
			}
		};

		auto err = [&](const std::string& desc) {
			valid = false;

			if(messages_out != nullptr)
			{
				messages_out->push_back(
				    {ConfigMessageLevel::ERROR, "VALIDATE", desc});
			}
		};

		// 校验模式名称
		if(config.mode.enabled)
		{
			if(!isValidModeName(config.mode.name))
			{
				err("mode.name is invalid: " + config.mode.name);
			}
		}

		// 校验摄像头
		if(config.camera.source.empty())
		{
			err("camera.source is empty");
		}

		if(config.camera.width <= 0 || config.camera.width > 4096)
		{
			err("camera.width out of range");
		}

		if(config.camera.height <= 0 || config.camera.height > 2160)
		{
			err("camera.height out of range");
		}

		// 校验 UART
		if(config.uart.device.empty())
		{
			warn("uart.device is empty");
		}

		// 协议主版本号只接受 4 或 5
		if(config.uart.protocol_version_major != 4
		   && config.uart.protocol_version_major != 5)
		{
			err("protocol_version_major must be 4 or 5, got "
			    + std::to_string(config.uart.protocol_version_major));
		}

		// handshake_timeout_ms: 100~10000
		if(config.uart.handshake_timeout_ms < 100
		   || config.uart.handshake_timeout_ms > 10000)
		{
			err("handshake_timeout_ms must be in [100, 10000]");
		}

		// heartbeat_interval_ms: 100~10000
		if(config.uart.heartbeat_interval_ms < 100
		   || config.uart.heartbeat_interval_ms > 10000)
		{
			err("heartbeat_interval_ms must be in [100, 10000]");
		}

		// heartbeat_timeout_ms must be > heartbeat_interval_ms
		if(config.uart.heartbeat_timeout_ms
		   <= config.uart.heartbeat_interval_ms)
		{
			err("heartbeat_timeout_ms must be > heartbeat_interval_ms");
		}

		if(config.uart.heartbeat_timeout_ms < 300
		   || config.uart.heartbeat_timeout_ms > 60000)
		{
			err("heartbeat_timeout_ms must be in [300, 60000]");
		}

		// 校验日志输出
		if(!config.logger.file && !config.logger.terminal)
		{
			warn("logger.file and logger.terminal are both false");
		}

		return valid;
	}

} // namespace etest