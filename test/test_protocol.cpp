#include "uart/protocol.hpp"

#include <cassert>
#include <cstring>
#include <iostream>
#include <string>

using namespace etest::uart::protocol;

// 测试 1：CRC16 已知测试向量
static void test_crc16_known_vector()
{
	// 空数据：CRC16-CCITT 初始值为 0xFFFF，空数据 CRC = 0xFFFF
	const std::uint8_t empty = 0;
	assert(crc16(&empty, 0) == 0xFFFF);

	// 测试字符串 "123456789"
	const std::string test = "123456789";
	const std::uint16_t result = crc16(
	    reinterpret_cast<const std::uint8_t*>(test.data()),
	    test.size());

	// CRC16-CCITT of "123456789" = 0x29B1
	assert(result == 0x29B1);

	std::cout << "[PASS] test_crc16_known_vector\n";
}

// 测试 2：正确协议帧编码和解码
static void test_encode_decode_valid_frame()
{
	const std::string frame =
	    encodeMessage(1, "HELLO", 1, "UPPER");

	assert(!frame.empty());
	assert(frame[0] == '@');

	// 解码
	const auto decoded = decodeFrame(frame);

	assert(decoded.valid);
	assert(decoded.version == 1);
	assert(decoded.type == "HELLO");
	assert(decoded.seq == 1);
	assert(decoded.payload == "UPPER");

	std::cout << "[PASS] test_encode_decode_valid_frame\n";
}

// 测试 3：错误 CRC 被拒绝
static void test_decode_bad_crc_rejected()
{
	// 构造一个正确帧，然后修改 CRC
	const std::string frame =
	    encodeMessage(1, "PING", 3, "0");

	// 修改最后一个字符
	std::string bad_frame = frame;
	bad_frame.back() = '0';

	const auto decoded = decodeFrame(bad_frame);

	assert(!decoded.valid);
	assert(!decoded.error.empty());

	std::cout << "[PASS] test_decode_bad_crc_rejected\n";
}

// 测试 4：超长帧被拒绝
static void test_decode_frame_too_long()
{
	std::string long_frame(600, 'X');
	long_frame[0] = '@';

	const auto decoded = decodeFrame(long_frame);

	assert(!decoded.valid);
	assert(decoded.error.find("too long") != std::string::npos);

	std::cout << "[PASS] test_decode_frame_too_long\n";
}

// 测试 5：字段缺失被拒绝
static void test_decode_missing_fields()
{
	// 只有 @1,HELLO
	const auto decoded = decodeFrame("@1,HELLO");

	assert(!decoded.valid);
	assert(decoded.error.find("missing field") != std::string::npos);

	std::cout << "[PASS] test_decode_missing_fields\n";
}

// 测试 6：半包拼接
static void test_half_packet_reassembly()
{
	FrameBuffer buffer;

	// 发送前半部分
	const std::string frame =
	    encodeMessage(1, "TARGET", 100, "320.5;241.2;0.92");

	const std::string first_half =
	    frame.substr(0, frame.size() / 2);

	const std::string second_half =
	    frame.substr(frame.size() / 2);

	auto frames = buffer.feed(first_half);
	assert(frames.empty()); // 没有完整帧

	frames = buffer.feed(second_half + "\n");
	assert(!frames.empty());
	assert(frames[0] == frame);

	std::cout << "[PASS] test_half_packet_reassembly\n";
}

// 测试 7：多包拆分
static void test_multiple_packets()
{
	FrameBuffer buffer;

	const std::string f1 = encodeMessage(1, "PING", 1, "0");
	const std::string f2 = encodeMessage(1, "PONG", 1, "0");

	const std::string data = f1 + "\n" + f2 + "\n";

	auto frames = buffer.feed(data);

	assert(frames.size() == 2);
	assert(frames[0] == f1);
	assert(frames[1] == f2);

	std::cout << "[PASS] test_multiple_packets\n";
}

// 测试 8：TARGET 编码解码
static void test_target_encode_decode()
{
	const std::string frame =
	    encodeMessage(1, "TARGET", 153, "320.5;241.2;-3.7;0.92");

	const auto decoded = decodeFrame(frame);

	assert(decoded.valid);
	assert(decoded.type == "TARGET");
	assert(decoded.seq == 153);
	assert(decoded.payload == "320.5;241.2;-3.7;0.92");

	std::cout << "[PASS] test_target_encode_decode\n";
}

// 测试 9：LOST 不含旧坐标
static void test_lost_no_coordinates()
{
	const std::string frame = encodeMessage(1, "LOST", 154, "0");

	const auto decoded = decodeFrame(frame);

	assert(decoded.valid);
	assert(decoded.type == "LOST");
	assert(decoded.seq == 154);

	// LOST 的负载中不应包含坐标
	assert(decoded.payload.find(';') == std::string::npos);

	std::cout << "[PASS] test_lost_no_coordinates\n";
}

// 测试 10：crc16ToHex 和 hexToCrc16 往返
static void test_crc_hex_roundtrip()
{
	std::uint16_t original = 0xA3F8;
	std::string hex = crc16ToHex(original);
	assert(hex.size() == 4);

	std::uint16_t parsed = 0;
	assert(hexToCrc16(hex, parsed));
	assert(parsed == original);

	std::cout << "[PASS] test_crc_hex_roundtrip\n";
}

// 测试 11：无效 CRC 十六进制
static void test_invalid_crc_hex()
{
	std::uint16_t out = 0;

	assert(!hexToCrc16("XYZ", out));     // 太短
	assert(!hexToCrc16("GHIJ", out));    // 无效字符
	assert(!hexToCrc16("12 34", out));   // 含空格

	std::cout << "[PASS] test_invalid_crc_hex\n";
}

// 测试 12：FrameBuffer 半包清空
static void test_frame_buffer_reset()
{
	FrameBuffer buffer;

	buffer.feed("@1,HELLO,"); // 半包

	buffer.reset();

	auto frames = buffer.feed("1,UPPER,12AB\n");

	assert(frames.empty()); // 前半部分已丢失

	std::cout << "[PASS] test_frame_buffer_reset\n";
}

int main()
{
	test_crc16_known_vector();
	test_encode_decode_valid_frame();
	test_decode_bad_crc_rejected();
	test_decode_frame_too_long();
	test_decode_missing_fields();
	test_half_packet_reassembly();
	test_multiple_packets();
	test_target_encode_decode();
	test_lost_no_coordinates();
	test_crc_hex_roundtrip();
	test_invalid_crc_hex();
	test_frame_buffer_reset();

	std::cout << "\nall protocol tests passed\n";
	return 0;
}