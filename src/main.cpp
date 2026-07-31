#include "core/config.hpp"
#include "core/context.hpp"
#include "core/logger.hpp"

#include "state/end.hpp"
#include "state/error.hpp"
#include "state/search.hpp"
#include "state/start.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <exception>
#include <iostream>
#include <string>

namespace
{

	// 信号处理
	std::atomic_bool g_shutdown_requested{false};

	void signalHandler(int /*signum*/)
	{
		g_shutdown_requested.store(true);
	}

	// 日志辅助

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

	void logEffectiveConfig(const etest::ConfigLoadResult& result)
	{
		const auto& config = result.config;

		ETEST_LOG_INFO("CONFIG", "config_dir=" + result.config_dir);

		for(const auto& path: result.loaded_files)
		{
			ETEST_LOG_INFO("CONFIG", "loaded file: " + path);
		}

		for(const auto& path: result.failed_files)
		{
			ETEST_LOG_WARN("CONFIG", "failed to load file: " + path);
		}

		ETEST_LOG_INFO(
		    "CONFIG",
		    "mode.enabled="
		        + std::string(config.mode.enabled ? "true" : "false"));

		ETEST_LOG_INFO("CONFIG", "mode.name=" + config.mode.name);

		if(config.mode.enabled)
		{
			if(result.mode_applied)
			{
				ETEST_LOG_INFO("CONFIG",
				               "applied highest-priority mode config: "
				                   + result.mode_file_path);
			}
			else
			{
				ETEST_LOG_ERROR(
				    "CONFIG",
				    "mode file not applied: " + result.mode_file_path);
			}
		}

		ETEST_LOG_INFO(
		    "CONFIG",
		    "runtime.headless="
		        + std::string(config.runtime.headless ? "true"
		                                              : "false")
		        + ", runtime.allow_keyboard_exit="
		        + std::string(config.runtime.allow_keyboard_exit
		                          ? "true"
		                          : "false")
		        + ", runtime.enable_self_check="
		        + std::string(
		            config.runtime.enable_self_check ? "true" : "false")
		        + ", runtime.enable_auto_recovery="
		        + std::string(config.runtime.enable_auto_recovery
		                          ? "true"
		                          : "false")
		        + ", runtime.camera_retry_interval_ms="
		        + std::to_string(
		            config.runtime.camera_retry_interval_ms)
		        + ", runtime.uart_retry_interval_ms="
		        + std::to_string(
		            config.runtime.uart_retry_interval_ms));

		ETEST_LOG_INFO(
		    "CONFIG",
		    "logger.directory=" + config.logger.directory
		        + ", logger.file="
		        + (config.logger.file ? "true" : "false")
		        + ", logger.terminal="
		        + (config.logger.terminal ? "true" : "false")
		        + ", logger.min_level="
		        + logLevelName(config.logger.min_level)
		        + ", logger.flush_each_write="
		        + (config.logger.flush_each_write ? "true" : "false")
		        + ", logger.throttle_interval_ms="
		        + std::to_string(config.logger.throttle_interval_ms));

		ETEST_LOG_INFO(
		    "CONFIG",
		    "camera.source=" + config.camera.source + ", camera.width="
		        + std::to_string(config.camera.width)
		        + ", camera.height="
		        + std::to_string(config.camera.height)
		        + ", camera.fps=" + std::to_string(config.camera.fps)
		        + ", camera.fourcc=" + config.camera.fourcc);

		ETEST_LOG_INFO(
		    "CONFIG",
		    "vision parameters: "
		    "h1=["
		        + std::to_string(config.vision.red_h1_min) + ","
		        + std::to_string(config.vision.red_h1_max) + "] h2=["
		        + std::to_string(config.vision.red_h2_min) + ","
		        + std::to_string(config.vision.red_h2_max) + "] S_min="
		        + std::to_string(config.vision.saturation_min)
		        + " V_min=" + std::to_string(config.vision.value_min)
		        + " min_area=" + std::to_string(config.vision.min_area)
		        + " kernel="
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

		ETEST_LOG_INFO("CONFIG", config.search.to_string());
	}

	// 简单命令行解析

	struct CliArgs
	{
		bool is_single_file = false;
		std::string path = "config";
		bool valid = true;
		std::string error;
	};

	CliArgs parseArgs(int argc, char* argv[])
	{
		CliArgs args;

		if(argc == 1)
		{
			return args;
		}

		if(argc == 2)
		{
			std::string arg(argv[1]);

			if(arg.rfind("--", 0) == 0)
			{
				args.valid = false;
				args.error = "unknown option: " + arg;
				return args;
			}

			args.path = arg;
			return args;
		}

		if(argc == 3)
		{
			std::string flag(argv[1]);
			std::string value(argv[2]);

			if(flag == "--config-dir")
			{
				args.path = value;
				return args;
			}

			if(flag == "--config")
			{
				args.is_single_file = true;
				args.path = value;
				return args;
			}

			args.valid = false;
			args.error = "unknown option: " + flag;
			return args;
		}

		args.valid = false;
		args.error = "too many arguments";
		return args;
	}

	void printUsage(const char* prog)
	{
		std::cerr << "Usage:\n";
		std::cerr << "  " << prog << "\n";
		std::cerr << "  " << prog << " --config-dir <dir>\n";
		std::cerr << "  " << prog << " --config <main.toml>\n";
	}

} // namespace

int main(int argc, char* argv[])
{
	// 注册信号处理
	std::signal(SIGINT, signalHandler);
	std::signal(SIGTERM, signalHandler);

	const CliArgs args = parseArgs(argc, argv);

	if(!args.valid)
	{
		std::cerr << "[ERROR] " << args.error << "\n";
		printUsage(argv[0]);
		return 1;
	}

	// 1. 初始化配置
	etest::ConfigLoadResult config_result;

	if(args.is_single_file)
	{
		config_result = etest::loadAppConfigFromMainFile(args.path);
	}
	else
	{
		config_result = etest::loadAppConfigFromDir(args.path);
	}

	// 2. 初始化日志系统
	auto& logger = etest::Logger::instance();

	logger.init(config_result.config.logger);

	logConfigMessages(config_result);

	logEffectiveConfig(config_result);

	ETEST_LOG_INFO("MAIN", "program started");

	int exit_code = 0;
	const auto& cfg = config_result.config;

	try
	{
		// 创建摄像头，使用 runtime 的重试间隔
		etest::vision::Camera camera(
		    cfg.camera, cfg.runtime.camera_retry_interval_ms);

		etest::vision::VisionProcessor vision(cfg.vision);

		etest::vision::VideoRecorder recorder(cfg.record);

		etest::Uart uart(cfg.uart);

		uart.start();

		etest::AppContext ctx{camera,
		                      vision,
		                      recorder,
		                      uart,
		                      cv::Mat{},
		                      {},
		                      true,
		                      {},
		                      0,
		                      0,
		                      etest::ExitReason::NORMAL,
		                      &g_shutdown_requested,
		                      false,
		                      0,
		                      {},
		                      etest::AppState::START};

		etest::State current_state = etest::State::START;

		const bool show_preview = cfg.search.show_preview;
		const bool allow_keyboard_exit =
		    cfg.runtime.allow_keyboard_exit;

		while(ctx.running)
		{
			// 信号检查
			if(g_shutdown_requested.load())
			{
				ETEST_LOG_INFO("MAIN", "shutdown signal received");
				ctx.running = false;
				break;
			}

			try
			{
				switch(current_state)
				{
				case etest::State::START:
					current_state = etest::state::runStart(
					    ctx, cfg.runtime, cfg.search, cfg.uart,
					    show_preview);
					break;

				case etest::State::SEARCH:
					current_state = etest::state::runSearch(
					    ctx, cfg.search, cfg.uart, allow_keyboard_exit);
					break;

				case etest::State::ERROR:
					current_state = etest::state::runError(ctx);
					break;

				case etest::State::END:
					current_state = etest::state::runEnd(ctx);
					break;

				default:
					ETEST_LOG_ERROR("MAIN", "unknown state");
					ctx.last_fault = {etest::FaultSource::INTERNAL,
					                  etest::RecoveryAction::SAFE_STOP,
					                  "UNKNOWN_STATE",
					                  "unknown state encountered",
					                  0,
					                  std::chrono::steady_clock::now()};
					current_state = etest::State::ERROR;
					break;
				}

				// 正常执行后清零连续异常计数
				if(current_state != etest::State::ERROR)
				{
					ctx.consecutive_exceptions = 0;
				}
			}
			catch(const cv::Exception& error)
			{
				ctx.consecutive_exceptions++;

				ETEST_LOG_ERROR(
				    "MAIN_LOOP",
				    "state="
				        + std::to_string(
				            static_cast<int>(current_state))
				        + " OpenCV: " + error.what() + " (consecutive="
				        + std::to_string(ctx.consecutive_exceptions)
				        + "/"
				        + std::to_string(etest::AppContext::
				                             kMaxConsecutiveExceptions)
				        + ")");

				ctx.last_fault = {
				    etest::FaultSource::VISION,
				    etest::RecoveryAction::CONTINUE,
				    "CV_EXCEPTION",
				    std::string("OpenCV: ") + error.what(),
				    0,
				    std::chrono::steady_clock::now()};

				if(ctx.consecutive_exceptions
				   >= etest::AppContext::kMaxConsecutiveExceptions)
				{
					ETEST_LOG_FATAL(
					    "MAIN_LOOP",
					    "too many consecutive exceptions ("
					        + std::to_string(ctx.consecutive_exceptions)
					        + "); requesting restart");

					ctx.last_fault.action =
					    etest::RecoveryAction::RESTART_PROCESS;
					current_state = etest::State::ERROR;
				}
			}
			catch(const std::exception& error)
			{
				ctx.consecutive_exceptions++;

				ETEST_LOG_ERROR(
				    "MAIN_LOOP",
				    "state="
				        + std::to_string(
				            static_cast<int>(current_state))
				        + " exception: " + error.what()
				        + " (consecutive="
				        + std::to_string(ctx.consecutive_exceptions)
				        + "/"
				        + std::to_string(etest::AppContext::
				                             kMaxConsecutiveExceptions)
				        + ")");

				ctx.last_fault = {
				    etest::FaultSource::INTERNAL,
				    etest::RecoveryAction::RETRY,
				    "STD_EXCEPTION",
				    std::string("exception: ") + error.what(),
				    0,
				    std::chrono::steady_clock::now()};

				if(ctx.consecutive_exceptions
				   >= etest::AppContext::kMaxConsecutiveExceptions)
				{
					ETEST_LOG_FATAL(
					    "MAIN_LOOP",
					    "too many consecutive exceptions ("
					        + std::to_string(ctx.consecutive_exceptions)
					        + "); requesting restart");

					ctx.last_fault.action =
					    etest::RecoveryAction::RESTART_PROCESS;
					current_state = etest::State::ERROR;
				}
			}
			catch(...)
			{
				ETEST_LOG_FATAL("MAIN_LOOP",
				                "unknown exception; immediate restart");

				ctx.last_fault = {
				    etest::FaultSource::INTERNAL,
				    etest::RecoveryAction::RESTART_PROCESS,
				    "UNKNOWN_EXCEPTION",
				    "unknown exception in main loop",
				    0,
				    std::chrono::steady_clock::now()};
				exit_code = 1;
				break;
			}
		}

		ctx.recorder.release();
		ctx.camera.release();
		ctx.uart.stop();

		if(ctx.exit_reason == etest::ExitReason::RESTART_REQUIRED)
		{
			exit_code = 1;
		}
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