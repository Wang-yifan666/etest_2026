#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace etest
{
	struct UartMessage;
}

namespace etest::uart::protocol
{

	// 工程通信协议 V4 辅助函数

	// 构造 TARGET 行：TARGET,<seq>,<x>,<y>,<angle>,<confidence>
	// x/y/angle 保留两位小数，confidence 保留三位小数
	// 使用 std::locale::classic()，不受系统区域设置影响
	// 拒绝 NaN、Inf、confidence 不在 0~1 等非法输入
	// 返回不含 \r\n 的行文本，非法时返回 std::nullopt
	std::optional<std::string> makeTargetLine(std::uint32_t seq,
	                                          double x, double y,
	                                          double angle,
	                                          double confidence);

	// 构造 LOST 行：LOST,<seq>
	// 返回不含 \r\n 的行文本
	std::string makeLostLine(std::uint32_t seq);

	// 严格判断是否为 BOOT,OK 消息
	// tag=="BOOT" && fields.size()==1 && fields[0]=="OK"
	// 注意：UartMessage 的 fields 不包含 tag，tag 是单独存储的
	bool isBootOk(const UartMessage& message) noexcept;

	// 严格判断是否为 PING 响应：OK,PING
	// type 必须为 OK，且 fields[0] 必须完全等于 "PING"
	bool isPingResponse(const UartMessage& message) noexcept;

	// 解析 PROTO,<version> 中的版本号
	// tag 必须为 "PROTO"，fields.size()==1，fields[0] 为十进制整数
	// 版本非法时返回 std::nullopt
	std::optional<int> getProtocolVersion(
	    const UartMessage& message) noexcept;

} // namespace etest::uart::protocol