#pragma once

#include <chrono>
#include <string>

namespace etest
{

	enum class State
	{
		START,
		SEARCH,
		ERROR,
		END
	};

	// 故障分类

	enum class FaultSource
	{
		NONE,
		CAMERA,
		UART,
		VISION,
		CONFIG,
		INTERNAL
	};

	enum class RecoveryAction
	{
		CONTINUE,
		RETRY,
		REOPEN_CAMERA,
		RECONNECT_UART,
		SAFE_STOP,
		RESTART_PROCESS
	};

	struct FaultInfo
	{
		FaultSource source = FaultSource::NONE;
		RecoveryAction action = RecoveryAction::CONTINUE;
		std::string code;
		std::string message;
		int occurrence_count = 0;
		std::chrono::steady_clock::time_point timestamp;
	};

} // namespace etest