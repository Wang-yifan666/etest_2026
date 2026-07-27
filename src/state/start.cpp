#include "state/start.hpp"

#include "core/logger.hpp"

#include <chrono>
#include <string>
#include <thread>

namespace etest::state
{

namespace
{

// 节流：同一消息在 throttle_ms 内只输出一次
bool shouldThrottle(std::string& last_msg,
                    std::chrono::steady_clock::time_point& last_time,
                    const std::string& msg, int throttle_ms = 500)
{
	const auto now = std::chrono::steady_clock::now();

	if(msg == last_msg)
	{
		const auto elapsed =
		    std::chrono::duration_cast<std::chrono::milliseconds>(
		        now - last_time);

		if(elapsed.count() < throttle_ms)
		{
			return true;
		}
	}

	last_msg = msg;
	last_time = now;
	return false;
}

} // namespace

State runStart(AppContext& ctx, const RuntimeConfig& runtime,
               const SearchConfig& search_cfg,
               bool show_preview)
{
	ETEST_LOG_INFO("STATE_START",
	               "system initialization started");

	// 日志最终模式信息
	ETEST_LOG_INFO("STATE_START",
	               "runtime.headless="
	                   + std::string(runtime.headless ? "true"
	                                                  : "false")
	                   + ", runtime.allow_keyboard_exit="
	                   + std::string(runtime.allow_keyboard_exit
	                                     ? "true"
	                                     : "false")
	                   + ", runtime.enable_self_check="
	                   + std::string(runtime.enable_self_check
	                                     ? "true"
	                                     : "false")
	                   + ", runtime.enable_auto_recovery="
	                   + std::string(runtime.enable_auto_recovery
	                                     ? "true"
	                                     : "false")
	                   + ", show_preview="
	                   + std::string(show_preview ? "true"
	                                              : "false"));

	// 简化自检
	if(!runtime.enable_self_check)
	{
		ETEST_LOG_WARN("STATE_START",
		               "enable_self_check=false; "
		               "performing simplified self-check");

		// 仅尝试打开摄像头和确认 UART 线程启动
		if(!ctx.camera.isOpened() && !ctx.camera.open())
		{
			ETEST_LOG_WARN(
			    "STATE_START",
			    "camera failed to open during simplified check");
		}

		if(!ctx.uart.isRunning())
		{
			ETEST_LOG_WARN(
			    "STATE_START",
			    "UART not running during simplified check");
		}

		ETEST_LOG_INFO("STATE_START",
		               "simplified check done; entering SEARCH");

		return State::SEARCH;
	}

	// =========================================================================
	// 完整自检
	// =========================================================================

	// 1. 检查摄像头
	ETEST_LOG_INFO("STATE_START", "self-check: opening camera");

	if(!ctx.camera.isOpened() && !ctx.camera.open())
	{
		ETEST_LOG_ERROR("STATE_START",
		                "camera failed to open initially");

		// 重试
		bool camera_ok = false;

		int retry_count = 0;
		const int max_retries = 30; // 防止无限循环
		std::string last_throttle_msg;
		auto last_throttle_time =
		    std::chrono::steady_clock::now()
		    - std::chrono::milliseconds(1000);

		while(!camera_ok && ctx.running && retry_count < max_retries)
		{
			// 检查退出信号
			if(ctx.shutdown_flag != nullptr
			   && ctx.shutdown_flag->load())
			{
				ETEST_LOG_INFO("STATE_START",
				               "shutdown requested during camera retry");

				return State::END;
			}

			const int interval = runtime.camera_retry_interval_ms;

			if(interval > 0)
			{
				std::this_thread::sleep_for(
				    std::chrono::milliseconds(interval));
			}

			if(ctx.camera.open())
			{
				camera_ok = true;

				ETEST_LOG_INFO("STATE_START",
				               "camera opened after "
				                   + std::to_string(retry_count + 1)
				                   + " retries");
			}
			else
			{
				++retry_count;

				const std::string msg =
				    "camera retry " + std::to_string(retry_count)
				    + "/" + std::to_string(max_retries);

				if(!shouldThrottle(last_throttle_msg,
				                   last_throttle_time, msg))
				{
					ETEST_LOG_ERROR("STATE_START", msg);
				}
			}
		}

		if(!camera_ok)
		{
			ETEST_LOG_ERROR("STATE_START",
			                "camera failed to open after "
			                    + std::to_string(max_retries)
			                    + " retries");

			if(!runtime.enable_auto_recovery)
			{
				ctx.last_fault = {FaultSource::CAMERA,
				                  RecoveryAction::SAFE_STOP,
				                  "CAM_OPEN_FAIL",
				                  "camera open failed"};
				return State::END;
			}

			ctx.last_fault = {FaultSource::CAMERA,
			                  RecoveryAction::RETRY,
			                  "CAM_OPEN_FAIL",
			                  "camera open failed; will retry"};
			return State::SEARCH;
		}
	}

	// 2. 连续读取 5 帧确认摄像头稳定
	{
		ETEST_LOG_INFO("STATE_START",
		               "self-check: reading initial frames");

		int frames_read = 0;
		const int required_frames = 5;
		std::string last_throttle_msg;
		auto last_throttle_time =
		    std::chrono::steady_clock::now()
		    - std::chrono::milliseconds(1000);
		int frame_failures = 0;

		while(frames_read < required_frames && ctx.running)
		{
			if(ctx.shutdown_flag != nullptr
			   && ctx.shutdown_flag->load())
			{
				return State::END;
			}

			cv::Mat test_frame;

			if(ctx.camera.read(test_frame) && !test_frame.empty())
			{
				++frames_read;

				if(frames_read == 1)
				{
					ETEST_LOG_INFO(
					    "STATE_START",
					    "actual frame size: "
					        + std::to_string(test_frame.cols)
					        + "x"
					        + std::to_string(test_frame.rows));
				}

				// 如果检测到文件 EOF，记录并继续
				if(ctx.camera.getState()
				   == vision::CameraState::FILE_EOF)
				{
					ETEST_LOG_INFO(
					    "STATE_START",
					    "file source ended after "
					        + std::to_string(frames_read)
					        + " frames");

					break;
				}
			}
			else
			{
				++frame_failures;

				const std::string msg =
				    "frame read failed ("
				    + std::to_string(frame_failures) + ")";

				if(!shouldThrottle(last_throttle_msg,
				                   last_throttle_time, msg))
				{
					ETEST_LOG_ERROR("STATE_START", msg);
				}

				// 最多允许 10 次失败
				if(frame_failures >= 10)
				{
					ETEST_LOG_ERROR(
					    "STATE_START",
					    "too many frame read failures during check");

					ctx.last_fault = {
					    FaultSource::CAMERA,
					    RecoveryAction::RETRY,
					    "FRAME_READ_FAIL",
					    "initial frame read failed"};
					return State::SEARCH;
				}

				std::this_thread::sleep_for(
				    std::chrono::milliseconds(100));
			}
		}

		ETEST_LOG_INFO("STATE_START",
		               "read " + std::to_string(frames_read)
		                   + " initial frames successfully");
	}

	// 3. 检查 UART 后台线程
	ETEST_LOG_INFO("STATE_START", "self-check: checking UART");

	if(!ctx.uart.isRunning())
	{
		ETEST_LOG_WARN("STATE_START", "UART thread is not running");

		// 尝试重启
		if(runtime.enable_auto_recovery)
		{
			ctx.uart.start();
		}

		if(!ctx.uart.isRunning())
		{
			ETEST_LOG_ERROR("STATE_START",
			                "UART failed to start");

			ctx.last_fault = {FaultSource::UART,
			                  RecoveryAction::RECONNECT_UART,
			                  "UART_NOT_RUNNING",
			                  "UART not running"};
		}
	}

	// 4. 发送启动握手消息
	ETEST_LOG_INFO("STATE_START", "self-check: sending handshake");

	if(ctx.uart.isOpen())
	{
		if(!ctx.uart.sendLine("BOOT"))
		{
			ETEST_LOG_WARN("STATE_START",
			               "handshake message send failed");
		}
		else
		{
			ETEST_LOG_INFO("STATE_START",
			               "handshake sent successfully");
		}
	}
	else
	{
		ETEST_LOG_WARN("STATE_START",
		               "UART not open; skipping handshake");
	}

	// 自检完成
	ETEST_LOG_INFO("STATE_START",
	               "self-check passed; entering SEARCH state");

	return State::SEARCH;
}

} // namespace etest::state