#include "state/search.hpp"

#include "core/config.hpp"
#include "core/logger.hpp"
#include "state/task_session.hpp"
#include "uart/protocol.hpp"
#include "uart/uart.hpp"
#include "vision/ball_ncnn_manager.hpp"
#include "vision/latest_frame_capture.hpp"
#include "vision/roi_utils.hpp"
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

		// 辅助：当前任务是否活跃（收到 M000x 且未结束）
		bool isMissionActive(const TaskSession& task)
		{
			return task.phase == ContestTaskPhase::PREPARING
			    || task.phase == ContestTaskPhase::CALIBRATING
			    || task.phase == ContestTaskPhase::RUNNING;
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
		std::uint64_t loop_calls = 0;
		std::uint64_t received_frames = 0;
		std::uint64_t processed_frames = 0;
		int perf_vision_calls = 0;
		std::int64_t perf_vision_total_us = 0;
		std::int64_t perf_loop_total_us = 0;
		std::uint64_t dropped_frames_total = 0;

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
		std::string last_hb_error;
		auto last_hb_error_time = std::chrono::steady_clock::now()
		    - std::chrono::milliseconds(5000);

		int frame_failures = 0;

		// BALL 发送限频（25 Hz）
		auto last_ball_send = std::chrono::steady_clock::now();
		constexpr auto kBallSendPeriod = std::chrono::milliseconds(40);

		// M0001 准备阶段：等待 1 个新帧
		bool m0001_frame_received = false;

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

			++loop_calls;
			const auto loop_start = std::chrono::steady_clock::now();

			// ── 1) 优先清空 UART 接收队列 ──
			{
				UartMessage msg;
				int processed = 0;
				constexpr int max_per_loop = 16;
				while(processed < max_per_loop && ctx.uart.tryPop(msg))
				{
					++processed;

					// M000X → 记录题目
					if(uart::protocol::isMissionCode(msg))
					{
						const std::string& new_tag = msg.tag;
						TaskMode new_mode =
						    TaskSession::modeFromTag(new_tag);

						// 重复同一指令处理（不发送额外报文，MCU 已知道当前模式）
						if(isMissionActive(ctx.task)
						   && ctx.task.command_tag == new_tag)
						{
							continue;
						}

						// 另一指令 → 终止旧 session
						ctx.task.reset();
						ctx.task.mode = new_mode;
						ctx.task.command_tag = new_tag;
						ctx.task.seq = 0;

						ETEST_LOG_INFO(
						    "SEARCH",
						    "received mission code: "
						        + new_tag
						        + " session_id="
						        + std::to_string(
						            ctx.task.session_id));

						// M0001: PREPARING
						if(new_tag == "M0001")
						{
							ctx.task.enterPhase(
							    ContestTaskPhase::PREPARING);
							ctx.task.vision_enabled = false;
							ctx.task.tracking_mode =
							    TrackingMode::NONE;
							m0001_frame_received = false;

							// M0001 也发送 VSESSION 声明
							ctx.uart.sendLine(
							    uart::protocol::makeVsessionLine(
							        ctx.task.session_id,
							        search_cfg.nominal_fps * 100,
							        search_cfg.camera_id));
							ctx.task.vsession_confirmed = false;
						}
						else
						{
							// M0002～M0005: CALIBRATING
							ctx.task.enterPhase(
							    ContestTaskPhase::CALIBRATING);
							ctx.task.vision_enabled = true;
							ctx.task.tracking_mode =
							    TrackingMode::FULL;

							// 发送 VSESSION 声明视觉会话
							ctx.uart.sendLine(
							    uart::protocol::makeVsessionLine(
							        ctx.task.session_id,
							        search_cfg.nominal_fps * 100,
							        search_cfg.camera_id));
							ctx.task.vsession_confirmed = false;

							ETEST_LOG_INFO(
							    "SEARCH",
							    "VSESSION sent: session="
							        + std::to_string(
							            ctx.task.session_id)
							        + " fps_x100="
							        + std::to_string(
							            search_cfg.nominal_fps * 100)
							        + " camera="
							        + search_cfg.camera_id);
						}

						ctx.vision.resetYoloSession();
						continue;
					}

					// BOOT,OK → 清除任务，重新握手
					if(uart::protocol::isBootOk(msg))
					{
						ETEST_LOG_WARN(
						    "SEARCH",
						    "lower machine reboot: " + msg.raw);
						ctx.lower_machine_online = false;
						ctx.task.reset();
						link_state = LinkState::WAIT_PING;
						heartbeat_offline = true;
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

					// CAPS 响应
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
						ctx.task.session_start_time =
						    std::chrono::steady_clock::now();
						ETEST_LOG_INFO("SEARCH",
						               "VSESSION confirmed: session="
						                   + std::to_string(
						                       ctx.task.session_id));
						continue;
					}

					// DONE（需匹配当前 mode）
					if(auto done = uart::protocol::parseDone(msg))
					{
						if(done->mode != ctx.task.command_tag)
						{
							ETEST_LOG_WARN(
							    "SEARCH",
							    "ignoring DONE mode mismatch: got "
							        + done->mode + " expected "
							        + ctx.task.command_tag);
							continue;
						}
						ETEST_LOG_INFO("SEARCH",
						               "DONE received: " + done->mode
						                   + " result=" + done->result);
						ctx.task.enterPhase(
						    ContestTaskPhase::FINISHED);
						ctx.task.vision_enabled = false;
						ctx.task.tracking_mode = TrackingMode::NONE;
						ctx.task.measurement_valid = false;
						ctx.vision.resetYoloSession();
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

			// ── 2) 心跳发送与超时 ──
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
						const std::string err_msg = "heartbeat timeout";
						if(!shouldThrottle(err_msg, last_hb_error,
						                   last_hb_error_time))
							ETEST_LOG_WARN("SEARCH", err_msg);
					}
				}
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

			// ── 3) 获取帧 ──
			vision::FramePacket packet;
			bool has_new_frame = false;

			if(use_latest_capture)
			{
				has_new_frame = capture.tryGetLatest(
				    packet, last_processed_sequence);

				if(!has_new_frame)
				{
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
					std::this_thread::sleep_for(
					    std::chrono::milliseconds(1));
					continue;
				}
			}
			else
			{
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

				packet.frame = ctx.frame;
				packet.received_at = std::chrono::steady_clock::now();
				packet.sequence = received_frames;
				has_new_frame = true;
			}

			if(has_new_frame)
			{
				++received_frames;
				ctx.frame = packet.frame;

				// 录像：完整原始画面异步写入
				ctx.recorder.writeRaw(ctx.frame);

				if(use_latest_capture && last_processed_sequence != 0
				   && packet.sequence > last_processed_sequence + 1)
				{
					dropped_frames_total +=
					    packet.sequence - last_processed_sequence - 1;
				}
				last_processed_sequence = packet.sequence;
			}

			// ── 4) 推进任务阶段 ──
			if(has_new_frame)
			{
				// M0001: VSESSION 确认后发送 CONTESTSTART
				if(ctx.task.phase == ContestTaskPhase::PREPARING
				   && ctx.task.vsession_confirmed)
				{
					m0001_frame_received = true;
					const char* mode_name =
					    TaskSession::modeName(ctx.task.mode);
					ctx.uart.sendLine(
					    uart::protocol::makeContestStartLine(
					        mode_name));
					ctx.task.start_sent = true;
					ctx.task.enterPhase(
					    ContestTaskPhase::RUNNING);
					ctx.task.vision_enabled = false;
					ctx.task.tracking_mode = TrackingMode::NONE;
					ETEST_LOG_INFO(
					    "SEARCH",
					    "M0001: PREPARING → RUNNING, "
					    "CONTESTSTART sent: "
					        + std::string(mode_name));
				}
			}

			// ── 5) 按需执行视觉推理（BallNcnn 双模型）──
			if(isMissionActive(ctx.task) && ctx.task.vision_enabled
			   && has_new_frame)
			{
				const auto vision_start =
				    std::chrono::steady_clock::now();

				// 标定阶段强制 FULL，RUNNING 阶段按 tracking_mode
				TrackingMode effective_mode =
				    ctx.task.tracking_mode;
				if(ctx.task.phase == ContestTaskPhase::CALIBRATING)
				{
					effective_mode = TrackingMode::FULL;
				}

				auto measurement =
				    ctx.vision.processBallNcnn(ctx.frame,
				                               effective_mode);
				const auto vision_end =
				    std::chrono::steady_clock::now();
				const auto vision_us = std::chrono::duration_cast<
				                           std::chrono::microseconds>(
				                           vision_end - vision_start)
				                           .count();
				++perf_vision_calls;
				perf_vision_total_us += vision_us;
				++processed_frames;

				// 同步测量结果到 task session
				ctx.task.last_confidence =
				    static_cast<double>(measurement.confidence);
				if(measurement.valid
				   && measurement.status == "OK")
				{
					ctx.task.current_position_mm =
					    measurement.position_0p1mm / 10.0;
					ctx.task.last_global_center =
					    measurement.global_center;
					ctx.task.measurement_valid = true;
					ctx.task.lost_frames = 0;
				}
				else
				{
					ctx.task.measurement_valid = false;
					if(!measurement.valid
					   || measurement.status == "LOST")
					{
						ctx.task.lost_frames++;
					}
				}

				// ── CENTER / FULL_REACQUIRE 切换（仅 Q4/Q5 RUNNING）──
				if(ctx.task.phase
				       == ContestTaskPhase::RUNNING
				   && (ctx.task.mode == TaskMode::Q4
				       || ctx.task.mode == TaskMode::Q5))
				{
					const auto& bn_cfg =
					    ctx.vision.getConfig().ball_ncnn;
					if(ctx.task.tracking_mode
					   == TrackingMode::CENTER)
					{
						bool near_edge =
						    !measurement.valid
						    || measurement.local_center.x
						        < static_cast<float>(
						            bn_cfg.edge_guard_px)
						    || measurement.local_center.x
						        > static_cast<float>(
						            bn_cfg.center_input_width
						            - bn_cfg.edge_guard_px);
						if(near_edge
						   || ctx.task.lost_frames
						       >= bn_cfg.lost_frames_to_reacquire)
						{
							ctx.task.tracking_mode =
							    TrackingMode::FULL_REACQUIRE;
							ctx.task.center_stable_frames = 0;
							ctx.vision
							    .resetBallNcnnSession();
							ETEST_LOG_INFO(
							    "SEARCH",
							    "CENTER → FULL_REACQUIRE");
						}
					}
					else if(ctx.task.tracking_mode
					        == TrackingMode::FULL_REACQUIRE)
					{
						if(measurement.valid
						   && measurement.status == "OK"
						   && measurement.local_center.x
						       >= static_cast<float>(
						           bn_cfg.edge_guard_px
						           + 10)
						   && measurement.local_center.x
						       <= static_cast<float>(
						           bn_cfg.center_input_width
						           - bn_cfg.edge_guard_px
						           - 10))
						{
							ctx.task.center_stable_frames++;
							if(ctx.task.center_stable_frames
							   >= bn_cfg
							          .stable_frames_to_center)
							{
								ctx.task.tracking_mode =
								    TrackingMode::CENTER;
								ctx.vision
								    .resetBallNcnnSession();
								ETEST_LOG_INFO(
								    "SEARCH",
								    "FULL_REACQUIRE → CENTER");
							}
						}
						else
						{
							ctx.task.center_stable_frames = 0;
						}
					}
				}

				// 填充 ctx.vision_result 以兼容旧的度量
				ctx.vision_result.valid = measurement.valid;
				ctx.vision_result.confidence =
				    static_cast<double>(measurement.confidence);
				ctx.vision_result.position_0p1mm =
				    measurement.position_0p1mm;
				ctx.vision_result.x =
				    static_cast<double>(
				        measurement.global_center.x);
				ctx.vision_result.y =
				    static_cast<double>(
				        measurement.global_center.y);
			}

			// ── 6) 限频发送任务输出 ──
			if(isMissionActive(ctx.task)
			   && ctx.task.vision_enabled
			   && ctx.task.vsession_confirmed)
			{
				const auto now = std::chrono::steady_clock::now();
				const auto ball_elapsed =
				    std::chrono::duration_cast<
				        std::chrono::milliseconds>(now - last_ball_send)
				        .count();

				if(ball_elapsed >= kBallSendPeriod.count()
				   || ctx.task.phase
				          == ContestTaskPhase::CALIBRATING)
				{
					std::string status;
					int pos = 0;
					float conf = 0.0F;

					if(ctx.task.phase
					       == ContestTaskPhase::CALIBRATING)
					{
						status = "CALIB";
					}
					else if(ctx.vision_result.calibrated
					        && ctx.vision_result.valid)
					{
						status = "OK";
						pos = ctx.vision_result.position_0p1mm;
						conf = static_cast<float>(
						    ctx.vision_result.confidence);
					}
					else if(!ctx.vision_result.valid)
					{
						status = "LOST";
						ctx.task.measurement_valid = false;
					}
					else
					{
						status = "ERROR";
						ctx.task.measurement_valid = false;
					}

					if(status == "OK")
					{
						ctx.task.measurement_valid = true;
						ctx.task.current_position_mm =
						    pos / 10.0;
						ctx.task.last_confidence = conf;
					}

					// 计算 capture_ms / age_ms
					std::uint32_t capture_ms = 0;
					std::uint32_t age_ms = 0;
					if(has_new_frame
					   && ctx.task.session_start_time
					          .time_since_epoch()
					          .count()
					      > 0)
					{
						capture_ms = static_cast<std::uint32_t>(
						    std::chrono::duration_cast<
						        std::chrono::milliseconds>(
						        packet.received_at
						        - ctx.task.session_start_time)
						        .count());
						age_ms = static_cast<std::uint32_t>(
						    std::chrono::duration_cast<
						        std::chrono::milliseconds>(
						        now - packet.received_at)
						        .count());
					}

					auto line = uart::protocol::makeBallLineV5Simple(
					    ctx.task.session_id, ctx.task.seq++,
					    capture_ms, age_ms, pos, conf, status);
					if(line.has_value())
					{
						ctx.uart.sendLine(*line);
						last_ball_send = now;
					}
				}

				// 标定中积累有效帧
				if(ctx.task.phase
				       == ContestTaskPhase::CALIBRATING
				   && ctx.task.measurement_valid
				   && has_new_frame)
				{
					const auto& bn_cfg =
					    ctx.vision.getConfig().ball_ncnn;
					int limit_0p1mm = static_cast<int>(
					    bn_cfg.initial_center_limit_mm * 10.0);
					int pos = ctx.vision_result.position_0p1mm;

					if(std::abs(pos) <= limit_0p1mm)
					{
						ctx.task.calibration_valid_frames++;
					}
					else
					{
						ctx.task.calibration_valid_frames = 0;
					}
				}

				// 标定完成 → 发送 START（M0002～M0005）
				if(ctx.task.phase
				       == ContestTaskPhase::CALIBRATING
				   && !ctx.task.start_sent
				   && ctx.task.vsession_confirmed
				   && ctx.task.calibration_valid_frames
				       >= ctx.vision.getConfig()
				              .ball_ncnn.calibration_frames)
				{
					int target_0p1mm = 0;
					if(ctx.task.mode == TaskMode::Q6)
					{
						// M0005: 锁定当前绝对位置
						target_0p1mm =
						    ctx.vision_result.position_0p1mm;
						ctx.task.target_position_mm =
						    target_0p1mm / 10.0;
					}

					const char* mode_name =
					    TaskSession::modeName(ctx.task.mode);
					if(ctx.task.mode == TaskMode::Q6)
					{
						// M0005 使用带 target 的 START
						auto start_line =
						    uart::protocol::makeStartLine(
						        ctx.task.command_tag,
						        target_0p1mm);
						ctx.uart.sendLine(start_line);
					}
					else
					{
						ctx.uart.sendLine(
						    uart::protocol::makeContestStartLine(
						        mode_name));
					}
					ctx.task.start_sent = true;
					ctx.task.enterPhase(
					    ContestTaskPhase::RUNNING);

					if(ctx.task.mode == TaskMode::Q4
					   || ctx.task.mode == TaskMode::Q5)
					{
						ctx.task.tracking_mode =
						    TrackingMode::CENTER;
					}

					ctx.vision_result.calibrated = true;

					ETEST_LOG_INFO(
					    "SEARCH",
					    "CALIBRATING → RUNNING, "
					    "START sent: "
					        + std::string(mode_name)
					        + " target="
					        + std::to_string(target_0p1mm));
				}

				// 标定超时
				if(ctx.task.phase
				       == ContestTaskPhase::CALIBRATING
				   && !ctx.task.start_sent)
				{
					const auto now =
					    std::chrono::steady_clock::now();
					const auto elapsed =
					    std::chrono::duration_cast<
					        std::chrono::milliseconds>(
					        now
					        - ctx.task.calibration_start_time);
					if(elapsed.count()
					   > ctx.vision.getConfig()
					          .ball_ncnn.calibration_timeout_ms)
					{
						ETEST_LOG_WARN(
						    "SEARCH",
						    "calibration timeout, resetting");
						ctx.task.calibration_valid_frames = 0;
						ctx.task.calibration_start_time = now;
					}
				}
			}

			// ── 7) 心跳、日志、预览 ──
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
					const auto avg_vision_ms =
					    processed_frames > 0
					    ? (static_cast<double>(perf_vision_total_us)
					       / processed_frames / 1000.0)
					    : 0.0;

					ETEST_LOG_INFO(
					    "PERF",
					    "vision_ms="
					        + std::to_string(
					            static_cast<int>(avg_vision_ms))
					        + " loops="
					        + std::to_string(loop_calls)
					        + " received="
					        + std::to_string(received_frames)
					        + " processed="
					        + std::to_string(processed_frames)
					        + " dropped="
					        + std::to_string(dropped_frames_total));

					last_perf_log = now;
					loop_calls = 0;
					received_frames = 0;
					processed_frames = 0;
					perf_vision_calls = 0;
					perf_vision_total_us = 0;
					perf_loop_total_us = 0;
				}
			}

			// 节流日志
			{
				const auto now = std::chrono::steady_clock::now();
				const auto elapsed = std::chrono::duration_cast<
				    std::chrono::milliseconds>(now
				                               - last_throttle_time);
				if(elapsed.count() >= throttle_ms)
				{
					std::string msg = "searching... frame="
					    + std::to_string(received_frames);

					if(ctx.task.vision_enabled)
					{
						if(ctx.vision_result.valid
						   && ctx.vision_result.calibrated)
						{
							msg += " | ball: "
							    + std::to_string(
							           ctx.vision_result.position_0p1mm)
							    + " (0.1mm)";
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

					msg += " | mode=" + ctx.task.command_tag;
					msg += " | phase=";
					switch(ctx.task.phase)
					{
					case ContestTaskPhase::IDLE:
						msg += "IDLE";
						break;
					case ContestTaskPhase::PREPARING:
						msg += "PREPARING";
						break;
					case ContestTaskPhase::CALIBRATING:
						msg += "CALIBRATING";
						break;
					case ContestTaskPhase::RUNNING:
						msg += "RUNNING";
						break;
					case ContestTaskPhase::FINISHED:
						msg += "FINISHED";
						break;
					case ContestTaskPhase::FAULT:
						msg += "FAULT";
						break;
					}

					ETEST_LOG_INFO("SEARCH", msg);
					last_throttle_time = now;
				}
			}

			// 预览绘制
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

		ETEST_LOG_INFO("SEARCH", "exiting search loop");
		return State::END;
	}

} // namespace etest::state