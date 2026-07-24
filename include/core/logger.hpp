#pragma once

#include "core/config.hpp"

#include <chrono>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>

namespace etest
{

class Logger final
{
public:
    // 全局访问点
    static Logger& instance() noexcept;

    // 初始化日志系统
    bool init(const LoggerConfig& config) noexcept;

    // 写入日志
    void log(
        LogLevel level,
        const std::string& source,
        const std::string& message) noexcept;

    // 关日志
    void shutdown() noexcept;

    // 禁止拷贝构造函数和拷贝赋值运算符
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

private:
    Logger() = default;
    ~Logger();

    // 参数可做输出，如果打开失败就将错误信息写入 error
    bool openFile(std::string& error) noexcept;

    void write(
        LogLevel level,
        const std::string& source,
        const std::string& message,
        bool force = false) noexcept;

    // 互斥锁，保护日志写入
    std::mutex mutex_;
    std::ofstream file_;
    LoggerConfig config_;
    std::string file_path_;
    bool initialized_ = false;

    // 节流：key = "source\0message"，value = 上次输出时间戳
    std::unordered_map<std::string, std::chrono::steady_clock::time_point>
        throttle_map_;
};

} // namespace etest

// 封装
#define ETEST_LOG_DEBUG(source, message) \
    ::etest::Logger::instance().log(     \
        ::etest::LogLevel::DEBUG, source, message)

#define ETEST_LOG_INFO(source, message) \
    ::etest::Logger::instance().log(    \
        ::etest::LogLevel::INFO, source, message)

#define ETEST_LOG_WARN(source, message) \
    ::etest::Logger::instance().log(    \
        ::etest::LogLevel::WARN, source, message)

#define ETEST_LOG_ERROR(source, message) \
    ::etest::Logger::instance().log(     \
        ::etest::LogLevel::ERROR, source, message)

#define ETEST_LOG_FATAL(source, message) \
    ::etest::Logger::instance().log(     \
        ::etest::LogLevel::FATAL, source, message)

