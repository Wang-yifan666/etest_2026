#include "core/context.hpp"

namespace etest
{

RecoveryAction faultActionForConsecutiveCount(int count)
{
	if(count >= 10)
	{
		return RecoveryAction::RESTART_PROCESS;
	}

	return RecoveryAction::CONTINUE;
}

int exitCodeForReason(ExitReason reason)
{
	switch(reason)
	{
	case ExitReason::NORMAL:
		return 0;

	case ExitReason::SAFE_STOP:
		return 0;

	case ExitReason::RESTART_REQUIRED:
		return 1;
	}

	return 1;
}

} // namespace etest