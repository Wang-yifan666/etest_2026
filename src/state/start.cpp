#include "state/start.hpp"

#include "core/logger.hpp"
#include "uart/protocol.hpp"
#include "uart/uart.hpp"

#include <chrono>
#include <string>
#include <thread>

namespace etest::state
{

	namespace
	{

		bool shouldThrottle(
		    std::string& last_msg,
		    std::chrono::steady_clock::time_point& last_time,
		    const std::string& msg, int throttle_ms = 500)
		{
			const auto now = std::chrono::steady_clock::now();

			if(msg == last_msg)
			{
				const auto elapsed = std::chrono::duration_cast<
				    std::chrono::milliseconds>(now - last_time);

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
	               const UartConfig& uart_cfg, bool show_preview)
	{
		ETEST_LOG_INFO("STATE_START", "system initialization started");

		ETEST_LOG_INFO(
		    "STATE_START",
		    "runtime.headless="
		        + std::string(runtime.headless ? "true" : "false")
		        + ", runtime.allow_keyboard_exit="
		        + std::string(runtime.allow_keyboard_exit ? "true"
		                                                  : "false")
		        + ", runtime.enable_self_check="
		        + std::string(runtime.enable_self_check ? "true"
		                                                : "false")
		        + ", runtime.enable_auto_recovery="
		        + std::string(runtime.enable_auto_recovery ? "true"
		                                                   : "false")
		        + ", show_preview="
		        + std::string(show_preview ? "true" : "false"));

		// 简化自检
		if(!runtime.enable_self_check)
		{
			ETEST_LOG_WARN("STATE_START",
			               "enable_self_check=false; "
			               "performing simplified self-check");

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

		bool camera_ready = true;
		bool lower_machine_ready = false;

		// 1. 检查摄像头
		ETEST_LOG_INFO("STATE_START", "self-check: opening camera");

		if(!ctx.camera.isOpened() && !ctx.camera.open())
		{
			ETEST_LOG_ERROR("STATE_START",
			                "camera failed to open initially");

			bool camera_ok = false;

			int retry_count = 0;
			const int max_retries = 30;
			std::string last_throttle_msg;
			auto last_throttle_time = std::chrono::steady_clock::now()
			    - std::chrono::milliseconds(1000);

			while(!camera_ok && ctx.running
			      && retry_count < max_retries)
			{
				if(ctx.shutdown_flag != nullptr
				   && ctx.shutdown_flag->load())
				{
					ETEST_LOG_INFO(
					    "STATE_START",
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

					const std::string msg = "camera retry "
					    + std::to_string(retry_count) + "/"
					    + std::to_string(max_retries);

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

				camera_ready = false;

				if(!runtime.enable_auto_recovery)
				{
					ctx.last_fault = {
					    FaultSource::CAMERA, RecoveryAction::SAFE_STOP,
					    "CAM_OPEN_FAIL", "camera open failed"};
					return State::END;
				}

				ctx.last_fault = {
				    FaultSource::CAMERA, RecoveryAction::REOPEN_CAMERA,
				    "CAM_OPEN_FAIL", "camera open failed; will retry"};
				return State::ERROR;
			}
		}

		// 2. 连续读取 5 帧确认摄像头稳定
		if(camera_ready)
		{
			ETEST_LOG_INFO("STATE_START",
			               "self-check: reading initial frames");

			int frames_read = 0;
			const int warmup_frames = ctx.camera.isFileSource() ? 0 : 5;
			std::string last_throttle_msg;
			auto last_throttle_time = std::chrono::steady_clock::now()
			    - std::chrono::milliseconds(1000);
			int frame_failures = 0;

			while(frames_read < warmup_frames && ctx.running)
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
						        + std::to_string(test_frame.cols) + "x"
						        + std::to_string(test_frame.rows));
					}

					if(ctx.camera.getState()
					   == vision::CameraState::FILE_EOF)
					{
						ETEST_LOG_INFO("STATE_START",
						               "file source ended after "
						                   + std::to_string(frames_read)
						                   + " frames");

						break;
					}
				}
				else
				{
					++frame_failures;

					const std::string msg = "frame read failed ("
					    + std::to_string(frame_failures) + ")";

					if(!shouldThrottle(last_throttle_msg,
					                   last_throttle_time, msg))
					{
						ETEST_LOG_ERROR("STATE_START", msg);
					}

					if(frame_failures >= 10)
					{
						ETEST_LOG_ERROR(
						    "STATE_START",
						    "too many frame read failures during check");

						camera_ready = false;
						break;
					}

					std::this_thread::sleep_for(
					    std::chrono::milliseconds(100));
				}
			}

			if(camera_ready)
			{
				ETEST_LOG_INFO("STATE_START",
				               "read " + std::to_string(frames_read)
				                   + " initial frames successfully");
			}
		}

		// 3. 检查 UART 后台线程
		ETEST_LOG_INFO("STATE_START", "self-check: checking UART");

		if(!ctx.uart.isRunning())
		{
			ETEST_LOG_WARN("STATE_START", "UART thread is not running");

			if(runtime.enable_auto_recovery)
			{
				ctx.uart.start();
			}

			if(!ctx.uart.isRunning())
			{
				ETEST_LOG_ERROR("STATE_START", "UART failed to start");

				ctx.last_fault = {
				    FaultSource::UART, RecoveryAction::RECONNECT_UART,
				    "UART_NOT_RUNNING", "UART not running"};
			}
		}

		// 4. V5 握手：等待可选 BOOT,OK → 发送 PING → 等待 OK,PING →
		//              发送 PROTO? → 等待 PROTO,<expected_major>,<expected_minor> →
		//              发送 CAPS? → 等待 CAPS,...
		ETEST_LOG_INFO("STATE_START", "self-check: V5 handshake");

		if(!ctx.uart.isOpen())
		{
			ETEST_LOG_ERROR("STATE_START",
			                "UART not open; cannot perform handshake");
		}
		else
		{
			const int timeout_ms = uart_cfg.handshake_timeout_ms;
			const int expected_major = uart_cfg.protocol_version_major;
			const int expected_minor = uart_cfg.protocol_version_minor;

			// 步骤 4a：等待可选的 BOOT,OK
			{
				UartMessage msg;
				const auto boot_deadline =
				    std::chrono::steady_clock::now()
				    + std::chrono::milliseconds(timeout_ms / 2);

				while(std::chrono::steady_clock::now() < boot_deadline)
				{
					if(!ctx.running)
						break;

					if(ctx.shutdown_flag != nullptr
					   && ctx.shutdown_flag->load())
					{
						return State::END;
					}

					if(ctx.uart.waitPop(msg, 100))
					{
						if(uart::protocol::isBootOk(msg))
						{
							ETEST_LOG_INFO(
							    "STATE_START",
							    "received BOOT,OK from lower machine");
							break;
						}

						if(msg.type == UartMessageType::ERROR
						   || msg.type == UartMessageType::WARNING)
						{
							ETEST_LOG_WARN("STATE_START",
							               "early message: " + msg.raw);
						}
					}
				}
			}

			// 步骤 4b：发送 PING 并等待 OK,PING
			{
				ETEST_LOG_INFO("STATE_START", "sending PING");
				ctx.uart.sendLine("PING");

				UartMessage msg;
				bool ping_ok = false;
				const auto ping_deadline =
				    std::chrono::steady_clock::now()
				    + std::chrono::milliseconds(timeout_ms);

				while(std::chrono::steady_clock::now() < ping_deadline)
				{
					if(!ctx.running)
						break;

					if(ctx.shutdown_flag != nullptr
					   && ctx.shutdown_flag->load())
					{
						return State::END;
					}

					if(ctx.uart.waitPop(msg, 100))
					{
						if(uart::protocol::isPingResponse(msg))
						{
							ping_ok = true;
							ETEST_LOG_INFO("STATE_START",
							               "received OK,PING");
							break;
						}

						if(msg.type == UartMessageType::ERROR
						   || msg.type == UartMessageType::WARNING)
						{
							ETEST_LOG_WARN(
							    "STATE_START",
							    "message during PING wait: " + msg.raw);
						}
					}
				}

				if(!ping_ok)
				{
					ETEST_LOG_ERROR(
					    "STATE_START",
					    "PING timeout; no OK,PING received");
					ctx.lower_machine_online = false;
				}
			}

			// 步骤 4c：发送 PROTO? 并等待 PROTO,<major>,<minor>
			// 即使 PING 超时也尝试 PROTO 查询
			{
				ETEST_LOG_INFO("STATE_START", "sending PROTO?");
				ctx.uart.sendLine("PROTO?");

				UartMessage msg;
				bool proto_ok = false;
				const auto proto_deadline =
				    std::chrono::steady_clock::now()
				    + std::chrono::milliseconds(timeout_ms);

				while(std::chrono::steady_clock::now() < proto_deadline)
				{
					if(!ctx.running)
						break;

					if(ctx.shutdown_flag != nullptr
					   && ctx.shutdown_flag->load())
					{
						return State::END;
					}

					if(ctx.uart.waitPop(msg, 100))
					{
						if(msg.type == UartMessageType::PROTOCOL)
						{
							auto major =
							    uart::protocol::getProtocolVersionMajor(
							        msg);
							auto minor =
							    uart::protocol::getProtocolVersionMinor(
							        msg);

							if(major.has_value()
							   && *major == expected_major
							   && minor.has_value()
							   && *minor == expected_minor)
							{
								proto_ok = true;
								ctx.lower_machine_online = true;

								ETEST_LOG_INFO(
								    "STATE_START",
								    "protocol version confirmed: "
								        + std::to_string(*major) + "."
								        + std::to_string(*minor));
								break;
							}
							else if(major.has_value())
							{
								ETEST_LOG_ERROR(
								    "STATE_START",
								    "protocol version mismatch: got "
								        + std::to_string(*major) + "."
								        + (minor.has_value()
								               ? std::to_string(*minor)
								               : "?")
								        + ", expected "
								        + std::to_string(expected_major)
								        + "."
								        + std::to_string(
								            expected_minor));
							}
							else
							{
								ETEST_LOG_ERROR(
								    "STATE_START",
								    "invalid PROTO message: "
								        + msg.raw);
							}
						}

						if(msg.type == UartMessageType::ERROR
						   || msg.type == UartMessageType::WARNING)
						{
							ETEST_LOG_WARN("STATE_START",
							               "message during PROTO wait: "
							                   + msg.raw);
						}

						if(uart::protocol::isPingResponse(msg))
						{
							ctx.lower_machine_online = true;
						}
					}
				}

				if(!proto_ok)
				{
					ETEST_LOG_ERROR("STATE_START",
					                "protocol version check failed");
					ctx.lower_machine_online = false;
				}
			}

			// 步骤 4d：发送 CAPS? 并等待 CAPS 响应
			{
				ETEST_LOG_INFO("STATE_START", "sending CAPS?");
				ctx.uart.sendLine("CAPS?");

				UartMessage msg;
				bool caps_ok = false;
				const auto caps_deadline =
				    std::chrono::steady_clock::now()
				    + std::chrono::milliseconds(timeout_ms);

				while(std::chrono::steady_clock::now() < caps_deadline)
				{
					if(!ctx.running)
						break;

					if(ctx.shutdown_flag != nullptr
					   && ctx.shutdown_flag->load())
					{
						return State::END;
					}

					if(ctx.uart.waitPop(msg, 100))
					{
						if(uart::protocol::isCapsResponse(msg))
						{
							caps_ok = true;
							lower_machine_ready = true;
							ctx.lower_machine_online = true;
							ETEST_LOG_INFO("STATE_START",
							               "received CAPS response");
							break;
						}

						if(msg.type == UartMessageType::ERROR
						   || msg.type == UartMessageType::WARNING)
						{
							ETEST_LOG_WARN(
							    "STATE_START",
							    "message during CAPS wait: " + msg.raw);
						}
					}
				}

				if(!caps_ok)
				{
					ETEST_LOG_ERROR("STATE_START",
					                "CAPS? timeout; no CAPS received");
				}
			}
		}

		// 5. 自检结果
		{
			std::string self_check_result;

			if(camera_ready && lower_machine_ready)
			{
				self_check_result =
				    "SELF_CHECK_OK: camera ready, lower controller online";
			}
			else if(camera_ready && !lower_machine_ready)
			{
				self_check_result =
				    "SELF_CHECK_DEGRADED: camera ready, lower controller offline";

				if(!runtime.enable_auto_recovery)
				{
					ETEST_LOG_ERROR(
					    "STATE_START",
					    "lower controller offline and auto-recovery disabled");
					ctx.running = false;
					return State::END;
				}

				ETEST_LOG_WARN(
				    "STATE_START",
				    "auto-recovery enabled; proceeding in degraded mode");
			}
			else if(!camera_ready && lower_machine_ready)
			{
				self_check_result =
				    "SELF_CHECK_DEGRADED: camera failed, lower controller online";
			}
			else
			{
				self_check_result =
				    "SELF_CHECK_FAILED: camera failed, lower controller offline";

				if(!runtime.enable_auto_recovery)
				{
					ctx.running = false;
					return State::END;
				}
			}

			ETEST_LOG_INFO("STATE_START", self_check_result);
		}

		// 6. 加载 YOLO 模型（detector=yolo 时）
		if(search_cfg.detector == "yolo")
		{
			ETEST_LOG_INFO(
			    "STATE_START",
			    "loading YOLO model: backend="
			        + search_cfg.yolo_backend
			        + " input="
			        + std::to_string(search_cfg.nn_input_width) + "x"
			        + std::to_string(search_cfg.nn_input_height)
			        + " threads="
			        + std::to_string(search_cfg.nn_threads));

			if(!ctx.vision.loadYoloModel(search_cfg))
			{
				ETEST_LOG_ERROR(
				    "STATE_START",
				    "YOLO model failed to load; will retry in SEARCH");
			}
			else
			{
				ETEST_LOG_INFO(
				    "STATE_START",
				    "YOLO model loaded: backend="
				        + std::string(
				            ctx.vision.yoloBackendName()));
			}

			// 初始化任务会话
			ctx.task.reset();
			ETEST_LOG_INFO("STATE_START",
			               "initial session_id="
			                   + std::to_string(ctx.task.session_id));
		}

		ETEST_LOG_INFO("STATE_START",
		               "self-check complete; entering SEARCH state");

		return State::SEARCH;
	}

} // namespace etest::state