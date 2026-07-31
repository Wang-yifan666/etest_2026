#include "uart/protocol.hpp"
#include "uart/uart.hpp"

#include <cassert>
#include <iostream>
#include <string>

namespace
{

	int failures = 0;

	void check(const std::string& label, bool condition)
	{
		if(!condition) { std::cerr << "[FAIL] " << label << "\n"; ++failures; }
		else std::cout << "[PASS] " << label << "\n";
	}

	void test_mission_codes()
	{
		// M0001~M0005 → isMissionCode=true, parse 1~5
		for(char c = '1'; c <= '5'; ++c)
		{
			std::string tag = "M000";
			tag += c;
			etest::UartMessage msg;
			msg.tag = tag;
			msg.type = etest::UartMessageType::UNKNOWN;
			check("isMission " + tag, etest::uart::protocol::isMissionCode(msg));
			int code = etest::uart::protocol::parseMissionCode(msg);
			check("parseMission " + tag, code == (c - '0'));
		}
		// M0006 → false
		etest::UartMessage msg;
		msg.tag = "M0006";
		check("isMission M0006=false", !etest::uart::protocol::isMissionCode(msg));
	}

	void test_mode_ack()
	{
		auto line = etest::uart::protocol::makeModeAckLine("M0001");
		check("ACK,M0001", line == "ACK,M0001");

		line = etest::uart::protocol::makeModeAckLine("M0005");
		check("ACK,M0005", line == "ACK,M0005");
	}

	void test_start_line()
	{
		auto line = etest::uart::protocol::makeStartLine("M0002", 0);
		check("START,M0002,0", line == "START,M0002,0");

		line = etest::uart::protocol::makeStartLine("M0005", -372);
		check("START,M0005,-372", line == "START,M0005,-372");
	}

	void test_calib_fail()
	{
		auto line = etest::uart::protocol::makeCalibrationFailLine("M0002", "NOT_AT_CENTER");
		check("CALIB_FAIL,M0002,NOT_AT_CENTER", line == "CALIB_FAIL,M0002,NOT_AT_CENTER");

		line = etest::uart::protocol::makeCalibrationFailLine("M0005", "BALL_LOST");
		check("CALIB_FAIL,M0005,BALL_LOST", line == "CALIB_FAIL,M0005,BALL_LOST");
	}

} // namespace

int main()
{
	test_mission_codes();
	test_mode_ack();
	test_start_line();
	test_calib_fail();

	if(failures > 0) { std::cerr << "\n" << failures << " FAILED\n"; return 1; }
	std::cout << "\nAll UART mode tests passed.\n";
	return 0;
}