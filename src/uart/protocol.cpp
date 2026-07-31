#include "uart/protocol.hpp"

#include "uart/uart.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <locale>
#include <sstream>
#include <string>

namespace etest::uart::protocol
{

	std::optional<std::string> makeTargetLine(std::uint32_t seq,
	                                          double x, double y,
	                                          double angle,
	                                          double confidence)
	{
		if(!std::isfinite(x) || !std::isfinite(y)
		   || !std::isfinite(angle) || !std::isfinite(confidence))
		{
			return std::nullopt;
		}

		if(confidence < 0.0 || confidence > 1.0)
		{
			return std::nullopt;
		}

		std::ostringstream oss;
		oss.imbue(std::locale::classic());
		oss << "TARGET," << seq << ',' << std::fixed
		    << std::setprecision(2) << x << ',' << std::fixed
		    << std::setprecision(2) << y << ',' << std::fixed
		    << std::setprecision(2) << angle << ',' << std::fixed
		    << std::setprecision(3) << confidence;

		return oss.str();
	}

	std::string makeLostLine(std::uint32_t seq)
	{
		std::ostringstream oss;
		oss.imbue(std::locale::classic());
		oss << "LOST," << seq;
		return oss.str();
	}

	bool isBootOk(const UartMessage& message) noexcept
	{
		if(message.tag != "BOOT")
		{
			return false;
		}

		if(message.fields.size() != 1)
		{
			return false;
		}

		return message.fields[0] == "OK";
	}

	bool isPoweroff(const UartMessage& message) noexcept
	{
		return message.tag == "POWEROFF" || message.tag == "poweroff";
	}

	bool isPingResponse(const UartMessage& message) noexcept
	{
		if(message.type != UartMessageType::OK)
		{
			return false;
		}

		if(message.tag != "OK")
		{
			return false;
		}

		if(message.fields.size() != 1)
		{
			return false;
		}

		return message.fields[0] == "PING";
	}

	std::optional<int> getProtocolVersion(
	    const UartMessage& message) noexcept
	{
		if(message.tag != "PROTO")
		{
			return std::nullopt;
		}

		if(message.fields.size() != 1)
		{
			return std::nullopt;
		}

		const std::string& version_str = message.fields[0];

		int version = 0;
		const auto [ptr, ec] = std::from_chars(
		    version_str.data(), version_str.data() + version_str.size(),
		    version);

		if(ec != std::errc{}
		   || ptr != version_str.data() + version_str.size())
		{
			return std::nullopt;
		}

		return version;
	}

	std::optional<int> getProtocolVersionMajor(
	    const UartMessage& message) noexcept
	{
		if(message.tag != "PROTO")
			return std::nullopt;

		if(message.fields.size() < 2)
			return std::nullopt;

		const std::string& s = message.fields[0];
		int v = 0;
		const auto [ptr, ec] =
		    std::from_chars(s.data(), s.data() + s.size(), v);
		if(ec != std::errc{} || ptr != s.data() + s.size())
			return std::nullopt;
		return v;
	}

	std::optional<int> getProtocolVersionMinor(
	    const UartMessage& message) noexcept
	{
		if(message.tag != "PROTO")
			return std::nullopt;

		if(message.fields.size() < 2)
			return std::nullopt;

		const std::string& s = message.fields[1];
		int v = 0;
		const auto [ptr, ec] =
		    std::from_chars(s.data(), s.data() + s.size(), v);
		if(ec != std::errc{} || ptr != s.data() + s.size())
			return std::nullopt;
		return v;
	}

	bool isCapsResponse(const UartMessage& msg) noexcept
	{
		return msg.tag == "CAPS";
	}

	std::optional<std::string> makeBallLine(std::uint32_t seq,
	                                        int offset_mm,
	                                        int confidence_0_255,
	                                        const std::string& status)
	{
		if(confidence_0_255 < 0 || confidence_0_255 > 255)
		{
			return std::nullopt;
		}

		if(status != "OK" && status != "LOST" && status != "CALIB"
		   && status != "ERROR")
		{
			return std::nullopt;
		}

		// V5 强制：非 OK 状态必须零位置、零置信度
		if(status != "OK" && (offset_mm != 0 || confidence_0_255 != 0))
		{
			return std::nullopt;
		}

		std::ostringstream oss;
		oss.imbue(std::locale::classic());
		oss << "BALL," << seq << "," << offset_mm << ","
		    << confidence_0_255 << "," << status;
		return oss.str();
	}

	// ── V5 简化版协议辅助 ──

	std::string makeVsessionLine(std::uint32_t session_id, int fps_x100,
	                             const std::string& camera_id)
	{
		std::ostringstream oss;
		oss.imbue(std::locale::classic());
		oss << "VSESSION," << session_id << ",MONOTONIC," << fps_x100
		    << "," << camera_id;
		return oss.str();
	}

	bool isVsessionAck(const UartMessage& msg,
	                   std::uint32_t session_id) noexcept
	{
		if(msg.type != UartMessageType::OK)
			return false;
		if(msg.tag != "OK")
			return false;
		if(msg.fields.size() < 2)
			return false;
		if(msg.fields[0] != "VSESSION")
			return false;

		std::uint32_t parsed = 0;
		const auto& field = msg.fields[1];
		const auto [ptr, ec] = std::from_chars(
		    field.data(), field.data() + field.size(), parsed);
		if(ec != std::errc{})
			return false;
		return parsed == session_id;
	}

	std::optional<std::string> makeBallLineV5Simple(
	    std::uint32_t session_id, std::uint32_t seq,
	    std::uint32_t capture_ms, std::uint32_t age_ms,
	    int position_0p1mm, float confidence, const std::string& status)
	{
		if(status != "OK" && status != "LOST" && status != "CALIB"
		   && status != "ERROR")
			return std::nullopt;

		if(!std::isfinite(confidence) || confidence < 0.0F
		   || confidence > 1.0F)
			return std::nullopt;

		// 非 OK 状态强制零位零置信度
		if(status != "OK"
		   && (position_0p1mm != 0 || confidence != 0.0F))
			return std::nullopt;

		std::ostringstream oss;
		oss.imbue(std::locale::classic());
		oss << "BALL," << session_id << "," << seq << "," << capture_ms
		    << "," << age_ms << "," << position_0p1mm << ","
		    << std::fixed << std::setprecision(2) << confidence << ","
		    << status;
		return oss.str();
	}

	std::string makeContestStartLine(const std::string& mode)
	{
		std::ostringstream oss;
		oss.imbue(std::locale::classic());
		oss << "CONTESTSTART," << mode;
		return oss.str();
	}

	std::string makeContestStatusQueryLine()
	{
		return "CONTESTSTATUS?";
	}

	std::string makeContestStopLine()
	{
		return "CONTESTSTOP";
	}

	bool isContestStartAck(const UartMessage& msg) noexcept
	{
		if(msg.type != UartMessageType::OK)
			return false;
		if(msg.tag != "OK")
			return false;
		if(msg.fields.size() < 3)
			return false;
		if(msg.fields[0] != "CONTESTSTART")
			return false;
		return msg.fields[2] == "ACCEPTED";
	}

	std::optional<DoneInfo> parseDone(const UartMessage& msg) noexcept
	{
		if(msg.type != UartMessageType::DONE)
			return std::nullopt;
		if(msg.fields.size() < 4)
			return std::nullopt;

		DoneInfo info;
		info.mode = msg.fields[0];

		// elapsed_ms
		{
			const auto& f = msg.fields[1];
			int v = 0;
			const auto [ptr, ec] =
			    std::from_chars(f.data(), f.data() + f.size(), v);
			if(ec != std::errc{})
				return std::nullopt;
			info.elapsed_ms = v;
		}

		// distance
		{
			const auto& f = msg.fields[2];
			int v = 0;
			const auto [ptr, ec] =
			    std::from_chars(f.data(), f.data() + f.size(), v);
			if(ec != std::errc{})
				return std::nullopt;
			info.distance = v;
		}

		info.result = msg.fields[3];
		return info;
	}

	// ── M000X 模式应答 ──

	std::string makeModeAckLine(const std::string& mode)
	{
		std::ostringstream oss;
		oss.imbue(std::locale::classic());
		oss << "ACK," << mode;
		return oss.str();
	}

	std::string makeStartLine(const std::string& mode, int target_0p1mm)
	{
		std::ostringstream oss;
		oss.imbue(std::locale::classic());
		oss << "START," << mode << "," << target_0p1mm;
		return oss.str();
	}

	std::string makeCalibrationFailLine(const std::string& mode,
	                                    const std::string& reason)
	{
		std::ostringstream oss;
		oss.imbue(std::locale::classic());
		oss << "CALIB_FAIL," << mode << "," << reason;
		return oss.str();
	}

	// ── M000X 题目编号 ──

	bool isMissionCode(const UartMessage& msg) noexcept
	{
		// M0001 ~ M0005
		if(msg.tag.size() != 5)
			return false;
		if(msg.tag.compare(0, 4, "M000") != 0)
			return false;
		char last = msg.tag[4];
		return last >= '1' && last <= '5';
	}

	int parseMissionCode(const UartMessage& msg) noexcept
	{
		if(!isMissionCode(msg))
			return 0;
		return msg.tag[4] - '0'; // 1~5
	}

	std::string missionModeName(int mission_code)
	{
		switch(mission_code)
		{
		case 1:
			return "H2";
		case 2:
			return "H3";
		case 3:
			return "H4";
		case 4:
			return "H5";
		case 5:
			return "H6";
		default:
			return "";
		}
	}

} // namespace etest::uart::protocol
