#include "state/search.hpp"

#include "core/config.hpp"
#include "core/logger.hpp"
#include "uart/protocol.hpp"
#include "uart/uart.hpp"

#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>

#include <chrono>
#include <cmath>
#include <string>
#include <thread>

namespace etest::state
{

namespace
{

bool shouldThrottle(
    const std::string& msg, std::string& last_msg,
    std::chrono::steady_clock::time_point& last_time,
    int throttle_ms = 2000)
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

// 运行期下位机重连握手状态
enum class LinkState
{
	ONLINE,
	WAIT_PING,
	WAIT_PROTO
};

} // namespace

State runSearch(AppContext& ctx, const SearchConfig& search_cfg,
                const UartConfig& uart_cfg,
                bool allow_keyboard_exit)
{
	ETEST_LOG_INFO("SEARCH", "entering search loop");

	const bool show_preview = search_cfg.show_preview;

	if(search_cfg.enable_nn && !ctx.vision.isNnLoaded())
	{
		ETEST_LOG_INFO(
		    "SEARCH", "loading NN model: " + search_cfg.model_path);

		if(ctx.vision.loadNnModel(
		       search_cfg.model_path, search_cfg.class_names_path,
		       search_cfg.nn_confidence_threshold,
		       search_cfg.nn_nms_threshold))
		{
			ETEST_LOG_INFO("SEARCH",
			               "NN model loaded; detection enabled");
		}
		else
		{
			ETEST_LOG_WARN("SEARCH",
			               "NN model failed to load; "
			               "running without detection");
		}
	}

	const std::string preview_window = "Camera Preview";
	bool preview_open = false;

	const int throttle_ms = 500;
	auto last_throttle_time = std::chrono::steady_clock::now()
	    - std::chrono::milliseconds(throttle_ms + 1);

	uint64_t frame_count = 0;

	const auto target_frame_interval =
	    std::chrono::milliseconds(33);
	auto last_frame_time = std::chrono::steady_clock::now();

	// 心跳定时
	auto last_ping_time = std::chrono::steady_clock::now();
	auto last_ping_response_time = std::chrono::steady_clock::now();

	// 重连握手状态
	LinkState link_state = ctx.lower_machine_online
	    ? LinkState::ONLINE
	    : LinkState::WAIT_PING;

	// 心跳离线标识（非 static，循环外局部变量）
	bool heartbeat_offline = !ctx.lower_machine_online;

	// 发送失败节流
	std::string last_send_error;
	auto last_send_error_time = std::chrono::steady_clock::now()
	    - std::chrono::milliseconds(5000);
	constexpr int send_error_throttle_ms = 2000;

	// 心跳超时日志节流
	std::string last_hb_error;
	auto last_hb_error_time = last_send_error_time;

	const int heartbeat_interval_ms = uart_cfg.heartbeat_interval_ms;
	const int heartbeat_timeout_ms = uart_cfg.heartbeat_timeout_ms;
	const int expected_proto_version = uart_cfg.protocol_version;

	while(ctx.running)
	{
		if(ctx.shutdown_flag != nullptr
		   && ctx.shutdown_flag->load())
		{
			ETEST_LOG_INFO("SEARCH", "shutdown signal detected");
			break;
		}

		++frame_count;

		const auto loop_start = std::chrono::steady_clock::now();
		const auto since_last_frame =
		    std::chrono::duration_cast<std::chrono::milliseconds>(
		        loop_start - last_frame_time);

		if(since_last_frame < target_frame_interval)
		{
			std::this_thread::sleep_for(target_frame_interval
			                            - since_last_frame);
		}

		// 1) 读取帧
		if(!ctx.camera.read(ctx.frame))
		{
			if(ctx.camera.getState()
			   == vision::CameraState::FILE_EOF)
			{
				ETEST_LOG_INFO("SEARCH",
				               "file source ended; exiting search");
				break;
			}

			ETEST_LOG_ERROR("SEARCH", "frame read failed");

			ctx.last_fault = {
			    FaultSource::CAMERA, RecoveryAction::REOPEN_CAMERA,
			    "SEARCH_FRAME_READ", "frame read failed in SEARCH"};
			return State::ERROR;
		}

		last_frame_time = std::chrono::steady_clock::now();

		// 2) 接收并处理 UART 消息（每循环最多 16 条）
		{
			UartMessage msg;
			int processed = 0;
			constexpr int max_per_loop = 16;

			while(processed < max_per_loop && ctx.uart.tryPop(msg))
			{
				++processed;

				// 处理 BOOT,OK（下位机重启）
				if(uart::protocol::isBootOk(msg))
				{
					ETEST_LOG_WARN("SEARCH",
					               "lower machine reboot detected: "
					                   + msg.raw);

					ctx.lower_machine_online = false;
					link_state = LinkState::WAIT_PING;
					heartbeat_offline = true;

					// 发送 PING 启动重新握手
					ctx.uart.sendLine("PING");
					last_ping_time =
					    std::chrono::steady_clock::now();
					continue;
				}

				// 处理 OK,PING
				if(uart::protocol::isPingResponse(msg))
				{
					// 仅 OK,PING 更新心跳响应时间
					last_ping_response_time =
					    std::chrono::steady_clock::now();

					if(link_state == LinkState::WAIT_PING)
					{
						// 收到 PING 响应，发送 PROTO? 进入版本确认
						ctx.uart.sendLine("PROTO?");
						link_state = LinkState::WAIT_PROTO;
						continue;
					}

					if(link_state == LinkState::ONLINE)
					{
						// 正常心跳响应
						if(heartbeat_offline)
						{
							ETEST_LOG_INFO(
							    "SEARCH",
							    "heartbeat restored; lower machine online");
							heartbeat_offline = false;
						}

						ctx.lower_machine_online = true;
					}

					continue;
				}

				// 处理 PROTO 响应
				if(msg.type == UartMessageType::PROTOCOL)
				{
					auto proto_ver =
					    uart::protocol::getProtocolVersion(msg);

					if(link_state == LinkState::WAIT_PROTO)
					{
						if(proto_ver.has_value()
						   && *proto_ver == expected_proto_version)
						{
							ETEST_LOG_INFO(
							    "SEARCH",
							    "protocol version re-confirmed: "
							        + std::to_string(*proto_ver));

							ctx.lower_machine_online = true;
							link_state = LinkState::ONLINE;

							if(heartbeat_offline)
							{
								ETEST_LOG_INFO(
								    "SEARCH",
								    "heartbeat restored; lower machine online");
								heartbeat_offline = false;
							}
						}
						else if(proto_ver.has_value())
						{
							ETEST_LOG_ERROR(
							    "SEARCH",
							    "protocol version mismatch: got "
							        + std::to_string(*proto_ver)
							        + ", expected "
							        + std::to_string(
							            expected_proto_version));
							// 保持离线，不改变 link_state
						}
						else
						{
							ETEST_LOG_ERROR(
							    "SEARCH",
							    "invalid PROTO message: " + msg.raw);
						}
					}
					else
					{
						// ONLINE 状态下也可能收到主动 PROTO 消息
						if(proto_ver.has_value()
						   && *proto_ver == expected_proto_version)
						{
							ETEST_LOG_INFO(
							    "SEARCH",
							    "protocol version re-confirmed: "
							        + std::to_string(*proto_ver));
							ctx.lower_machine_online = true;
							last_ping_response_time =
							    std::chrono::steady_clock::now();
						}
						else if(proto_ver.has_value())
						{
							ETEST_LOG_ERROR(
							    "SEARCH",
							    "protocol version mismatch: got "
							        + std::to_string(*proto_ver)
							        + ", expected "
							        + std::to_string(
							            expected_proto_version));
						}
						else
						{
							ETEST_LOG_ERROR(
							    "SEARCH",
							    "invalid PROTO message: " + msg.raw);
						}
					}
					continue;
				}

				// 处理 ERR / WARN
				if(msg.type == UartMessageType::ERROR
				   || msg.type == UartMessageType::WARNING)
				{
					ETEST_LOG_WARN("SEARCH",
					               "UART message: " + msg.raw);
				}

				// 注意：不在此处更新 last_ping_response_time
				// 只有 OK,PING 才更新心跳响应时间
			}
		}

		// 3) 心跳发送与超时
		{
			const auto now = std::chrono::steady_clock::now();
			const auto elapsed =
			    std::chrono::duration_cast<std::chrono::milliseconds>(
			        now - last_ping_time)
			        .count();

			if(elapsed >= heartbeat_interval_ms)
			{
				if(ctx.uart.isOpen())
				{
					ctx.uart.sendLine("PING");
				}
				last_ping_time = now;
			}

			// 超时判断（仅对比 OK,PING 响应时间）
			const auto response_elapsed =
			    std::chrono::duration_cast<std::chrono::milliseconds>(
			        now - last_ping_response_time)
			        .count();

			if(response_elapsed > heartbeat_timeout_ms)
			{
				if(ctx.lower_machine_online)
				{
					ctx.lower_machine_online = false;
					heartbeat_offline = true;

					const std::string err_msg = "heartbeat timeout";
					if(!shouldThrottle(err_msg, last_hb_error,
					                   last_hb_error_time))
					{
						ETEST_LOG_WARN(
						    "SEARCH",
						    "heartbeat timeout, lower machine offline");
					}
				}
			}
		}

		// 4) 执行视觉算法（无论是否预览）
		ctx.vision_result = ctx.vision.process(
		    ctx.frame, vision::VisionMode::ColorTarget);

		// 5) 发送目标结果（无论是否预览）
		if(ctx.uart.isOpen())
		{
			if(ctx.vision_result.valid)
			{
				auto line = uart::protocol::makeTargetLine(
				    ++ctx.uart_seq, ctx.vision_result.x,
				    ctx.vision_result.y, ctx.vision_result.angle,
				    ctx.vision_result.confidence);

				if(line.has_value())
				{
					if(!ctx.uart.sendLine(*line))
					{
						const std::string err_msg =
						    "failed to send TARGET";

						if(!shouldThrottle(err_msg, last_send_error,
						                   last_send_error_time,
						                   send_error_throttle_ms))
						{
							ETEST_LOG_ERROR("SEARCH", err_msg);
						}
					}
				}
				else
				{
					ETEST_LOG_ERROR(
					    "SEARCH",
					    "invalid vision result values; sending LOST");

					const std::string lost_line =
					    uart::protocol::makeLostLine(++ctx.uart_seq);

					if(!ctx.uart.sendLine(lost_line))
					{
						const std::string err_msg =
						    "failed to send LOST (invalid target)";

						if(!shouldThrottle(err_msg, last_send_error,
						                   last_send_error_time,
						                   send_error_throttle_ms))
						{
							ETEST_LOG_ERROR("SEARCH", err_msg);
						}
					}
				}
			}
			else
			{
				const std::string lost_line =
				    uart::protocol::makeLostLine(++ctx.uart_seq);

				if(!ctx.uart.sendLine(lost_line))
				{
					const std::string err_msg =
					    "failed to send LOST";

					if(!shouldThrottle(err_msg, last_send_error,
					                   last_send_error_time,
					                   send_error_throttle_ms))
					{
						ETEST_LOG_ERROR("SEARCH", err_msg);
					}
				}
			}
		}

		// 6) 节流日志
		const auto now = std::chrono::steady_clock::now();
		const auto elapsed =
		    std::chrono::duration_cast<std::chrono::milliseconds>(
		        now - last_throttle_time);

		if(elapsed.count() >= throttle_ms)
		{
			std::string msg =
			    "searching... frame=" + std::to_string(frame_count);

			if(ctx.vision_result.valid)
			{
				msg += " | target: ("
				    + std::to_string(ctx.vision_result.x) + ","
				    + std::to_string(ctx.vision_result.y)
				    + ") angle="
				    + std::to_string(ctx.vision_result.angle);
			}
			else
			{
				msg += " | target: LOST";
			}

			ETEST_LOG_INFO("SEARCH", msg);

			last_throttle_time = now;
		}

		// 7) 仅预览模式绘制和键盘
		if(show_preview)
		{
			cv::Mat display = ctx.frame.clone();
			ctx.vision.drawDebugInfo(display, ctx.vision_result);

			if(!preview_open)
			{
				cv::namedWindow(
				    preview_window,
				    cv::WINDOW_NORMAL | cv::WINDOW_KEEPRATIO);
				cv::resizeWindow(preview_window, 1280, 480);
				preview_open = true;
			}

			cv::imshow(preview_window, display);

			const int key = cv::waitKey(1) & 0xFF;

			if(allow_keyboard_exit
			   && (key == 27 || key == 'q' || key == 'Q'))
			{
				ETEST_LOG_INFO("SEARCH",
				               "exit requested via keyboard");
				break;
			}
		}
		else
		{
			std::this_thread::sleep_for(
			    std::chrono::milliseconds(1));
		}
	}

	if(show_preview && preview_open)
	{
		cv::destroyAllWindows();
	}

	ETEST_LOG_INFO("SEARCH", "exiting search loop");

	return State::END;
}

} // namespace etest::state