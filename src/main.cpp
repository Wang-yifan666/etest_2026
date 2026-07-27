#include "core/config.hpp"
#include "core/context.hpp"
#include "core/logger.hpp"

#include "state/end.hpp"
#include "state/error.hpp"
#include "state/search.hpp"
#include "state/start.hpp"

#include <exception>
#include <string>

namespace
{

	const char* logLevelName(etest::LogLevel level) noexcept
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

	void logConfigMessages(const etest::ConfigLoadResult& result)
	{
		for(const auto& message: result.messages)
		{
			if(message.level == etest::ConfigMessageLevel::ERROR)
			{
				ETEST_LOG_ERROR(message.source, message.description);
			}
			else
			{
				ETEST_LOG_WARN(message.source, message.description);
			}
		}
	}

	void logEffectiveConfig(const etest::AppConfig& config,
	                        const std::string& config_path,
	                        bool file_loaded)
	{
		ETEST_LOG_INFO(
		    "CONFIG",
		    file_loaded ? "loaded file: " + config_path
		                : "using defaults because file was not loaded: "
		            + config_path);

		ETEST_LOG_INFO("CONFIG",
		               "logger.directory=" + config.logger.directory
		                   + ", logger.file="
		                   + (config.logger.file ? "true" : "false")
		                   + ", logger.terminal="
		                   + (config.logger.terminal ? "true" : "false")
		                   + ", logger.min_level="
		                   + logLevelName(config.logger.min_level));

		ETEST_LOG_INFO(
		    "CONFIG",
		    "camera.source=" + config.camera.source + ", camera.width="
		        + std::to_string(config.camera.width)
		        + ", camera.height="
		        + std::to_string(config.camera.height)
		        + ", camera.fps=" + std::to_string(config.camera.fps));

		ETEST_LOG_INFO(
		    "CONFIG",
		    "vision.min_area=" + std::to_string(config.vision.min_area)
		        + ", vision.morphology_kernel="
		        + std::to_string(config.vision.morphology_kernel));

		ETEST_LOG_INFO(
		    "CONFIG",
		    "uart.device=" + config.uart.device + ", uart.baudrate="
		        + std::to_string(config.uart.baudrate)
		        + ", uart.timeout_ms="
		        + std::to_string(config.uart.timeout_ms)
		        + ", uart.write_timeout_ms="
		        + std::to_string(config.uart.write_timeout_ms)
		        + ", uart.reconnect_interval_ms="
		        + std::to_string(config.uart.reconnect_interval_ms)
		        + ", uart.auto_reconnect="
		        + std::string(config.uart.auto_reconnect ? "true"
		                                                 : "false")
		        + ", uart.max_line_length="
		        + std::to_string(config.uart.max_line_length)
		        + ", uart.queue_capacity="
		        + std::to_string(config.uart.queue_capacity));

		ETEST_LOG_INFO(
		    "CONFIG",
		    "search.show_preview="
		        + std::string(config.search.show_preview ? "true"
		                                                 : "false")
		        + ", search.enable_nn="
		        + std::string(config.search.enable_nn ? "true"
		                                              : "false")
		        + ", search.model_path=" + config.search.model_path
		        + ", search.nn_confidence_threshold="
		        + std::to_string(
		            config.search.nn_confidence_threshold));

		ETEST_LOG_INFO(
		    "CONFIG",
		    "logger.throttle_interval_ms="
		        + std::to_string(config.logger.throttle_interval_ms));
	}

} // namespace

int main(int argc, char* argv[])
{
	const std::string config_path =
	    argc > 1 ? argv[1] : "config/main.toml";

	/*
     * 配置模块不依赖 Logger。
     * 读取过程中产生的错误暂存在 ConfigLoadResult 中。
     */
	const etest::ConfigLoadResult config_result =
	    etest::ConfigLoader::loadMultiple(
	        {"config/main.toml", "config/logger.toml",
	         "config/camera.toml", "config/search.toml"});

	auto& logger = etest::Logger::instance();

	logger.init(config_result.config.logger);

	logConfigMessages(config_result);

	logEffectiveConfig(config_result.config, config_path,
	                   config_result.file_loaded);

	ETEST_LOG_INFO("MAIN", "program started");

	int exit_code = 0;

	try
	{
		etest::vision::Camera camera(config_result.config.camera);

		etest::vision::VisionProcessor vision(
		    config_result.config.vision);

		etest::Uart uart(config_result.config.uart);

		uart.start();

		etest::AppContext ctx{camera,    vision, uart,
		                      cv::Mat{}, {},     true};

		etest::State current_state = etest::State::START;

		if(!ctx.camera.open())
		{
			ETEST_LOG_ERROR("MAIN",
			                "camera initialization failed; "
			                "switching to ERROR state");

			current_state = etest::State::ERROR;
		}

		while(ctx.running)
		{
			try
			{
				switch(current_state)
				{
				case etest::State::START:
					current_state = etest::state::runStart(ctx);
					break;

				case etest::State::SEARCH:
					current_state = etest::state::runSearch(
					    ctx, config_result.config.search);
					break;

				case etest::State::ERROR:
					current_state = etest::state::runError(ctx);
					break;

				case etest::State::END:
					current_state = etest::state::runEnd(ctx);
					break;

				default:
					ETEST_LOG_ERROR("MAIN",
					                "unknown state; "
					                "switching to ERROR state");

					current_state = etest::State::ERROR;
					break;
				}
			}
			catch(const cv::Exception& error)
			{
				ETEST_LOG_ERROR(
				    "MAIN_LOOP",
				    std::string("OpenCV exception: ") + error.what());

				current_state = etest::State::ERROR;
			}
			catch(const std::exception& error)
			{
				ETEST_LOG_ERROR(
				    "MAIN_LOOP",
				    std::string("std::exception: ") + error.what());

				current_state = etest::State::ERROR;
			}
			catch(...)
			{
				ETEST_LOG_ERROR("MAIN_LOOP", "unknown exception");

				current_state = etest::State::ERROR;
			}
		}

		ctx.camera.release();
		ctx.uart.stop();
	}
	catch(const std::exception& error)
	{
		ETEST_LOG_FATAL("MAIN",
		                std::string("fatal initialization exception: ")
		                    + error.what());

		exit_code = 1;
	}
	catch(...)
	{
		ETEST_LOG_FATAL("MAIN",
		                "unknown fatal initialization exception");

		exit_code = 1;
	}

	ETEST_LOG_INFO("MAIN",
	               exit_code == 0 ? "program exited normally"
	                              : "program exited with error");

	logger.shutdown();

	return exit_code;
}