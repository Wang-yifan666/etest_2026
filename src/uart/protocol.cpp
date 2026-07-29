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

} // namespace etest::uart::protocol
