/**
 * @file test/test_protocol.cpp
 * @brief V5.2 协议辅助函数单元测试
 *
 * 覆盖：makeTargetLine、makeLostLine、makeBallLine、isBootOk、
 * isPingResponse、getProtocolVersion (legacy)、getProtocolVersionMajor/Minor、
 * isCapsResponse、isMissionCode、parseMissionCode、missionModeName、
 * makeContestStartLine、makeContestStatusQueryLine。
 */

#include "uart/protocol.hpp"
#include "uart/uart.hpp"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>

using namespace etest::uart::protocol;
using namespace etest;

static int passed = 0;
static int failed = 0;

static void check(const std::string& name, bool condition)
{
	if(condition)
	{
		++passed;
		std::cout << "  PASS: " << name << '\n';
	}
	else
	{
		++failed;
		std::cerr << "  FAIL: " << name << '\n';
	}
}


// makeTargetLine


static void test_makeTargetLine_correct()
{
	auto result = makeTargetLine(7, 12.345, 98.10, -2.50, 0.876);
	check("makeTargetLine correct output", result.has_value());
	check("makeTargetLine format TARGET,7,12.35,98.10,-2.50,0.876",
	      result.value() == "TARGET,7,12.35,98.10,-2.50,0.876");
}

static void test_makeTargetLine_seq_wrap()
{
	auto result = makeTargetLine(0xFFFFFFFF, 1.0, 2.0, 3.0, 0.5);
	check("makeTargetLine seq wrap ok", result.has_value());
}

static void test_makeTargetLine_nan_rejected()
{
	auto result = makeTargetLine(1, std::nan(""), 2.0, 3.0, 0.5);
	check("makeTargetLine NaN x rejected", !result.has_value());

	auto result2 = makeTargetLine(1, 2.0, std::nan(""), 3.0, 0.5);
	check("makeTargetLine NaN y rejected", !result2.has_value());

	auto result3 = makeTargetLine(1, 2.0, 3.0, std::nan(""), 0.5);
	check("makeTargetLine NaN angle rejected", !result3.has_value());

	auto result4 = makeTargetLine(1, 2.0, 3.0, 4.0, std::nan(""));
	check("makeTargetLine NaN confidence rejected",
	      !result4.has_value());
}

static void test_makeTargetLine_inf_rejected()
{
	auto inf = std::numeric_limits<double>::infinity();
	auto result = makeTargetLine(1, inf, 2.0, 3.0, 0.5);
	check("makeTargetLine Inf rejected", !result.has_value());

	auto neg_inf = -std::numeric_limits<double>::infinity();
	auto result2 = makeTargetLine(1, 2.0, neg_inf, 3.0, 0.5);
	check("makeTargetLine -Inf rejected", !result2.has_value());
}

static void test_makeTargetLine_confidence_lt0()
{
	auto result = makeTargetLine(1, 2.0, 3.0, 4.0, -0.1);
	check("makeTargetLine confidence<0 rejected", !result.has_value());
}

static void test_makeTargetLine_confidence_gt1()
{
	auto result = makeTargetLine(1, 2.0, 3.0, 4.0, 1.1);
	check("makeTargetLine confidence>1 rejected", !result.has_value());
}

static void test_makeTargetLine_zero_confidence()
{
	auto result = makeTargetLine(1, 0.0, 0.0, 0.0, 0.0);
	check("makeTargetLine confidence=0 ok", result.has_value());
}

static void test_makeTargetLine_one_confidence()
{
	auto result = makeTargetLine(1, 0.0, 0.0, 0.0, 1.0);
	check("makeTargetLine confidence=1 ok", result.has_value());
}


// makeLostLine


static void test_makeLostLine_correct()
{
	std::string line = makeLostLine(8);
	check("makeLostLine format LOST,8", line == "LOST,8");
}

static void test_makeLostLine_large_seq()
{
	std::string line = makeLostLine(4294967295u);
	check("makeLostLine large seq", !line.empty());
}


// makeBallLine (V5 legacy: offset_mm + confidence_0_255)


static void test_makeBallLine_ok()
{
	auto result = makeBallLine(123, -18, 220, "OK");
	check("makeBallLine OK", result.has_value());
	check("makeBallLine BALL,123,-18,220,OK",
	      result.value() == "BALL,123,-18,220,OK");
}

static void test_makeBallLine_lost()
{
	auto result = makeBallLine(124, 0, 0, "LOST");
	check("makeBallLine LOST", result.has_value());
	check("makeBallLine BALL,124,0,0,LOST",
	      result.value() == "BALL,124,0,0,LOST");
}

static void test_makeBallLine_calib()
{
	auto result = makeBallLine(125, 0, 0, "CALIB");
	check("makeBallLine CALIB", result.has_value());
	check("makeBallLine BALL,125,0,0,CALIB",
	      result.value() == "BALL,125,0,0,CALIB");
}

static void test_makeBallLine_error()
{
	auto result = makeBallLine(126, 0, 0, "ERROR");
	check("makeBallLine ERROR", result.has_value());
	check("makeBallLine BALL,126,0,0,ERROR",
	      result.value() == "BALL,126,0,0,ERROR");
}

static void test_makeBallLine_conf_neg_rejected()
{
	auto result = makeBallLine(1, 0, -1, "OK");
	check("makeBallLine confidence=-1 rejected", !result.has_value());
}

static void test_makeBallLine_conf_256_rejected()
{
	auto result = makeBallLine(1, 0, 256, "OK");
	check("makeBallLine confidence=256 rejected", !result.has_value());
}

static void test_makeBallLine_unknown_status_rejected()
{
	auto result = makeBallLine(1, 0, 0, "UNKNOWN");
	check("makeBallLine unknown status rejected", !result.has_value());
}

static void test_makeBallLine_lost_nonzero_offset_rejected()
{
	auto result = makeBallLine(1, 5, 0, "LOST");
	check("makeBallLine LOST + offset=5 rejected", !result.has_value());
}

static void test_makeBallLine_calib_nonzero_conf_rejected()
{
	auto result = makeBallLine(1, 0, 100, "CALIB");
	check("makeBallLine CALIB + confidence=100 rejected",
	      !result.has_value());
}

static void test_makeBallLine_max_seq_accepted()
{
	auto result = makeBallLine(0xFFFFFFFF, -120, 200, "OK");
	check("makeBallLine max seq accepted", result.has_value());
}

static void test_makeBallLine_negative_offset_accepted()
{
	auto result = makeBallLine(1, -120, 200, "OK");
	check("makeBallLine negative offset accepted", result.has_value());
	check("makeBallLine BALL,1,-120,200,OK",
	      result.value() == "BALL,1,-120,200,OK");
}

static void test_makeBallLine_ok_zero_zero()
{
	auto result = makeBallLine(1, 0, 0, "OK");
	check("makeBallLine OK with offset=0 conf=0 accepted",
	      result.has_value());
}

static void test_makeBallLine_error_nonzero_rejected()
{
	auto result = makeBallLine(1, 3, 0, "ERROR");
	check("makeBallLine ERROR + offset=3 rejected",
	      !result.has_value());
}


// isBootOk


static void test_isBootOk_valid()
{
	auto msg = Uart::parseLine("BOOT,OK");
	check("isBootOk BOOT,OK -> true", isBootOk(msg));
}

static void test_isBootOk_boot_error()
{
	auto msg = Uart::parseLine("BOOT,ERROR");
	check("isBootOk BOOT,ERROR -> false", !isBootOk(msg));
}

static void test_isBootOk_extra_fields()
{
	auto msg = Uart::parseLine("BOOT,OK,EXTRA");
	check("isBootOk BOOT,OK,EXTRA -> false (fields.size!=1)",
	      !isBootOk(msg));
}

static void test_isBootOk_wrong_tag()
{
	auto msg = Uart::parseLine("OK,PING");
	check("isBootOk OK,PING -> false", !isBootOk(msg));
}

// isPingResponse

static void test_isPingResponse_valid()
{
	auto msg = Uart::parseLine("OK,PING");
	check("isPingResponse OK,PING -> true", isPingResponse(msg));
}

static void test_isPingResponse_extra()
{
	auto msg = Uart::parseLine("OK,PING,EXTRA");
	check("isPingResponse OK,PING,EXTRA -> false",
	      !isPingResponse(msg));
}

static void test_isPingResponse_ping_extra()
{
	auto msg = Uart::parseLine("OK,PING_EXTRA");
	check("isPingResponse OK,PING_EXTRA -> false",
	      !isPingResponse(msg));
}

static void test_isPingResponse_wrong_type()
{
	auto msg = Uart::parseLine("ERR,PING");
	check("isPingResponse ERR,PING -> false", !isPingResponse(msg));
}

// getProtocolVersion (legacy: PROTO,<single_version>)

static void test_getProtocolVersion_valid()
{
	auto msg = Uart::parseLine("PROTO,5");
	auto v = getProtocolVersion(msg);
	check("getProtocolVersion PROTO,5 -> 5", v.has_value() && *v == 5);
}

static void test_getProtocolVersion_v4_accepted()
{
	auto msg = Uart::parseLine("PROTO,4");
	auto v = getProtocolVersion(msg);
	check("getProtocolVersion PROTO,4 -> 4", v.has_value() && *v == 4);
}

static void test_getProtocolVersion_abc_rejected()
{
	auto msg = Uart::parseLine("PROTO,abc");
	auto v = getProtocolVersion(msg);
	check("getProtocolVersion PROTO,abc -> nullopt", !v.has_value());
}

static void test_getProtocolVersion_extra_rejected()
{
	auto msg = Uart::parseLine("PROTO,5,EXTRA");
	auto v = getProtocolVersion(msg);
	check("getProtocolVersion PROTO,5,EXTRA -> nullopt",
	      !v.has_value());
}

static void test_getProtocolVersion_empty()
{
	auto msg = Uart::parseLine("PROTO,");
	auto v = getProtocolVersion(msg);
	check("getProtocolVersion PROTO, -> nullopt", !v.has_value());
}

static void test_getProtocolVersion_proto_query()
{
	auto msg = Uart::parseLine("PROTO?");
	auto v = getProtocolVersion(msg);
	check("getProtocolVersion PROTO? -> nullopt", !v.has_value());
}


// V5.2 PROTO,<major>,<minor>


static void test_getProtoMajorMinor_valid()
{
	auto msg = Uart::parseLine("PROTO,5,2");
	auto major = getProtocolVersionMajor(msg);
	auto minor = getProtocolVersionMinor(msg);
	check("getProtocolVersionMajor PROTO,5,2 -> 5",
	      major.has_value() && *major == 5);
	check("getProtocolVersionMinor PROTO,5,2 -> 2",
	      minor.has_value() && *minor == 2);
}

static void test_getProtoMajorMinor_old_format_rejected()
{
	auto msg = Uart::parseLine("PROTO,5");
	auto major = getProtocolVersionMajor(msg);
	check("getProtocolVersionMajor PROTO,5 -> nullopt",
	      !major.has_value());
}

static void test_getProtoMajorMinor_abc_rejected()
{
	auto msg = Uart::parseLine("PROTO,abc,2");
	auto major = getProtocolVersionMajor(msg);
	check("getProtocolVersionMajor PROTO,abc,2 -> nullopt",
	      !major.has_value());
}


// CAPS response


static void test_isCapsResponse_valid()
{
	auto msg = Uart::parseLine("CAPS,MOTION=4,BALL=2");
	check("isCapsResponse CAPS,MOTION=4,BALL=2 -> true",
	      isCapsResponse(msg));
}

static void test_isCapsResponse_wrong()
{
	auto msg = Uart::parseLine("PROTO,5,2");
	check("isCapsResponse PROTO,5,2 -> false", !isCapsResponse(msg));
}


// M000X mission code


static void test_isMissionCode_valid()
{
	auto msg = Uart::parseLine("M0004");
	check("isMissionCode M0004 -> true", isMissionCode(msg));
}

static void test_isMissionCode_invalid()
{
	auto msg = Uart::parseLine("M0006");
	check("isMissionCode M0006 -> false", !isMissionCode(msg));
}

static void test_parseMissionCode()
{
	auto msg = Uart::parseLine("M0003");
	check("parseMissionCode M0003 -> 3", parseMissionCode(msg) == 3);
}

static void test_missionModeName()
{
	check("missionModeName 1 -> H2", missionModeName(1) == "H2");
	check("missionModeName 4 -> H5", missionModeName(4) == "H5");
	check("missionModeName 5 -> H6", missionModeName(5) == "H6");
	check("missionModeName 0 -> empty", missionModeName(0) == "");
}


// CONTESTSTART / CONTESTSTATUS


static void test_makeContestStartLine_v5()
{
	auto line = makeContestStartLine("H5");
	check("makeContestStartLine H5", line == "CONTESTSTART,H5");
}

static void test_makeContestStatusQueryLine()
{
	auto line = makeContestStatusQueryLine();
	check("makeContestStatusQueryLine", line == "CONTESTSTATUS?");
}


// UART parseLine (M0005 / PROTO)


static void test_parseLine_m0005()
{
	auto msg = Uart::parseLine("M0005");
	check("parseLine M0005 -> KEY_EVENT",
	      msg.type == UartMessageType::KEY_EVENT);
}

static void test_parseLine_crlf()
{
	auto msg = Uart::parseLine("BOOT,OK");
	check("parseLine BOOT,OK (no newline)",
	      msg.type == UartMessageType::BOOT);
}

static void test_parseLine_proto()
{
	auto msg = Uart::parseLine("PROTO,5");
	check("parseLine PROTO,5 -> type PROTOCOL",
	      msg.type == UartMessageType::PROTOCOL);
}


// main


int main()
{
	std::cout << "=== V5.2 Protocol Unit Tests ===\n\n";

	std::cout << "[1] makeTargetLine\n";
	test_makeTargetLine_correct();
	test_makeTargetLine_seq_wrap();
	test_makeTargetLine_nan_rejected();
	test_makeTargetLine_inf_rejected();
	test_makeTargetLine_confidence_lt0();
	test_makeTargetLine_confidence_gt1();
	test_makeTargetLine_zero_confidence();
	test_makeTargetLine_one_confidence();

	std::cout << "\n[2] makeLostLine\n";
	test_makeLostLine_correct();
	test_makeLostLine_large_seq();

	std::cout << "\n[3] makeBallLine (V5 legacy)\n";
	test_makeBallLine_ok();
	test_makeBallLine_lost();
	test_makeBallLine_calib();
	test_makeBallLine_error();
	test_makeBallLine_conf_neg_rejected();
	test_makeBallLine_conf_256_rejected();
	test_makeBallLine_unknown_status_rejected();
	test_makeBallLine_lost_nonzero_offset_rejected();
	test_makeBallLine_calib_nonzero_conf_rejected();
	test_makeBallLine_max_seq_accepted();
	test_makeBallLine_negative_offset_accepted();
	test_makeBallLine_ok_zero_zero();
	test_makeBallLine_error_nonzero_rejected();

	std::cout << "\n[4] isBootOk\n";
	test_isBootOk_valid();
	test_isBootOk_boot_error();
	test_isBootOk_extra_fields();
	test_isBootOk_wrong_tag();

	std::cout << "\n[5] isPingResponse\n";
	test_isPingResponse_valid();
	test_isPingResponse_extra();
	test_isPingResponse_ping_extra();
	test_isPingResponse_wrong_type();

	std::cout << "\n[6] getProtocolVersion (V5 legacy)\n";
	test_getProtocolVersion_valid();
	test_getProtocolVersion_v4_accepted();
	test_getProtocolVersion_abc_rejected();
	test_getProtocolVersion_extra_rejected();
	test_getProtocolVersion_empty();
	test_getProtocolVersion_proto_query();

	std::cout << "\n[7] V5.2 PROTO major/minor\n";
	test_getProtoMajorMinor_valid();
	test_getProtoMajorMinor_old_format_rejected();
	test_getProtoMajorMinor_abc_rejected();

	std::cout << "\n[8] CAPS response\n";
	test_isCapsResponse_valid();
	test_isCapsResponse_wrong();

	std::cout << "\n[9] M000X mission code\n";
	test_isMissionCode_valid();
	test_isMissionCode_invalid();
	test_parseMissionCode();
	test_missionModeName();

	std::cout << "\n[10] CONTESTSTART / CONTESTSTATUS\n";
	test_makeContestStartLine_v5();
	test_makeContestStatusQueryLine();

	std::cout << "\n[11] UART parseLine (M0005 / PROTO)\n";
	test_parseLine_m0005();
	test_parseLine_crlf();
	test_parseLine_proto();

	std::cout << '\n' << "========================================\n";
	std::cout << "Results: " << passed << " passed, " << failed
	          << " failed, " << (passed + failed) << " total\n";
	std::cout << "========================================\n";

	return failed == 0 ? 0 : 1;
}