/**
 * @file test/test_protocol.cpp
 * @brief V4 协议辅助函数单元测试
 *
 * 测试 makeTargetLine、makeLostLine、isBootOk、isPingResponse、
 * getProtocolVersion。
 * 所有测试调用真实的 src/uart/protocol.cpp 实现，不复制代码。
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

// =============================================================================
// makeTargetLine 测试
// =============================================================================

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

// =============================================================================
// makeLostLine 测试
// =============================================================================

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

// =============================================================================
// isBootOk 测试
// =============================================================================

static void test_isBootOk_valid()
{
	// 构造正确的 BOOT,OK 消息：parseLine("BOOT,OK")
	auto msg = Uart::parseLine("BOOT,OK");
	// parseLine 设置 tag="BOOT", fields=["OK"]
	check("parseLine BOOT,OK -> type BOOT",
	      msg.type == UartMessageType::BOOT);
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

// =============================================================================
// isPingResponse 测试
// =============================================================================

static void test_isPingResponse_valid()
{
	auto msg = Uart::parseLine("OK,PING");
	// parseLine: tag="OK", type=OK, fields=["PING"]
	check("parseLine OK,PING -> type OK",
	      msg.type == UartMessageType::OK);
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

// =============================================================================
// getProtocolVersion 测试
// =============================================================================

static void test_getProtocolVersion_valid()
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
	auto msg = Uart::parseLine("PROTO,4,EXTRA");
	auto v = getProtocolVersion(msg);
	check("getProtocolVersion PROTO,4,EXTRA -> nullopt",
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
	// PROTO? 会被解析为类型 DATA（因为末尾有?，不匹配PROTO）
	// 确认 getProtocolVersion 拒绝它
	auto v = getProtocolVersion(msg);
	check("getProtocolVersion PROTO? -> nullopt (tag mismatch)",
	      !v.has_value());
}

// =============================================================================
// UART 行解析测试（\n vs \r\n 兼容性）
// =============================================================================

static void test_parseLine_crlf()
{
	// parseLine 不处理末尾换行，只接收纯文本行
	auto msg = Uart::parseLine("BOOT,OK");
	check("parseLine BOOT,OK (no newline)",
	      msg.type == UartMessageType::BOOT);
}

static void test_parseLine_proto()
{
	auto msg = Uart::parseLine("PROTO,4");
	check("parseLine PROTO,4 -> type PROTOCOL",
	      msg.type == UartMessageType::PROTOCOL);
}

// =============================================================================
// 入口
// =============================================================================

int main()
{
	std::cout << "=== V4 Protocol Unit Tests ===\n\n";

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

	std::cout << "\n[3] isBootOk\n";
	test_isBootOk_valid();
	test_isBootOk_boot_error();
	test_isBootOk_extra_fields();
	test_isBootOk_wrong_tag();

	std::cout << "\n[4] isPingResponse\n";
	test_isPingResponse_valid();
	test_isPingResponse_extra();
	test_isPingResponse_ping_extra();
	test_isPingResponse_wrong_type();

	std::cout << "\n[5] getProtocolVersion\n";
	test_getProtocolVersion_valid();
	test_getProtocolVersion_abc_rejected();
	test_getProtocolVersion_extra_rejected();
	test_getProtocolVersion_empty();
	test_getProtocolVersion_proto_query();

	std::cout << "\n[6] UART parseLine\n";
	test_parseLine_crlf();
	test_parseLine_proto();

	std::cout << '\n' << "========================================\n";
	std::cout << "Results: " << passed << " passed, " << failed
	          << " failed, " << (passed + failed) << " total\n";
	std::cout << "========================================\n";

	return failed == 0 ? 0 : 1;
}