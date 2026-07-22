#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace etest
{

	struct UartConfig
	{
		std::string device = "/dev/ttyAMA0";
		int baudrate = 115200;
	};

	class Uart
	{
	public:
		explicit Uart(UartConfig config = {});

		bool open();
		void close();

		bool send(const std::vector<std::uint8_t>& data);
		bool send(const std::string& text);

		std::vector<std::uint8_t> receive(size_t max_len = 256);

		bool isOpen() const;

	private:
		UartConfig config_;
		int fd_ = -1;
	};

} // namespace etest
