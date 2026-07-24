#include "core/config.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace
{

struct RawValue
{
    std::string text;
    std::size_t line = 0;
};

using RawConfig = std::unordered_map<std::string, RawValue>;

std::string trim(std::string text)
{
    const auto not_space = [](unsigned char ch) {
        return !std::isspace(ch);
    };

    text.erase(
        text.begin(),
        std::find_if(text.begin(), text.end(), not_space));

    text.erase(
        std::find_if(text.rbegin(), text.rend(), not_space).base(),
        text.end());

    return text;
}

std::string upper(std::string text)
{
    std::transform(
        text.begin(), text.end(), text.begin(),
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

void addMessage(
    etest::ConfigLoadResult& result,
    etest::ConfigMessageLevel level,
    const std::string& description)
{
    result.messages.push_back(
        etest::ConfigMessage{level, "CONFIG", description});
}

bool parseString(
    const std::string& raw,
    std::string& value)
{
    const std::string text = trim(raw);

    if(text.size() < 2 ||
       text.front() != '"' ||
       text.back() != '"')
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

bool parseBool(
    const std::string& raw,
    bool& value)
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

bool parseInt(
    const std::string& raw,
    int& value)
{
    const std::string text = trim(raw);

    if(text.empty())
    {
        return false;
    }

    int parsed = 0;

    const auto result = std::from_chars(
        text.data(),
        text.data() + text.size(),
        parsed);

    if(result.ec != std::errc{} ||
       result.ptr != text.data() + text.size())
    {
        return false;
    }

    value = parsed;
    return true;
}

bool parseDouble(
    const std::string& raw,
    double& value)
{
    const std::string text = trim(raw);

    if(text.empty())
    {
        return false;
    }

    char* end = nullptr;
    const double parsed =
        std::strtod(text.c_str(), &end);

    if(end != text.c_str() + text.size() ||
       !std::isfinite(parsed))
    {
        return false;
    }

    value = parsed;
    return true;
}

const RawValue* findValue(
    const RawConfig& raw_config,
    const std::string& key)
{
    const auto it = raw_config.find(key);

    if(it == raw_config.end())
    {
        return nullptr;
    }

    return &it->second;
}

std::string getString(
    const RawConfig& raw_config,
    const std::string& key,
    const std::string& default_value,
    bool allow_empty,
    etest::ConfigLoadResult& result)
{
    const RawValue* raw = findValue(raw_config, key);

    if(raw == nullptr)
    {
        return default_value;
    }

    std::string value;

    if(!parseString(raw->text, value) ||
       (!allow_empty && value.empty()))
    {
        addMessage(
            result,
            etest::ConfigMessageLevel::ERROR,
            key + " is invalid at line " +
                std::to_string(raw->line) +
                "; using default \"" +
                default_value + "\"");

        return default_value;
    }

    return value;
}

bool getBool(
    const RawConfig& raw_config,
    const std::string& key,
    bool default_value,
    etest::ConfigLoadResult& result)
{
    const RawValue* raw = findValue(raw_config, key);

    if(raw == nullptr)
    {
        return default_value;
    }

    bool value = false;

    if(!parseBool(raw->text, value))
    {
        addMessage(
            result,
            etest::ConfigMessageLevel::ERROR,
            key + " is invalid at line " +
                std::to_string(raw->line) +
                "; using default " +
                (default_value ? "true" : "false"));

        return default_value;
    }

    return value;
}

int getInt(
    const RawConfig& raw_config,
    const std::string& key,
    int default_value,
    int minimum,
    int maximum,
    etest::ConfigLoadResult& result)
{
    const RawValue* raw = findValue(raw_config, key);

    if(raw == nullptr)
    {
        return default_value;
    }

    int value = 0;

    if(!parseInt(raw->text, value))
    {
        addMessage(
            result,
            etest::ConfigMessageLevel::ERROR,
            key + " is not an integer at line " +
                std::to_string(raw->line) +
                "; using default " +
                std::to_string(default_value));

        return default_value;
    }

    if(value < minimum || value > maximum)
    {
        addMessage(
            result,
            etest::ConfigMessageLevel::WARNING,
            key + " is outside [" +
                std::to_string(minimum) + ", " +
                std::to_string(maximum) +
                "] at line " +
                std::to_string(raw->line) +
                "; using default " +
                std::to_string(default_value));

        return default_value;
    }

    return value;
}

double getDouble(
    const RawConfig& raw_config,
    const std::string& key,
    double default_value,
    double minimum,
    double maximum,
    etest::ConfigLoadResult& result)
{
    const RawValue* raw = findValue(raw_config, key);

    if(raw == nullptr)
    {
        return default_value;
    }

    double value = 0.0;

    if(!parseDouble(raw->text, value))
    {
        addMessage(
            result,
            etest::ConfigMessageLevel::ERROR,
            key + " is not a number at line " +
                std::to_string(raw->line) +
                "; using default " +
                std::to_string(default_value));

        return default_value;
    }

    if(value < minimum || value > maximum)
    {
        addMessage(
            result,
            etest::ConfigMessageLevel::WARNING,
            key + " is outside the allowed range at line " +
                std::to_string(raw->line) +
                "; using default " +
                std::to_string(default_value));

        return default_value;
    }

    return value;
}

etest::LogLevel getLogLevel(
    const RawConfig& raw_config,
    const std::string& key,
    etest::LogLevel default_value,
    etest::ConfigLoadResult& result)
{
    const RawValue* raw = findValue(raw_config, key);

    if(raw == nullptr)
    {
        return default_value;
    }

    std::string value;

    if(!parseString(raw->text, value))
    {
        addMessage(
            result,
            etest::ConfigMessageLevel::ERROR,
            key + " is invalid at line " +
                std::to_string(raw->line) +
                "; using default INFO");

        return default_value;
    }

    value = upper(trim(value));

    if(value == "DEBUG")
    {
        return etest::LogLevel::DEBUG;
    }

    if(value == "INFO")
    {
        return etest::LogLevel::INFO;
    }

    if(value == "WARN" || value == "WARNING")
    {
        return etest::LogLevel::WARN;
    }

    if(value == "ERROR")
    {
        return etest::LogLevel::ERROR;
    }

    if(value == "FATAL")
    {
        return etest::LogLevel::FATAL;
    }

    addMessage(
        result,
        etest::ConfigMessageLevel::ERROR,
        key + " has unknown level \"" +
            value + "\" at line " +
            std::to_string(raw->line) +
            "; using default INFO");

    return default_value;
}

RawConfig parseFile(
    std::ifstream& input,
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
            if(line.size() < 3 ||
               line.back() != ']')
            {
                addMessage(
                    result,
                    etest::ConfigMessageLevel::ERROR,
                    "invalid section declaration at line " +
                        std::to_string(line_number));

                continue;
            }

            section = trim(
                line.substr(1, line.size() - 2));

            if(section.empty())
            {
                addMessage(
                    result,
                    etest::ConfigMessageLevel::ERROR,
                    "empty section name at line " +
                        std::to_string(line_number));
            }

            continue;
        }

        const auto equal = line.find('=');

        if(equal == std::string::npos)
        {
            addMessage(
                result,
                etest::ConfigMessageLevel::ERROR,
                "missing '=' at line " +
                    std::to_string(line_number));

            continue;
        }

        const std::string key =
            trim(line.substr(0, equal));

        const std::string value =
            trim(line.substr(equal + 1));

        if(key.empty() || value.empty())
        {
            addMessage(
                result,
                etest::ConfigMessageLevel::ERROR,
                "empty key or value at line " +
                    std::to_string(line_number));

            continue;
        }

        const std::string full_key =
            section.empty()
                ? key
                : section + "." + key;

        const auto existing =
            raw_config.find(full_key);

        if(existing != raw_config.end())
        {
            addMessage(
                result,
                etest::ConfigMessageLevel::WARNING,
                full_key + " is duplicated at line " +
                    std::to_string(line_number) +
                    "; the last value is used");
        }

        raw_config[full_key] =
            RawValue{value, line_number};
    }

    return raw_config;
}

void warnUnknownKeys(
    const RawConfig& raw_config,
    etest::ConfigLoadResult& result)
{
    const std::unordered_set<std::string> known_keys{
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

        "search.show_preview",
        "search.enable_nn",
        "search.model_path",
        "search.class_names_path",
        "search.nn_confidence_threshold",
        "search.nn_nms_threshold"
    };

    for(const auto& item : raw_config)
    {
        if(known_keys.find(item.first) ==
           known_keys.end())
        {
            addMessage(
                result,
                etest::ConfigMessageLevel::WARNING,
                "unknown key " + item.first +
                    " at line " +
                    std::to_string(item.second.line));
        }
    }
}

void extractAppConfig(
    etest::AppConfig& config,
    const RawConfig& raw_config,
    etest::ConfigLoadResult& result)
{
    config.logger.directory =
        getString(
            raw_config,
            "logger.directory",
            config.logger.directory,
            false,
            result);

    config.logger.file =
        getBool(
            raw_config,
            "logger.file",
            config.logger.file,
            result);

    config.logger.terminal =
        getBool(
            raw_config,
            "logger.terminal",
            config.logger.terminal,
            result);

    config.logger.flush_each_write =
        getBool(
            raw_config,
            "logger.flush_each_write",
            config.logger.flush_each_write,
            result);

    config.logger.min_level =
        getLogLevel(
            raw_config,
            "logger.min_level",
            config.logger.min_level,
            result);

    if(!config.logger.file &&
       !config.logger.terminal)
    {
        config.logger.terminal = true;

        addMessage(
            result,
            etest::ConfigMessageLevel::WARNING,
            "logger.file and logger.terminal cannot both be false; "
            "terminal output is forced on");
    }

    config.camera.source =
        getString(
            raw_config,
            "camera.source",
            config.camera.source,
            false,
            result);

    config.camera.width =
        getInt(
            raw_config,
            "camera.width",
            config.camera.width,
            1,
            4096,
            result);

    config.camera.height =
        getInt(
            raw_config,
            "camera.height",
            config.camera.height,
            1,
            2160,
            result);

    config.camera.fps =
        getInt(
            raw_config,
            "camera.fps",
            config.camera.fps,
            1,
            240,
            result);

    config.vision.red_h1_min =
        getInt(
            raw_config,
            "vision.red_h1_min",
            config.vision.red_h1_min,
            0,
            180,
            result);

    config.vision.red_h1_max =
        getInt(
            raw_config,
            "vision.red_h1_max",
            config.vision.red_h1_max,
            0,
            180,
            result);

    config.vision.red_h2_min =
        getInt(
            raw_config,
            "vision.red_h2_min",
            config.vision.red_h2_min,
            0,
            180,
            result);

    config.vision.red_h2_max =
        getInt(
            raw_config,
            "vision.red_h2_max",
            config.vision.red_h2_max,
            0,
            180,
            result);

    config.vision.saturation_min =
        getInt(
            raw_config,
            "vision.saturation_min",
            config.vision.saturation_min,
            0,
            255,
            result);

    config.vision.value_min =
        getInt(
            raw_config,
            "vision.value_min",
            config.vision.value_min,
            0,
            255,
            result);

    config.vision.morphology_kernel =
        getInt(
            raw_config,
            "vision.morphology_kernel",
            config.vision.morphology_kernel,
            1,
            31,
            result);

    if(config.vision.morphology_kernel % 2 == 0)
    {
        config.vision.morphology_kernel = 5;

        addMessage(
            result,
            etest::ConfigMessageLevel::WARNING,
            "vision.morphology_kernel must be odd; using default 5");
    }

    config.vision.min_area =
        getDouble(
            raw_config,
            "vision.min_area",
            config.vision.min_area,
            0.0,
            1000000000.0,
            result);

    config.uart.device =
        getString(
            raw_config,
            "uart.device",
            config.uart.device,
            false,
            result);

    config.uart.baudrate =
        getInt(
            raw_config,
            "uart.baudrate",
            config.uart.baudrate,
            1200,
            3000000,
            result);

    config.uart.timeout_ms =
        getInt(
            raw_config,
            "uart.timeout_ms",
            config.uart.timeout_ms,
            0,
            10000,
            result);

    config.logger.throttle_interval_ms =
        getInt(
            raw_config,
            "logger.throttle_interval_ms",
            config.logger.throttle_interval_ms,
            0,
            60000,
            result);

    config.search.show_preview =
        getBool(
            raw_config,
            "search.show_preview",
            config.search.show_preview,
            result);

    config.search.enable_nn =
        getBool(
            raw_config,
            "search.enable_nn",
            config.search.enable_nn,
            result);

    config.search.model_path =
        getString(
            raw_config,
            "search.model_path",
            config.search.model_path,
            false,
            result);

    config.search.class_names_path =
        getString(
            raw_config,
            "search.class_names_path",
            config.search.class_names_path,
            true,
            result);

    config.search.nn_confidence_threshold =
        getDouble(
            raw_config,
            "search.nn_confidence_threshold",
            config.search.nn_confidence_threshold,
            0.0,
            1.0,
            result);

    config.search.nn_nms_threshold =
        getDouble(
            raw_config,
            "search.nn_nms_threshold",
            config.search.nn_nms_threshold,
            0.0,
            1.0,
            result);
}

} // namespace

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
            addMessage(
                result,
                ConfigMessageLevel::ERROR,
                "cannot open " + path +
                    "; all modules use default values");

            return result;
        }

        result.file_loaded = true;

        const RawConfig raw_config =
            parseFile(input, result);

        warnUnknownKeys(raw_config, result);

        extractAppConfig(
            result.config,
            raw_config,
            result);

        return result;
    }
    catch(const std::exception& error)
    {
        addMessage(
            result,
            ConfigMessageLevel::ERROR,
            std::string("unexpected config exception: ") +
                error.what() +
                "; remaining values use defaults");

        return result;
    }
    catch(...)
    {
        addMessage(
            result,
            ConfigMessageLevel::ERROR,
            "unknown config exception; remaining values use defaults");

        return result;
    }
}

ConfigLoadResult ConfigLoader::loadMultiple(
    const std::vector<std::string>& paths) noexcept
{
    ConfigLoadResult result;
    RawConfig merged;

    bool any_loaded = false;

    for(const auto& path : paths)
    {
        try
        {
            std::ifstream input(path);

            if(!input.is_open())
            {
                addMessage(
                    result,
                    ConfigMessageLevel::ERROR,
                    "cannot open " + path +
                        "; skipping");

                continue;
            }

            any_loaded = true;

            const RawConfig file_config =
                parseFile(input, result);

            for(const auto& item : file_config)
            {
                merged[item.first] = item.second;
            }
        }
        catch(const std::exception& error)
        {
            addMessage(
                result,
                ConfigMessageLevel::ERROR,
                std::string("exception while reading ") +
                    path + ": " + error.what() +
                    "; skipping");
        }
        catch(...)
        {
            addMessage(
                result,
                ConfigMessageLevel::ERROR,
                "unknown exception while reading " +
                    path + "; skipping");
        }
    }

    result.file_loaded = any_loaded;

    warnUnknownKeys(merged, result);

    extractAppConfig(
        result.config,
        merged,
        result);

    return result;
}

} // namespace etest