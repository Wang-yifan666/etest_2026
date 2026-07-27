#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace etest::uart::protocol
{

// 协议常量

constexpr int kProtocolVersion = 1;
constexpr std::size_t kMaxFrameLength = 512;
constexpr char kFrameDelimiter = '\n';

// CRC16-CCITT

// 计算 CRC16-CCITT（多项式 0x1021，初始值 0xFFFF）
// 计算范围由调用者决定
std::uint16_t crc16(const std::uint8_t* data, std::size_t size) noexcept;

// 将 16 位 CRC 编码为 4 字符十六进制大写字符串
std::string crc16ToHex(std::uint16_t crc);

// 从十六进制字符串解析 CRC16
bool hexToCrc16(const std::string& hex, std::uint16_t& out);

// 消息编解码

struct DecodedMessage
{
	bool valid = false;
	int version = 0;
	std::string type;
	std::uint32_t seq = 0;
	std::string payload;
	std::string error;
};

// 编码消息为帧：@1,TYPE,SEQ,PAYLOAD,CRC16
// TYPE: 消息类型（HELLO, READY, PING, PONG, TARGET, LOST, ERROR 等）
// SEQ: 序列号（0 表示无序列号）
// PAYLOAD: 消息负载
// 返回完整帧（不含换行符）
std::string encodeMessage(int version, const std::string& type,
                          std::uint32_t seq,
                          const std::string& payload);

// 解码一个完整帧（不含换行符前缀 @ 和尾随换行符需要调用者剥离）
// 只解码格式为 @version,type,seq,payload,crc 的帧
DecodedMessage decodeFrame(const std::string& frame);

// 帧缓冲（处理半包、粘包）

class FrameBuffer
{
public:
	// 喂入原始字节数据。返回已解析的完整帧（不含换行符）。
	// 半包暂存在内部缓冲区，粘包分批返回。
	std::vector<std::string> feed(const std::uint8_t* data,
	                              std::size_t size);

	// 喂入字符串数据
	std::vector<std::string> feed(const std::string& data);

	// 清空缓冲区
	void reset();

private:
	std::string buffer_;
};

} // namespace etest::uart::protocol