#include "uart/protocol.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

namespace etest::uart::protocol
{

// CRC16-CCITT 查表

namespace
{

// CRC16-CCITT 查找表（多项式 0x1021）
constexpr std::uint16_t kCrc16Table[256] = {
    0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50A5, 0x60C6, 0x70E7,
    0x8108, 0x9129, 0xA14A, 0xB16B, 0xC18C, 0xD1AD, 0xE1CE, 0xF1EF,
    0x1231, 0x0210, 0x3273, 0x2252, 0x52B5, 0x4294, 0x72F7, 0x62D6,
    0x9339, 0x8318, 0xB37B, 0xA35A, 0xD3BD, 0xC39C, 0xF3FF, 0xE3DE,
    0x2462, 0x3443, 0x0420, 0x1401, 0x64E6, 0x74C7, 0x44A4, 0x5485,
    0xA56A, 0xB54B, 0x8528, 0x9509, 0xE5EE, 0xF5CF, 0xC5AC, 0xD58D,
    0x3653, 0x2672, 0x1611, 0x0630, 0x76D7, 0x66F6, 0x5695, 0x46B4,
    0xB75B, 0xA77A, 0x9719, 0x8738, 0xF7DF, 0xE7FE, 0xD79D, 0xC7BC,
    0x48C4, 0x58E5, 0x6886, 0x78A7, 0x0840, 0x1861, 0x2802, 0x3823,
    0xC9CC, 0xD9ED, 0xE98E, 0xF9AF, 0x8948, 0x9969, 0xA90A, 0xB92B,
    0x5AF5, 0x4AD4, 0x7AB7, 0x6A96, 0x1A71, 0x0A50, 0x3A33, 0x2A12,
    0xDBFD, 0xCBDC, 0xFBBF, 0xEB9E, 0x9B79, 0x8B58, 0xBB3B, 0xAB1A,
    0x6CA6, 0x7C87, 0x4CE4, 0x5CC5, 0x2C22, 0x3C03, 0x0C60, 0x1C41,
    0xEDAE, 0xFD8F, 0xCDEC, 0xDDCD, 0xAD2A, 0xBD0B, 0x8D68, 0x9D49,
    0x7E97, 0x6EB6, 0x5ED5, 0x4EF4, 0x3E13, 0x2E32, 0x1E51, 0x0E70,
    0xFF9F, 0xEFBE, 0xDFDD, 0xCFFC, 0xBF1B, 0xAF3A, 0x9F59, 0x8F78,
    0x9188, 0x81A9, 0xB1CA, 0xA1EB, 0xD10C, 0xC12D, 0xF14E, 0xE16F,
    0x1080, 0x00A1, 0x30C2, 0x20E3, 0x5004, 0x4025, 0x7046, 0x6067,
    0x83B9, 0x9398, 0xA3FB, 0xB3DA, 0xC33D, 0xD31C, 0xE37F, 0xF35E,
    0x02B1, 0x1290, 0x22F3, 0x32D2, 0x4235, 0x5214, 0x6277, 0x7256,
    0xB5EA, 0xA5CB, 0x95A8, 0x8589, 0xF56E, 0xE54F, 0xD52C, 0xC50D,
    0x34E2, 0x24C3, 0x14A0, 0x0481, 0x7466, 0x6447, 0x5424, 0x4405,
    0xA7DB, 0xB7FA, 0x8799, 0x97B8, 0xE75F, 0xF77E, 0xC71D, 0xD73C,
    0x26D3, 0x36F2, 0x0691, 0x16B0, 0x6657, 0x7676, 0x4615, 0x5634,
    0xD94C, 0xC96D, 0xF90E, 0xE92F, 0x99C8, 0x89E9, 0xB98A, 0xA9AB,
    0x5844, 0x4865, 0x7806, 0x6827, 0x18C0, 0x08E1, 0x3882, 0x28A3,
    0xCB7D, 0xDB5C, 0xEB3F, 0xFB1E, 0x8BF9, 0x9BD8, 0xABBB, 0xBB9A,
    0x4A75, 0x5A54, 0x6A37, 0x7A16, 0x0AF1, 0x1AD0, 0x2AB3, 0x3A92,
    0xFD2E, 0xED0F, 0xDD6C, 0xCD4D, 0xBDAA, 0xAD8B, 0x9DE8, 0x8DC9,
    0x7C26, 0x6C07, 0x5C64, 0x4C45, 0x3CA2, 0x2C83, 0x1CE0, 0x0CC1,
    0xEF1F, 0xFF3E, 0xCF5D, 0xDF7C, 0xAF9B, 0xBFBA, 0x8FD9, 0x9FF8,
    0x6E17, 0x7E36, 0x4E55, 0x5E74, 0x2E93, 0x3EB2, 0x0ED1, 0x1EF0};

} // namespace

std::uint16_t crc16(const std::uint8_t* data,
                    std::size_t size) noexcept
{
	std::uint16_t crc = 0xFFFF;

	for(std::size_t i = 0; i < size; ++i)
	{
		crc = static_cast<std::uint16_t>(
		    (crc << 8) ^ kCrc16Table[((crc >> 8) ^ data[i]) & 0xFF]);
	}

	return crc;
}

std::string crc16ToHex(std::uint16_t crc)
{
	char buf[5];
	std::snprintf(buf, sizeof(buf), "%04X", crc);
	return std::string(buf);
}

bool hexToCrc16(const std::string& hex, std::uint16_t& out)
{
	if(hex.size() != 4)
	{
		return false;
	}

	std::uint16_t value = 0;

	for(char ch : hex)
	{
		value <<= 4;

		if(ch >= '0' && ch <= '9')
		{
			value |= static_cast<std::uint16_t>(ch - '0');
		}
		else if(ch >= 'A' && ch <= 'F')
		{
			value |=
			    static_cast<std::uint16_t>(ch - 'A' + 10);
		}
		else if(ch >= 'a' && ch <= 'f')
		{
			value |=
			    static_cast<std::uint16_t>(ch - 'a' + 10);
		}
		else
		{
			return false;
		}
	}

	out = value;
	return true;
}

// 消息编解码

std::string encodeMessage(int version, const std::string& type,
                          std::uint32_t seq,
                          const std::string& payload)
{
	// 构建 TYPE,SEQ,PAYLOAD 部分
	std::ostringstream body;
	body << type << ',' << seq << ',' << payload;

	const std::string body_str = body.str();

	// CRC 计算范围：body_str 的全部字节
	const std::uint16_t crc = crc16(
	    reinterpret_cast<const std::uint8_t*>(body_str.data()),
	    body_str.size());

	// 构建完整帧：@version,body,crc
	std::ostringstream frame;
	frame << '@' << version << ',' << body_str << ',' << crc16ToHex(crc);

	return frame.str();
}

DecodedMessage decodeFrame(const std::string& frame)
{
	DecodedMessage result;

	if(frame.empty() || frame[0] != '@')
	{
		result.error = "missing leading '@'";
		return result;
	}

	// 检查帧长度
	if(frame.size() > kMaxFrameLength)
	{
		result.error = "frame too long: " + std::to_string(frame.size())
		               + " > " + std::to_string(kMaxFrameLength);
		return result;
	}

	// 解析 @version,type,seq,payload,crc
	// 跳过 @
	const std::string content = frame.substr(1);

	// 按逗号分割，最多 4 部分（最后一部分是 CRC）
	std::vector<std::string> parts;
	std::size_t pos = 0;

	for(int i = 0; i < 4; ++i)
	{
		const std::size_t comma = content.find(',', pos);

		if(comma == std::string::npos)
		{
			result.error =
			    "missing field " + std::to_string(i + 1)
			    + " of 5 required fields";
			return result;
		}

		parts.push_back(content.substr(pos, comma - pos));
		pos = comma + 1;
	}

	// 第 5 部分是 CRC（剩余全部）
	parts.push_back(content.substr(pos));

	// 验证 5 个字段
	if(parts.size() != 5)
	{
		result.error = "expected 5 fields, got "
		               + std::to_string(parts.size());
		return result;
	}

	const std::string& version_str = parts[0];
	const std::string& type_str = parts[1];
	const std::string& seq_str = parts[2];
	const std::string& payload_str = parts[3];
	const std::string& crc_str = parts[4];

	// 解析 version
	int version = 0;
	{
		const auto [ptr, ec] = std::from_chars(
		    version_str.data(),
		    version_str.data() + version_str.size(), version);

		if(ec != std::errc{} || version <= 0 || version > 99)
		{
			result.error = "invalid version: " + version_str;
			return result;
		}
	}

	// 解析 seq
	std::uint32_t seq = 0;
	{
		const auto [ptr, ec] = std::from_chars(
		    seq_str.data(), seq_str.data() + seq_str.size(), seq);

		if(ec != std::errc{})
		{
			result.error = "invalid seq: " + seq_str;
			return result;
		}
	}

	// 验证 type 不为空且仅含大写字母和数字
	if(type_str.empty())
	{
		result.error = "empty type";
		return result;
	}

	for(char ch : type_str)
	{
		if(!std::isalnum(static_cast<unsigned char>(ch))
		   || (ch >= 'a' && ch <= 'z'))
		{
			result.error =
			    "type must be uppercase alphanumeric: " + type_str;
			return result;
		}
	}

	// 校验 CRC
	std::uint16_t expected_crc = 0;

	if(!hexToCrc16(crc_str, expected_crc))
	{
		result.error = "invalid CRC hex: " + crc_str;
		return result;
	}

	// CRC 计算范围：type_str + ',' + seq_str + ',' + payload_str
	const std::string crc_body =
	    type_str + ',' + seq_str + ',' + payload_str;

	const std::uint16_t computed_crc = crc16(
	    reinterpret_cast<const std::uint8_t*>(crc_body.data()),
	    crc_body.size());

	if(computed_crc != expected_crc)
	{
		result.error =
		    "CRC mismatch: expected=" + crc16ToHex(expected_crc)
		    + ", computed=" + crc16ToHex(computed_crc);
		return result;
	}

	result.valid = true;
	result.version = version;
	result.type = type_str;
	result.seq = seq;
	result.payload = payload_str;
	return result;
}

// FrameBuffer

std::vector<std::string> FrameBuffer::feed(const std::uint8_t* data,
                                           std::size_t size)
{
	std::string str(reinterpret_cast<const char*>(data), size);
	return feed(str);
}

std::vector<std::string> FrameBuffer::feed(const std::string& data)
{
	std::vector<std::string> frames;

	for(char ch : data)
	{
		if(ch == kFrameDelimiter)
		{
			if(!buffer_.empty())
			{
				// 剥离前缀 @ 或空白
				std::string frame = buffer_;
				buffer_.clear();

				frames.push_back(std::move(frame));
			}
		}
		else
		{
			buffer_ += ch;

			// 防止无限增长（丢弃超长行）
			if(buffer_.size() > kMaxFrameLength)
			{
				buffer_.clear();
			}
		}
	}

	return frames;
}

void FrameBuffer::reset()
{
	buffer_.clear();
}

} // namespace etest::uart::protocol