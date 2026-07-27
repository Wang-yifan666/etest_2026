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
		               + "; keeping current value \"" + current_value
		               + "\"");

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

	    "vision.red_h1_min",
	    "vision.red_h1_max",
	    "vision.red_h2_min",
	    "vision.red_h2_max",
	    "vision.saturation_min",
	    "vision.value_min",
	    "vision.morphology_kernel",
	    "vision.min_area",

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

	    "search.show_preview",
	    "search.enable_nn",
	    "search.model_path",
	    "search.class_names_path",
	    "search.nn_confidence_threshold",
	    "search.nn_nms_threshold",
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
	// ---- [mode] ----
	config.mode.enabled =
	    getBool(raw_config, "mode.enabled", config.mode.enabled,
	            result, &config.mode.source_enabled);

	config.mode.name = getString(raw_config, "mode.name",
	                             config.mode.name, false, result,
	                             &config.mode.source_name);

	// ---- [runtime] ----
	config.runtime.headless =
	    getBool(raw_config, "runtime.headless",
	            config.runtime.headless, result,
	            &config.runtime.source_headless);

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

	// ---- [logger] ----
	config.logger.directory =
	    getString(raw_config, "logger.directory",
	              config.logger.directory, false, result,
	              &config.logger.source_directory);

	config.logger.file =
	    getBool(raw_config, "logger.file", config.logger.file,
	            result, &config.logger.source_file);

	config.logger.terminal =
	    getBool(raw_config, "logger.terminal",
	            config.logger.terminal, result,
	            &config.logger.source_terminal);

	config.logger.flush_each_write =
	    getBool(raw_config, "logger.flush_each_write",
	            config.logger.flush_each_write, result,
	            &config.logger.source_flush_each_write);

	config.logger.min_level =
	    getLogLevel(raw_config, "logger.min_level",
	                config.logger.min_level, result,
	                &config.logger.source_min_level);

	if(!config.logger.file && !config.logger.terminal)
	{
		config.logger.terminal = true;

		addMessage(
		    result, etest::ConfigMessageLevel::WARN,
		    "logger.file and logger.terminal cannot both be false; "
		    "terminal output is forced on");
	}

	config.logger.throttle_interval_ms = getInt(
	    raw_config, "logger.throttle_interval_ms",
	    config.logger.throttle_interval_ms, 0, 60000, result,
	    &config.logger.source_throttle_interval_ms);

	// ---- [camera] ----
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

	config.camera.loop_video =
	    getBool(raw_config, "camera.loop_video",
	            config.camera.loop_video, result,
	            &config.camera.source_loop_video);

	// ---- [vision] ----
	config.vision.red_h1_min =
	    getInt(raw_config, "vision.red_h1_min",
	           config.vision.red_h1_min, 0, 180, result,
	           &config.vision.source_red_h1_min);

	config.vision.red_h1_max =
	    getInt(raw_config, "vision.red_h1_max",
	           config.vision.red_h1_max, 0, 180, result,
	           &config.vision.source_red_h1_max);

	config.vision.red_h2_min =
	    getInt(raw_config, "vision.red_h2_min",
	           config.vision.red_h2_min, 0, 180, result,
	           &config.vision.source_red_h2_min);

	config.vision.red_h2_max =
	    getInt(raw_config, "vision.red_h2_max",
	           config.vision.red_h2_max, 0, 180, result,
	           &config.vision.source_red_h2_max);

	config.vision.saturation_min =
	    getInt(raw_config, "vision.saturation_min",
	           config.vision.saturation_min, 0, 255, result,
	           &config.vision.source_saturation_min);

	config.vision.value_min =
	    getInt(raw_config, "vision.value_min",
	           config.vision.value_min, 0, 255, result,
	           &config.vision.source_value_min);

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

	config.vision.min_area =
	    getDouble(raw_config, "vision.min_area",
	              config.vision.min_area, 0.0, 1000000000.0, result,
	              &config.vision.source_min_area);

	// ---- [uart] ----
	config.uart.device =
	    getString(raw_config, "uart.device", config.uart.device,
	              false, result, &config.uart.source_device);

	config.uart.baudrate =
	    getInt(raw_config, "uart.baudrate", config.uart.baudrate,
	           1200, 3000000, result, &config.uart.source_baudrate);

	config.uart.timeout_ms =
	    getInt(raw_config, "uart.timeout_ms",
	           config.uart.timeout_ms, 0, 10000, result,
	           &config.uart.source_timeout_ms);

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
	           config.uart.heartbeat_interval_ms, 100, 10000, result,
	           &config.uart.source_heartbeat_interval_ms);

	config.uart.heartbeat_timeout_ms =
	    getInt(raw_config, "uart.heartbeat_timeout_ms",
	           config.uart.heartbeat_timeout_ms, 300, 60000, result,
	           &config.uart.source_heartbeat_timeout_ms);

	config.uart.protocol_version =
	    getInt(raw_config, "uart.protocol_version",
	           config.uart.protocol_version, 4, 4, result,
	           &config.uart.source_protocol_version);

	// ---- [search] ----
	config.search.show_preview =
	    getBool(raw_config, "search.show_preview",
	            config.search.show_preview, result,
	            &config.search.source_show_preview);

	config.search.enable_nn =
	    getBool(raw_config, "search.enable_nn",
	            config.search.enable_nn, result,
	            &config.search.source_enable_nn);

	config.search.model_path =
	    getString(raw_config, "search.model_path",
	              config.search.model_path, false, result,
	              &config.search.source_model_path);

	config.search.class_names_path =
	    getString(raw_config, "search.class_names_path",
	              config.search.class_names_path, true, result,
	              &config.search.source_class_names_path);

	config.search.nn_confidence_threshold =
	    getDouble(raw_config, "search.nn_confidence_threshold",
	              config.search.nn_confidence_threshold, 0.0, 1.0,
	              result,
	              &config.search.source_nn_confidence_threshold);

	config.search.nn_nms_threshold =
	    getDouble(raw_config, "search.nn_nms_threshold",
	              config.search.nn_nms_threshold, 0.0, 1.0, result,
	              &config.search.source_nn_nms_threshold);
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
			addMessage(result, etest::ConfigMessageLevel::ERROR,
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
		addMessage(result, etest::ConfigMessageLevel::ERROR,
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

etest::ConfigLoadResult
loadAppConfigInternal(const std::string& config_dir)
{
	etest::ConfigLoadResult result;
	result.config_dir = config_dir;
	result.mode_name = "";

	// 从 C++ 默认值开始（protocol_version=4）
	etest::AppConfig config;

	// 第1层：main.toml
	loadFileLayer(config, config_dir + "/main.toml", result);

	// 第2层：logger.toml
	loadFileLayer(config, config_dir + "/logger.toml", result);

	// 第3层：camera.toml
	loadFileLayer(config, config_dir + "/camera.toml", result);

	// 第4层：search.toml
	loadFileLayer(config, config_dir + "/search.toml", result);

	// 模式处理
	result.mode_name = config.mode.name;

	if(config.mode.enabled)
	{
		if(!isValidModeName(config.mode.name))
		{
			addMessage(result, etest::ConfigMessageLevel::ERROR,
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
		    std::min(60000,
		             config.uart.heartbeat_interval_ms * 3);

		addMessage(result, etest::ConfigMessageLevel::ERROR,
		           "heartbeat_timeout_ms must be greater than "
		           "heartbeat_interval_ms; adjusted to "
		               + std::to_string(
		                   config.uart.heartbeat_timeout_ms));
	}

	result.config = std::move(config);

	return result;
}

} // namespace

// 公共接口实现

namespace etest
{

ConfigLoadResult ConfigLoader::load(const std::string& path) noexcept
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

ConfigLoadResult
loadAppConfigFromMainFile(const std::string& main_toml_path)
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
	return "directory=" + directory + ", file="
	       + std::string(file ? "true" : "false") + ", terminal="
	       + std::string(terminal ? "true" : "false")
	       + ", min_level="
	       + [](LogLevel level) {
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

std::string VisionConfig::to_string() const
{
	return "red_h1=" + std::to_string(red_h1_min) + "-"
	       + std::to_string(red_h1_max) + ", red_h2="
	       + std::to_string(red_h2_min) + "-"
	       + std::to_string(red_h2_max) + ", saturation_min="
	       + std::to_string(saturation_min) + ", value_min="
	       + std::to_string(value_min)
	       + ", morphology_kernel="
	       + std::to_string(morphology_kernel)
	       + ", min_area=" + std::to_string(min_area);
}

std::string UartConfig::to_string() const
{
	return "device=" + device + ", baudrate="
	       + std::to_string(baudrate) + ", timeout_ms="
	       + std::to_string(timeout_ms) + ", write_timeout_ms="
	       + std::to_string(write_timeout_ms)
	       + ", reconnect_interval_ms="
	       + std::to_string(reconnect_interval_ms)
	       + ", auto_reconnect="
	       + std::string(auto_reconnect ? "true" : "false")
	       + ", max_line_length="
	       + std::to_string(max_line_length) + ", queue_capacity="
	       + std::to_string(queue_capacity)
	       + ", handshake_timeout_ms="
	       + std::to_string(handshake_timeout_ms)
	       + ", heartbeat_interval_ms="
	       + std::to_string(heartbeat_interval_ms)
	       + ", heartbeat_timeout_ms="
	       + std::to_string(heartbeat_timeout_ms)
	       + ", protocol_version="
	       + std::to_string(protocol_version);
}

std::string SearchConfig::to_string() const
{
	return "show_preview="
	       + std::string(show_preview ? "true" : "false")
	       + ", enable_nn="
	       + std::string(enable_nn ? "true" : "false")
	       + ", model_path=" + model_path
	       + ", class_names_path=" + class_names_path
	       + ", nn_confidence_threshold="
	       + std::to_string(nn_confidence_threshold)
	       + ", nn_nms_threshold="
	       + std::to_string(nn_nms_threshold);
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

	// 协议版本只接受 4
	if(config.uart.protocol_version != 4)
	{
		err("protocol_version must be 4, got "
		     + std::to_string(config.uart.protocol_version));
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