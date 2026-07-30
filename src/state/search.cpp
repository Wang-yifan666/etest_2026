#include "state/search.hpp"

#include "core/config.hpp"
#include "core/logger.hpp"
#include "uart/protocol.hpp"
#include "uart/uart.hpp"
#include "vision/latest_frame_capture.hpp"
#include "vision/vision.hpp"

#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <limits>
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
					return true;
			}
			last_msg = msg;
			last_time = now;
			return false;
		}

	} // namespace

	State runSearch(AppContext& ctx, const SearchConfig& search_cfg,
	                const UartConfig& uart_cfg,
	                bool allow_keyboard_exit)
	{
		ETEST_LOG_INFO("SEARCH", "entering search loop");

		const bool show_preview = search_cfg.show_preview;
		const std::string preview_window = "Camera Preview";
		bool preview_open = false;

		const bool can_show_preview =
		    show_preview && (std::getenv("DISPLAY") != nullptr);
		if(show_preview && !can_show_preview)
		{
			ETEST_LOG_WARN("SEARCH",
			               "DISPLAY not set; forcing headless mode");
		}

		const int throttle_ms = 500;
		auto last_throttle_time = std::chrono::steady_clock::now()
		    - std::chrono::milliseconds(throttle_ms + 1);

		// ── 性能基线统计 ──
		const auto perf_interval = std::chrono::milliseconds(1000);
		auto last_perf_log = std::chrono::steady_clock::now();
		int perf_vision_calls = 0;
		std::int64_t perf_vision_total_us = 0;
		std::int64_t perf_loop_total_us = 0;
		std::uint64_t dropped_frames_total = 0;
		std::uint64_t last_perf_captured = 0;
		std::uint64_t last_perf_processed = 0;

		uint64_t frame_count = 0;

		// 帧率控制（仅文件源）
		const bool is_file = ctx.camera.isFileSource();
		const bool realtime_playback = ctx.camera.realtimePlayback();
		const int playback_fps = ctx.camera.playbackFps();
		const bool should_pace_frames =
		    is_file && realtime_playback && playback_fps > 0;
		const auto frame_interval = should_pace_frames
		    ? std::chrono::milliseconds(1000 / playback_fps)
		    : std::chrono::milliseconds(0);
		auto last_frame_time = std::chrono::steady_clock::now();

		// 心跳定时
		auto last_ping_time = std::chrono::steady_clock::now();
		auto last_ping_response_time = std::chrono::steady_clock::now();

		// 重连握手
		enum class LinkState
		{
			ONLINE,
			WAIT_PING,
			WAIT_PROTO
		};
		LinkState link_state = ctx.lower_machine_online
		    ? LinkState::ONLINE
		    : LinkState::WAIT_PING;
		auto link_state_since = std::chrono::steady_clock::now();
		int handshake_retry_count = 0;
		bool heartbeat_offline = !ctx.lower_machine_online;

		const int heartbeat_interval_ms =
		    uart_cfg.heartbeat_interval_ms;
		const int heartbeat_timeout_ms = uart_cfg.heartbeat_timeout_ms;
		const int expected_proto_major =
		    uart_cfg.protocol_version_major;
		const int expected_proto_minor =
		    uart_cfg.protocol_version_minor;

		// 发送失败节流
		std::string last_send_error;
		auto last_send_error_time = std::chrono::steady_clock::now()
		    - std::chrono::milliseconds(5000);
		constexpr int send_error_throttle_ms = 2000;
		std::string last_hb_error;
		auto last_hb_error_time = last_send_error_time;

		int frame_failures = 0;

		// VSESSION 重发
		auto last_vsession_send = std::chrono::steady_clock::now();
		auto last_ball_send = std::chrono::steady_clock::now();
		auto last_contest_start_send = std::chrono::steady_clock::now();

		// CONTESTSTART 等待中检测球移动
		bool contest_start_sent = false;
		auto contest_start_since = std::chrono::steady_clock::now();

		// DONE 标记
		bool done_received = false;

		// ── 最新帧采集（仅实时摄像头）──
		const bool use_latest_capture = !is_file;
		vision::LatestFrameCapture capture(ctx.camera);

		if(use_latest_capture)
		{
			if(!capture.start())
			{
				ETEST_LOG_ERROR(
				    "SEARCH",
				    "failed to start latest-frame capture worker");
				ctx.last_fault = {FaultSource::CAMERA,
				                  RecoveryAction::REOPEN_CAMERA,
				                  "CAPTURE_THREAD_START",
				                  "failed to start capture worker"};
				return State::ERROR;
			}
		}

		std::uint64_t last_processed_sequence = 0;

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

			// ── 2) UART 消息处理（移到帧读取之前，确保及时处理）──
			{
				UartMessage msg;
				int processed = 0;
				constexpr int max_per_loop = 16;
				while(processed < max_per_loop && ctx.uart.tryPop(msg))
				{
					++processed;

					// M000X → 记录题目编号，开始标定流程
					if(uart::protocol::isMissionCode(msg))
					{
						int code =
						    uart::protocol::parseMissionCode(msg);
						std::string mode_name =
						    uart::protocol::missionModeName(code);
						ETEST_LOG_INFO(
						    "SEARCH",
						    "received mission code: " + msg.tag
						        + " → mode=" + mode_name);
						ctx.task.command = code;
						ctx.task.problem_number =
						    code + 1; // 1→H2, ..., 5→H6
						ctx.task.active_mode = mode_name;
						ctx.task.mission_received = true;
						ctx.task_phase = TaskPhase::CALIBRATING;
						ctx.task.contest_start_acked = false;
						ctx.task.contest_start_sent = false;
						contest_start_sent = false;

						// 重新初始化 VSESSION
						ctx.task.vsession_confirmed = false;
						ctx.task.session_id =
						    static_cast<std::uint32_t>(
						        std::chrono::steady_clock::now()
						            .time_since_epoch()
						            .count()
						        & 0xFFFFFFFFu);
						ctx.task.vision_epoch_ns =
						    std::chrono::steady_clock::now()
						        .time_since_epoch()
						        .count();
						ctx.vision.setVisionEpoch(
						    ctx.task.vision_epoch_ns);
						ctx.vision.resetYoloSession();
						ctx.task.seq = 0;

						// 记录 vision_epoch（steady_clock 时间点）
						ctx.task.vision_epoch =
						    std::chrono::steady_clock::now();

						ETEST_LOG_INFO(
						    "SEARCH",
						    "new vsession session_id="
						        + std::to_string(ctx.task.session_id));
						last_vsession_send =
						    std::chrono::steady_clock::now()
						    - std::chrono::milliseconds(
						        search_cfg.vsession_retry_interval_ms);
						continue;
					}

					// BOOT,OK → 清除 VSESSION，重新握手
					if(uart::protocol::isBootOk(msg))
					{
						ETEST_LOG_WARN(
						    "SEARCH",
						    "lower machine reboot: " + msg.raw);
						ctx.lower_machine_online = false;
						ctx.task.mission_received = false;
						ctx.task.command = 0;
						link_state = LinkState::WAIT_PING;
						heartbeat_offline = true;
						ctx.task.vsession_confirmed = false;
						ctx.task.contest_start_acked = false;
						ctx.vision.resetYoloSession();
						ctx.uart.sendLine("PING");
						last_ping_time =
						    std::chrono::steady_clock::now();
						continue;
					}

					// PING 响应
					if(uart::protocol::isPingResponse(msg))
					{
						last_ping_response_time =
						    std::chrono::steady_clock::now();
						if(link_state == LinkState::WAIT_PING)
						{
							ctx.uart.sendLine("PROTO?");
							link_state = LinkState::WAIT_PROTO;
							link_state_since =
							    std::chrono::steady_clock::now();
							continue;
						}
						if(link_state == LinkState::ONLINE)
						{
							if(heartbeat_offline)
							{
								ETEST_LOG_INFO("SEARCH",
								               "heartbeat restored");
								heartbeat_offline = false;
							}
							ctx.lower_machine_online = true;
						}
						continue;
					}

					// CAPS 响应（重连握手期间）
					if(uart::protocol::isCapsResponse(msg)
					   && link_state == LinkState::WAIT_PROTO)
					{
						ETEST_LOG_INFO(
						    "SEARCH",
						    "CAPS received; handshake complete");
						ctx.lower_machine_online = true;
						link_state = LinkState::ONLINE;
						heartbeat_offline = false;
						handshake_retry_count = 0;
						continue;
					}

					// PROTO 响应
					if(msg.type == UartMessageType::PROTOCOL)
					{
						auto major =
						    uart::protocol::getProtocolVersionMajor(
						        msg);
						auto minor =
						    uart::protocol::getProtocolVersionMinor(
						        msg);
						if(link_state == LinkState::WAIT_PROTO)
						{
							if(major.has_value()
							   && *major == expected_proto_major
							   && minor.has_value()
							   && *minor == expected_proto_minor)
							{
								ETEST_LOG_INFO(
								    "SEARCH",
								    "proto version confirmed: "
								        + std::to_string(*major) + "."
								        + std::to_string(*minor));
								// 发送 CAPS?
								ctx.uart.sendLine("CAPS?");
								link_state_since =
								    std::chrono::steady_clock::now();
							}
							else if(major.has_value())
							{
								ETEST_LOG_ERROR(
								    "SEARCH",
								    "proto version mismatch: got "
								        + std::to_string(*major) + "."
								        + (minor.has_value()
								               ? std::to_string(*minor)
								               : "?"));
							}
						}
						else
						{
							if(major.has_value()
							   && *major == expected_proto_major
							   && minor.has_value()
							   && *minor == expected_proto_minor)
							{
								ctx.lower_machine_online = true;
								last_ping_response_time =
								    std::chrono::steady_clock::now();
							}
						}
						continue;
					}

					// VSESSION ACK
					if(uart::protocol::isVsessionAck(
					       msg, ctx.task.session_id))
					{
						ctx.task.vsession_confirmed = true;
						ETEST_LOG_INFO("SEARCH", "VSESSION confirmed");
						continue;
					}

					// CONTESTSTART ACK
					if(uart::protocol::isContestStartAck(msg))
					{
						ctx.task.contest_start_acked = true;
						ctx.task_phase = TaskPhase::CONTEST;
						ETEST_LOG_INFO("SEARCH",
						               "CONTESTSTART confirmed");
						continue;
					}

					// DONE
					if(auto done = uart::protocol::parseDone(msg))
					{
						ETEST_LOG_INFO("SEARCH",
						               "DONE received: " + done->mode
						                   + " result=" + done->result);
						done_received = true;
						continue;
					}

					// ERR / WARN
					if(msg.type == UartMessageType::ERROR
					   || msg.type == UartMessageType::WARNING)
					{
						ETEST_LOG_WARN("SEARCH",
						               "UART message: " + msg.raw);
					}
				}
			}

			// ── 3) 心跳发送与超时 ──
			{
				const auto now = std::chrono::steady_clock::now();
				const auto elapsed =
				    std::chrono::duration_cast<
				        std::chrono::milliseconds>(now - last_ping_time)
				        .count();
				if(!ctx.uart.isOpen())
				{
					ctx.lower_machine_online = false;
					link_state = LinkState::WAIT_PING;
					heartbeat_offline = true;
					handshake_retry_count = 0;
				}
				if(elapsed >= heartbeat_interval_ms)
				{
					if(ctx.uart.isOpen())
						ctx.uart.sendLine("PING");
					last_ping_time = now;
				}
				const auto resp_elapsed =
				    std::chrono::duration_cast<
				        std::chrono::milliseconds>(
				        now - last_ping_response_time)
				        .count();
				if(resp_elapsed > heartbeat_timeout_ms)
				{
					if(ctx.lower_machine_online)
					{
						ctx.lower_machine_online = false;
						heartbeat_offline = true;
						link_state = LinkState::WAIT_PING;
						link_state_since = now;
						handshake_retry_count = 0;
						ctx.task.vsession_confirmed = false;
						const std::string err_msg = "heartbeat timeout";
						if(!shouldThrottle(err_msg, last_hb_error,
						                   last_hb_error_time))
							ETEST_LOG_WARN("SEARCH", err_msg);
					}
				}
				// WAIT_PROTO 超时重试
				if(link_state == LinkState::WAIT_PROTO)
				{
					const auto proto_elapsed =
					    std::chrono::duration_cast<
					        std::chrono::milliseconds>(
					        now - link_state_since)
					        .count();
					if(proto_elapsed >= uart_cfg.handshake_timeout_ms)
					{
						if(handshake_retry_count < 3)
						{
							++handshake_retry_count;
							ctx.uart.sendLine("PROTO?");
							link_state_since = now;
							ETEST_LOG_WARN(
							    "SEARCH",
							    "PROTO retry "
							        + std::to_string(
							            handshake_retry_count));
						}
						else
						{
							handshake_retry_count = 0;
							link_state = LinkState::WAIT_PING;
							ctx.lower_machine_online = false;
							link_state_since = now;
						}
					}
				}
			}

			// ── 4) VSESSION 握手（仅在收到 M000X 后发送）──
			if(ctx.task.mission_received && !ctx.task.vsession_confirmed
			   && ctx.uart.isOpen() && link_state == LinkState::ONLINE)
			{
				const auto now = std::chrono::steady_clock::now();
				const auto elapsed = std::chrono::duration_cast<
				                         std::chrono::milliseconds>(
				                         now - last_vsession_send)
				                         .count();
				if(elapsed >= search_cfg.vsession_retry_interval_ms)
				{
					auto line = uart::protocol::makeVsessionLine(
					    ctx.task.session_id,
					    search_cfg.nominal_fps * 100,
					    search_cfg.camera_id);
					ctx.uart.sendLine(line);
					last_vsession_send = now;
				}
			}

			// ── 1) 获取帧 ──
			vision::FramePacket packet;
			bool has_new_frame = false;

			if(use_latest_capture)
			{
				has_new_frame = capture.tryGetLatest(
				    packet, last_processed_sequence);

				if(!has_new_frame)
				{
					// 检查采集线程是否报错
					if(capture.state()
					   == vision::CaptureWorkerState::CAMERA_ERROR)
					{
						ETEST_LOG_ERROR(
						    "SEARCH",
						    "latest-frame capture worker failed");
						ctx.last_fault = {FaultSource::CAMERA,
						                  RecoveryAction::REOPEN_CAMERA,
						                  "CAPTURE_READ_FAILED",
						                  "capture worker failed"};
						return State::ERROR;
					}

					// 无新帧，短暂休眠后继续（保证 UART/心跳及时）
					std::this_thread::sleep_for(
					    std::chrono::milliseconds(1));
					continue;
				}

				// 过滤任务开始前采集的旧帧
				if(ctx.task.mission_received
				   && packet.received_at < ctx.task.vision_epoch)
				{
					last_processed_sequence = packet.sequence;
					continue;
				}
			}
			else
			{
				// 文件源：保留原有同步读取逻辑

				// 帧率控制
				if(frame_interval.count() > 0)
				{
					const auto since_last = std::chrono::duration_cast<
					    std::chrono::milliseconds>(loop_start
					                               - last_frame_time);
					if(since_last < frame_interval)
						std::this_thread::sleep_for(frame_interval
						                            - since_last);
				}

				if(!ctx.camera.read(ctx.frame))
				{
					++frame_failures;
					if(ctx.camera.getState()
					   == vision::CameraState::FILE_EOF)
					{
						ETEST_LOG_INFO(
						    "SEARCH",
						    "file source ended; exiting search");
						break;
					}

					if(frame_failures >= 3)
					{
						ETEST_LOG_ERROR(
						    "SEARCH",
						    "camera failed for 3 consecutive frames");
						ctx.last_fault = {
						    FaultSource::CAMERA,
						    RecoveryAction::REOPEN_CAMERA,
						    "SEARCH_FRAME_READ",
						    "camera failed for 3 consecutive frames"};
						return State::ERROR;
					}
					std::this_thread::sleep_for(
					    std::chrono::milliseconds(20));
					continue;
				}
				frame_failures = 0;
				last_frame_time = std::chrono::steady_clock::now();

				// 将同步读取的帧包装为 FramePacket
				packet.frame = ctx.frame;
				packet.received_at = std::chrono::steady_clock::now();
				packet.sequence = frame_count;
				has_new_frame = true;
			}

			// 更新 ctx.frame 和丢帧统计
			if(has_new_frame)
			{
				ctx.frame = packet.frame;

				// 丢帧统计：sequence 跳跃说明有帧被覆盖（仅在最新帧模式下有意义）
				if(use_latest_capture && last_processed_sequence != 0
				   && packet.sequence > last_processed_sequence + 1)
				{
					dropped_frames_total +=
					    packet.sequence - last_processed_sequence - 1;
				}

				last_processed_sequence = packet.sequence;
			}

			// ── 5) 视觉处理 + BALL 发送（仅在收到 M000X 后）──
			if(ctx.task.mission_received && ctx.task.vsession_confirmed
			   && has_new_frame)
			{
				const auto vision_start =
				    std::chrono::steady_clock::now();
				ctx.vision_result = ctx.vision.process(
				    ctx.frame, vision::VisionMode::Ball);
				const auto vision_end =
				    std::chrono::steady_clock::now();
				const auto vision_us = std::chrono::duration_cast<
				                           std::chrono::microseconds>(
				                           vision_end - vision_start)
				                           .count();
				++perf_vision_calls;
				perf_vision_total_us += vision_us;

				const auto now = std::chrono::steady_clock::now();
				const auto ball_elapsed =
				    std::chrono::duration_cast<
				        std::chrono::milliseconds>(now - last_ball_send)
				        .count();

				if(ball_elapsed >= search_cfg.result_send_interval_ms
				   || ctx.task_phase == TaskPhase::CALIBRATING)
				{
					// ── 修正后的 capture_ms / age_ms ──
					// capture_ms: 帧采集时刻相对于视觉会话开始的时间（毫秒）
					// age_ms: 帧从采集到发送 BALL 的延迟（毫秒）

					// 使用 packet.received_at（采集线程打的时间戳）代替 now
					const auto capture_ms_64 =
					    std::chrono::duration_cast<
					        std::chrono::milliseconds>(
					        packet.received_at - ctx.task.vision_epoch)
					        .count();
					const auto age_ms_64 =
					    std::chrono::duration_cast<
					        std::chrono::milliseconds>(
					        now - packet.received_at)
					        .count();

					const std::uint32_t capture_ms = static_cast<
					    std::uint32_t>(std::clamp<std::int64_t>(
					    capture_ms_64, 0,
					    std::numeric_limits<std::uint32_t>::max()));
					const std::uint32_t age_ms = static_cast<
					    std::uint32_t>(std::clamp<std::int64_t>(
					    age_ms_64, 0,
					    std::numeric_limits<std::uint32_t>::max()));

					std::string status;
					int pos = 0;
					float conf = 0.0F;

					if(ctx.vision_result.calibrated
					   && ctx.vision_result.valid)
					{
						status = "OK";
						pos = ctx.vision_result.position_0p1mm;
						conf = static_cast<float>(
						    ctx.vision_result.confidence);
					}
					else if(!ctx.vision_result.calibrated)
					{
						status = "CALIB";
					}
					else if(!ctx.vision_result.valid)
					{
						status = "LOST";
					}
					else
					{
						status = "ERROR";
					}

					auto line = uart::protocol::makeBallLineV5Simple(
					    ctx.task.session_id, ctx.task.seq++, capture_ms,
					    age_ms, pos, conf, status);
					if(line.has_value())
					{
						ctx.uart.sendLine(*line);
						last_ball_send = now;
					}
				}

				// 6) 标定完成 → 自动发送 CONTESTSTART
				if(ctx.task_phase == TaskPhase::CALIBRATING
				   && ctx.vision_result.calibrated
				   && !contest_start_sent
				   && link_state == LinkState::ONLINE)
				{
					auto line = uart::protocol::makeContestStartLine(
					    ctx.task.active_mode.empty()
					        ? "H5"
					        : ctx.task.active_mode);
					ctx.uart.sendLine(line);
					contest_start_sent = true;
					contest_start_since = now;
					ctx.task.active_mode = "H5_LAP_CENTER";

					ETEST_LOG_INFO(
					    "SEARCH", "auto-sent CONTESTSTART after calib");
				}

				// 7) CONTESTSTART 未确认期间检测球移动
				if(contest_start_sent
				   && ctx.task_phase == TaskPhase::CALIBRATING
				   && ctx.vision_result.calibrated)
				{
					if(ctx.vision_result.position_0p1mm != 0
					   && std::abs(ctx.vision_result.position_0p1mm)
					       > 200)
					{
						ETEST_LOG_WARN(
						    "SEARCH",
						    "ball moved during CONTESTSTART wait; "
						    "recalibrating");
						ctx.vision.resetYoloSession();
					}
				}

				// 8) CONTESTSTART 重发
				if(contest_start_sent && !ctx.task.contest_start_acked
				   && ctx.task_phase != TaskPhase::CONTEST)
				{
					const auto elapsed = std::chrono::duration_cast<
					                         std::chrono::milliseconds>(
					                         now - contest_start_since)
					                         .count();
					if(elapsed > 1000)
					{
						auto line =
						    uart::protocol::makeContestStartLine(
						        ctx.task.active_mode.empty()
						            ? "H5"
						            : ctx.task.active_mode);
						ctx.uart.sendLine(line);
						contest_start_since = now;
					}
				}
			}

			// 9) DONE → CONTESTSTOP
			if(done_received && ctx.task_phase == TaskPhase::CONTEST)
			{
				ctx.uart.sendLine(
				    uart::protocol::makeContestStopLine());
				ctx.task_phase = TaskPhase::CALIBRATING;
				ctx.task.contest_start_sent = false;
				ctx.task.contest_start_acked = false;
				contest_start_sent = false;
				done_received = false;
				ctx.vision.resetYoloSession();
				ctx.task.active_mode.clear();

				ETEST_LOG_INFO("SEARCH",
				               "CONTESTSTOP sent; recalibrating");
			}

			// 10) 性能统计日志
			{
				const auto now = std::chrono::steady_clock::now();
				const auto loop_us =
				    std::chrono::duration_cast<
				        std::chrono::microseconds>(now - loop_start)
				        .count();
				perf_loop_total_us += loop_us;

				const auto perf_elapsed = std::chrono::duration_cast<
				    std::chrono::milliseconds>(now - last_perf_log);
				if(perf_elapsed >= perf_interval)
				{
					const auto avg_vision_ms = perf_vision_calls > 0
					    ? (static_cast<double>(perf_vision_total_us)
					       / perf_vision_calls / 1000.0)
					    : 0.0;
					const auto avg_loop_us = perf_vision_calls > 0
					    ? (perf_loop_total_us
					       / std::max<std::int64_t>(1,
					                                perf_vision_calls))
					    : std::int64_t{0};

					const auto now_captured = use_latest_capture
					    ? capture.capturedFrames()
					    : frame_count;

					ETEST_LOG_INFO(
					    "PERF",
					    "vision_ms="
					        + std::to_string(
					            static_cast<int>(avg_vision_ms))
					        + " loop_us="
					        + std::to_string(
					            static_cast<int>(avg_loop_us))
					        + " frames=" + std::to_string(frame_count)
					        + " captured="
					        + std::to_string(now_captured) + " dropped="
					        + std::to_string(dropped_frames_total)
					        + " read_fail="
					        + std::to_string(
					            use_latest_capture
					                ? capture.readFailures()
					                : 0));

					last_perf_log = now;
					last_perf_captured = now_captured;
					last_perf_processed = last_processed_sequence;
					perf_vision_calls = 0;
					perf_vision_total_us = 0;
					perf_loop_total_us = 0;
				}
			}

			// 11) 节流日志
			{
				const auto now = std::chrono::steady_clock::now();
				const auto elapsed = std::chrono::duration_cast<
				    std::chrono::milliseconds>(now
				                               - last_throttle_time);
				if(elapsed.count() >= throttle_ms)
				{
					std::string msg = "searching... frame="
					    + std::to_string(frame_count);

					if(ctx.vision_result.target_type == "BALL")
					{
						if(ctx.vision_result.valid
						   && ctx.vision_result.calibrated)
						{
							msg += " | ball: "
							    + std::to_string(
							           ctx.vision_result.position_0p1mm)
							    + " (0.1mm) conf="
							    + std::to_string(
							           static_cast<int>(std::lround(
							               ctx.vision_result.confidence
							               * 100.0)))
							    + "%";
						}
						else if(!ctx.vision_result.error_code.empty())
						{
							msg += " | ball: "
							    + ctx.vision_result.error_code;
						}
						else
						{
							msg += " | ball: LOST";
						}
					}
					msg += " | phase=";
					switch(ctx.task_phase)
					{
					case TaskPhase::CALIBRATING:
						msg += "CALIBRATING";
						break;
					case TaskPhase::RUNNING:
						msg += "RUNNING";
						break;
					case TaskPhase::CONTEST:
						msg += "CONTEST";
						break;
					case TaskPhase::STOPPING:
						msg += "STOPPING";
						break;
					}
					msg += " vsession="
					    + std::string(
					           ctx.task.vsession_confirmed ? "1" : "0");
					ETEST_LOG_INFO("SEARCH", msg);
					last_throttle_time = now;
				}
			}

			// 12) 预览绘制
			if(can_show_preview && has_new_frame)
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
			else if(can_show_preview)
			{
				// 无新帧时仍处理窗口事件
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

		if(can_show_preview && preview_open)
			cv::destroyAllWindows();

		// LatestFrameCapture 在此析构，自动 stop + join
		ETEST_LOG_INFO("SEARCH", "exiting search loop");
		return State::END;
	}

} // namespace etest::state