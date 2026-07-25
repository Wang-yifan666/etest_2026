#include "core/logger.hpp"

#include <chrono>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace
{

	std::tm localTime(std::time_t value) noexcept
	{
		std::tm result{};

#if defined(_WIN32)
		localtime_s(&result, &value);
#else
		localtime_r(&value, &result);
#endif

		return result;
	}

	std::string timeText(const char* format)
	{
		const auto now = std::chrono::system_clock::now();

		const auto time_value =
		    std::chrono::system_clock::to_time_t(now);

		const std::tm local = localTime(time_value);

		const auto milliseconds =
		    std::chrono::duration_cast<std::chrono::milliseconds>(
		        now.time_since_epoch())
		    % 1000;

		std::ostringstream output;

		output << std::put_time(&local, format) << '.' << std::setw(3)
		       << std::setfill('0') << milliseconds.count();

		return output.str();
	}

	std::string oneLine(const std::string& text)
	{
		std::string result;
		result.reserve(text.size());

		for(char ch: text)
		{
			if(ch == '\n')
			{
				result += "\\n";
			}
			else if(ch == '\r')
			{
				result += "\\r";
			}
			else
			{
				result.push_back(ch);
			}
		}

		return result;
	}

	const char* levelName(etest::LogLevel level) noexcept
	{
		switch(level)
		{
		case etest::LogLevel::DEBUG:
			return "DEBUG";

		case etest::LogLevel::INFO:
			return "INFO";

		case etest::LogLevel::WARN:
			return "WARN";

		case etest::LogLevel::ERROR:
			return "ERROR";

		case etest::LogLevel::FATAL:
			return "FATAL";
		}

		return "UNKNOWN";
	}

	void fallback(etest::LogLevel level, const std::string& source,
	              const std::string& message) noexcept
	{
		std::fprintf(stderr, "[LOGGER FALLBACK] %s - %s - %s\n",
		             levelName(level), source.c_str(), message.c_str());

		std::fflush(stderr);
	}

} // namespace

namespace etest
{

	Logger& Logger::instance() noexcept
	{
		static Logger logger;
		return logger;
	}

	Logger::~Logger()
	{
		shutdown();
	}

	bool Logger::init(const LoggerConfig& config) noexcept
	{
		try
		{
			std::lock_guard<std::mutex> lock(mutex_);

			if(file_.is_open())
			{
				file_.close();
			}

			config_ = config;
			file_path_.clear();
			initialized_ = false;

			bool terminal_forced = false;

			if(!config_.file && !config_.terminal)
			{
				config_.terminal = true;
				terminal_forced = true;
			}

			std::string file_error;

			if(!openFile(file_error))
			{
				config_.file = false;
				config_.terminal = true;
			}

			initialized_ = true;

			if(terminal_forced)
			{
				write(LogLevel::WARN, "LOGGER",
				      "file and terminal outputs were both disabled; "
				      "terminal output was forced on",
				      true);
			}

			if(!file_error.empty())
			{
				write(LogLevel::ERROR, "LOGGER",
				      file_error + "; continuing with terminal output",
				      true);
			}

			write(LogLevel::INFO, "LOGGER",
			      file_.is_open() ? "initialized, file=" + file_path_
			                      : "initialized, file output disabled",
			      true);

			return config_.terminal || file_.is_open();
		}
		catch(const std::exception& error)
		{
			fallback(
			    LogLevel::ERROR, "LOGGER",
			    std::string("initialization failed: ") + error.what());

			return false;
		}
		catch(...)
		{
			fallback(LogLevel::ERROR, "LOGGER",
			         "unknown initialization failure");

			return false;
		}
	}

	void Logger::log(LogLevel level, const std::string& source,
	                 const std::string& message) noexcept
	{
		try
		{
			std::lock_guard<std::mutex> lock(mutex_);

			write(level, source, message);
		}
		catch(const std::exception& error)
		{
			fallback(level, source,
			         std::string("logging failure: ") + error.what());
		}
		catch(...)
		{
			fallback(level, source, "unknown logging failure");
		}
	}

	void Logger::shutdown() noexcept
	{
		try
		{
			std::lock_guard<std::mutex> lock(mutex_);

			if(!initialized_ && !file_.is_open())
			{
				return;
			}

			if(initialized_)
			{
				write(LogLevel::INFO, "LOGGER", "shutdown", true);
			}

			if(file_.is_open())
			{
				file_.flush();
				file_.close();
			}

			initialized_ = false;
		}
		catch(...)
		{
			fallback(LogLevel::ERROR, "LOGGER", "shutdown failure");
		}
	}

	bool Logger::openFile(std::string& error) noexcept
	{
		if(!config_.file)
		{
			return true;
		}

		try
		{
			std::error_code error_code;

			std::filesystem::create_directories(config_.directory,
			                                    error_code);

			if(error_code)
			{
				error = "cannot create log directory "
				    + config_.directory + ": " + error_code.message();

				return false;
			}

			const std::filesystem::path path =
			    std::filesystem::path(config_.directory)
			    / (timeText("%Y-%m-%d_%H-%M-%S") + ".log");

			file_.open(path, std::ios::out | std::ios::trunc);

			if(!file_.is_open())
			{
				error = "cannot open log file " + path.string();

				return false;
			}

			file_path_ = path.string();
			return true;
		}
		catch(const std::exception& exception)
		{
			error = std::string("open log file exception: ")
			    + exception.what();

			return false;
		}
		catch(...)
		{
			error = "unknown open log file exception";

			return false;
		}
	}

	void Logger::write(LogLevel level, const std::string& source,
	                   const std::string& message, bool force) noexcept
	{
		if(!force
		   && static_cast<int>(level)
		       < static_cast<int>(config_.min_level))
		{
			return;
		}

		// 节流检查
		if(!force && config_.throttle_interval_ms > 0)
		{
			const std::string throttle_key = source + '\0' + message;

			const auto now = std::chrono::steady_clock::now();

			const auto it = throttle_map_.find(throttle_key);

			if(it != throttle_map_.end())
			{
				const auto elapsed = std::chrono::duration_cast<
				    std::chrono::milliseconds>(now - it->second);

				if(elapsed.count() < config_.throttle_interval_ms)
				{
					return;
				}
			}

			throttle_map_[throttle_key] = now;
		}

		if(!initialized_)
		{
			fallback(level, source.empty() ? "UNKNOWN" : source,
			         message);

			return;
		}

		try
		{
			const std::string safe_source =
			    source.empty() ? "UNKNOWN" : oneLine(source);

			const std::string line = timeText("%Y-%m-%d %H:%M:%S")
			    + " - " + levelName(level) + " - " + safe_source + " - "
			    + oneLine(message);

			if(config_.terminal)
			{
				std::ostream& output =
				    level >= LogLevel::ERROR ? std::cerr : std::cout;

				output << line << '\n';

				if(config_.flush_each_write)
				{
					output.flush();
				}
			}

			if(config_.file && file_.is_open())
			{
				file_ << line << '\n';

				if(config_.flush_each_write)
				{
					file_.flush();
				}

				if(!file_)
				{
					config_.file = false;

					fallback(LogLevel::ERROR, "LOGGER",
					         "file write failed; "
					         "file output disabled");
				}
			}
		}
		catch(const std::exception& error)
		{
			fallback(
			    level, source.empty() ? "UNKNOWN" : source,
			    std::string("formatting failure: ") + error.what());
		}
		catch(...)
		{
			fallback(level, source.empty() ? "UNKNOWN" : source,
			         "unknown formatting failure");
		}
	}

} // namespace etest