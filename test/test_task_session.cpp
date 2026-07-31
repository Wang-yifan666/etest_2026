#include "state/task_session.hpp"

#include <iostream>
#include <string>

namespace
{
	int failures = 0;
	void check(const std::string& label, bool condition)
	{
		if(!condition)
		{
			std::cerr << "[FAIL] " << label << "\n";
			++failures;
		}
		else
			std::cout << "[PASS] " << label << "\n";
	}

	void test_mode_mapping()
	{
		check("M0001→Q2",
		      etest::TaskSession::modeFromTag("M0001")
		          == etest::TaskMode::Q2);
		check("M0002→Q3",
		      etest::TaskSession::modeFromTag("M0002")
		          == etest::TaskMode::Q3);
		check("M0003→Q4",
		      etest::TaskSession::modeFromTag("M0003")
		          == etest::TaskMode::Q4);
		check("M0004→Q5",
		      etest::TaskSession::modeFromTag("M0004")
		          == etest::TaskMode::Q5);
		check("M0005→Q6",
		      etest::TaskSession::modeFromTag("M0005")
		          == etest::TaskMode::Q6);
		check("unknown→NONE",
		      etest::TaskSession::modeFromTag("M0006")
		          == etest::TaskMode::NONE);
	}

	void test_reset_increments_session_id()
	{
		etest::TaskSession s;
		auto id1 = s.session_id;
		s.reset();
		check("session_id incremented", s.session_id == id1 + 1);
		check("mode=NONE after reset", s.mode == etest::TaskMode::NONE);
		check("phase=IDLE after reset",
		      s.phase == etest::ContestTaskPhase::IDLE);
		check("vision_enabled=false", !s.vision_enabled);
		check("measurement_valid=false", !s.measurement_valid);
	}

	void test_enter_phase()
	{
		etest::TaskSession s;
		s.enterPhase(etest::ContestTaskPhase::CALIBRATING);
		check("phase=CALIBRATING",
		      s.phase == etest::ContestTaskPhase::CALIBRATING);
		check("calib_frames=0", s.calibration_valid_frames == 0);
	}
} // namespace

int main()
{
	test_mode_mapping();
	test_reset_increments_session_id();
	test_enter_phase();
	if(failures > 0)
	{
		std::cerr << "\n" << failures << " FAILED\n";
		return 1;
	}
	std::cout << "\nAll task session tests passed.\n";
	return 0;
}
