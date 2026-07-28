#include "state/error.hpp"

#include "core/logger.hpp"

#include <chrono>
#include <string>
#include <thread>

namespace etest::state
{

	namespace
	{

		const char* faultSourceName(etest::FaultSource source)
		{
			switch(source)
			{
			case etest::FaultSource::NONE:
				return "NONE";

			case etest::FaultSource::CAMERA:
				return "CAMERA";

			case etest::FaultSource::UART:
				return "UART";

			case etest::FaultSource::VISION:
				return "VISION";

			case etest::FaultSource::CONFIG:
				return "CONFIG";

			case etest::FaultSource::INTERNAL:
				return "INTERNAL";
			}

			return "UNKNOWN";
		}

		const char* recoveryActionName(etest::RecoveryAction action)
		{
			switch(action)
			{
			case etest::RecoveryAction::CONTINUE:
				return "CONTINUE";

			case etest::RecoveryAction::RETRY:
				return "RETRY";

			case etest::RecoveryAction::REOPEN_CAMERA:
				return "REOPEN_CAMERA";

			case etest::RecoveryAction::RECONNECT_UART:
				return "RECONNECT_UART";

			case etest::RecoveryAction::SAFE_STOP:
				return "SAFE_STOP";

			case etest::RecoveryAction::RESTART_PROCESS:
				return "RESTART_PROCESS";
			}

			return "UNKNOWN";
		}

	} // namespace

	State runError(AppContext& ctx)
	{
		ctx.error_state_entry_count++;

		const auto& fault = ctx.last_fault;

		ETEST_LOG_ERROR(
		    "STATE_ERROR",
		    "entered ERROR state (count="
		        + std::to_string(ctx.error_state_entry_count)
		        + "): source=" + faultSourceName(fault.source)
		        + ", action=" + recoveryActionName(fault.action)
		        + ", code=" + fault.code
		        + ", message=" + fault.message);

		// 连续 ERROR 进入超过 5 次 → SAFE_STOP
		if(ctx.error_state_entry_count >= 5)
		{
			ETEST_LOG_FATAL(
			    "STATE_ERROR",
			    "too many ERROR state entries ("
			        + std::to_string(ctx.error_state_entry_count)
			        + "); performing safe stop");

			ctx.exit_reason = ExitReason::RESTART_REQUIRED;
			ctx.running = false;
			return State::END;
		}

		// 根据恢复动作执行
		switch(fault.action)
		{
		case RecoveryAction::CONTINUE:
			ETEST_LOG_INFO("STATE_ERROR",
			               "recovery: continue to SEARCH");
			return State::SEARCH;

		case RecoveryAction::RETRY:
			ETEST_LOG_INFO("STATE_ERROR",
			               "recovery: retry after delay");
			std::this_thread::sleep_for(std::chrono::milliseconds(200));
			return State::SEARCH;

		case RecoveryAction::REOPEN_CAMERA: {
			ETEST_LOG_INFO("STATE_ERROR", "recovery: reopening camera");

			ctx.camera.release();

			if(ctx.camera.open())
			{
				ETEST_LOG_INFO("STATE_ERROR",
				               "camera reopened successfully");
				ctx.last_fault = {};
				ctx.error_state_entry_count = 0;
				return State::START;
			}

			ETEST_LOG_ERROR("STATE_ERROR",
			                "camera reopen failed; will retry");

			ctx.last_fault = {
			    FaultSource::CAMERA, RecoveryAction::REOPEN_CAMERA,
			    "CAM_REOPEN_FAIL", "camera reopen failed"};
			return State::ERROR;
		}

		case RecoveryAction::RECONNECT_UART: {
			ETEST_LOG_INFO("STATE_ERROR",
			               "recovery: reconnecting UART");

			if(!ctx.uart.isOpen())
			{
				ctx.uart.open();
			}

			if(!ctx.uart.isRunning())
			{
				ctx.uart.start();
			}

			if(ctx.uart.isOpen() && ctx.uart.isRunning())
			{
				ETEST_LOG_INFO("STATE_ERROR",
				               "UART reconnected successfully");
				ctx.last_fault = {};
				ctx.error_state_entry_count = 0;
				return State::SEARCH;
			}

			ETEST_LOG_ERROR("STATE_ERROR",
			                "UART reconnect failed; will retry");

			ctx.last_fault = {
			    FaultSource::UART, RecoveryAction::RECONNECT_UART,
			    "UART_RECONNECT_FAIL", "UART reconnect failed"};
			return State::ERROR;
		}

		case RecoveryAction::SAFE_STOP:
			ETEST_LOG_INFO("STATE_ERROR",
			               "recovery: safe stop requested");
			ctx.running = false;
			return State::END;

		case RecoveryAction::RESTART_PROCESS:
			ETEST_LOG_FATAL("STATE_ERROR",
			                "recovery: restart process requested "
			                "(systemd will restart)");
			ctx.exit_reason = ExitReason::RESTART_REQUIRED;
			ctx.running = false;
			return State::END;
		}

		// 默认：回到 SEARCH
		return State::SEARCH;
	}

} // namespace etest::state