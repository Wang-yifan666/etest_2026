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

	// 工程通信协议 V5 辅助函数

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

	// 构造 BALL 行：BALL,<seq>,<offset_mm>,<confidence_0_255>,<status>
	// offset_mm: int32 一维偏差 (mm)
	// confidence_0_255: 0~255
	// status: "OK" | "LOST" | "CALIB" | "ERROR"
	// V5 强制：非 OK 状态时 offset_mm 与 confidence_0_255 必须为 0
	// 非法 status / confidence 超范围 / 非 OK 携带非零值时返回 std::nullopt
	// 返回不含 \r\n 的行文本
	std::optional<std::string> makeBallLine(std::uint32_t seq,
	                                        int offset_mm,
	                                        int confidence_0_255,
	                                        const std::string& status);

	// 严格判断是否为 BOOT,OK 消息
	// tag=="BOOT" && fields.size()==1 && fields[0]=="OK"
	// 注意：UartMessage 的 fields 不包含 tag，tag 是单独存储的
	bool isBootOk(const UartMessage& message) noexcept;

	// 判断是否为 POWEROFF 消息（下位机关机指令）
	// 大小写不敏感：tag=="POWEROFF" 或 tag=="poweroff"
	bool isPoweroff(const UartMessage& message) noexcept;

	// 严格判断是否为 PING 响应：OK,PING
	// type 必须为 OK，且 fields[0] 必须完全等于 "PING"
	bool isPingResponse(const UartMessage& message) noexcept;

	// 解析 PROTO,<major>,<minor> 中的主版本号
	// tag 必须为 "PROTO"，fields.size()>=2，fields[0] 为十进制整数（主版本）
	// 版本非法时返回 std::nullopt
	std::optional<int> getProtocolVersionMajor(
	    const UartMessage& message) noexcept;

	// 解析 PROTO,<major>,<minor> 中的次版本号
	std::optional<int> getProtocolVersionMinor(
	    const UartMessage& message) noexcept;

	// 解析 PROTO,<version> 中的版本号（旧版兼容：单字段 PROTO,5）
	// tag 必须为 "PROTO"，fields.size()==1，fields[0] 为十进制整数
	// 版本非法时返回 std::nullopt
	std::optional<int> getProtocolVersion(
	    const UartMessage& message) noexcept;

	// 判断是否为 CAPS 响应
	bool isCapsResponse(const UartMessage& msg) noexcept;

	// ── V5 协议辅助 ──

	// VSESSION,<session>,MONOTONIC,<fps_x100>,<camera_id>
	std::string makeVsessionLine(std::uint32_t session_id, int fps_x100,
	                             const std::string& camera_id);

	// 判断是否为 OK,VSESSION,<session>
	bool isVsessionAck(const UartMessage& msg,
	                   std::uint32_t session_id) noexcept;

	// BALL,<session>,<seq>,<capture_ms>,<age_ms>,<position_0p1mm>,<confidence>,<status>
	// confidence 为 float 0.0~1.0
	std::optional<std::string> makeBallLineV5Simple(
	    std::uint32_t session_id, std::uint32_t seq,
	    std::uint32_t capture_ms, std::uint32_t age_ms,
	    int position_0p1mm, float confidence,
	    const std::string& status);

	// CONTESTSTART,<Hx>
	std::string makeContestStartLine(const std::string& mode);

	// CONTESTSTATUS? 查询行
	std::string makeContestStatusQueryLine();

	// CONTESTSTOP
	std::string makeContestStopLine();

	// 判断是否为 OK,CONTESTSTART,<mode>,ACCEPTED
	bool isContestStartAck(const UartMessage& msg) noexcept;

	// DONE 解析
	struct DoneInfo
	{
		std::string mode;
		int elapsed_ms = 0;
		int distance = 0;
		std::string result;
	};

	std::optional<DoneInfo> parseDone(const UartMessage& msg) noexcept;

	// ── M000X 模式应答 ──

	// ACK,M0001 / ACK,M0002 ...
	std::string makeModeAckLine(const std::string& mode);

	// START,M0001,0 / START,M0005,-372 ...
	// target_0p1mm: 目标位置，0.1 mm 单位
	std::string makeStartLine(const std::string& mode,
	                          int target_0p1mm);

	// CALIB_FAIL,M0003,NOT_AT_CENTER ...
	std::string makeCalibrationFailLine(const std::string& mode,
	                                    const std::string& reason);

	// ── M000X 题目编号 ──

	// 判断是否为 M0001~M0005
	bool isMissionCode(const UartMessage& msg) noexcept;

	// 解析 M000X → 题目编号 1~5
	// 非 M000X 时返回 0
	int parseMissionCode(const UartMessage& msg) noexcept;

	// 题目编号 → 模式名 Hx
	// 1→H2, 2→H3, 3→H4, 4→H5, 5→H6
	std::string missionModeName(int mission_code);

} // namespace etest::uart::protocol