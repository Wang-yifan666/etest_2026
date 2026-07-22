#include "uart/uart.hpp"

#include <cstring>
#include <iostream>

#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

namespace etest
{

	Uart::Uart(UartConfig config): config_(std::move(config)) {}

	bool Uart::open()
	{
		fd_ = ::open(config_.device.c_str(), O_RDWR | O_NOCTTY | O_NDELAY);

		if(fd_ == -1)
		{
			std::cerr << "[Uart] Failed to open " << config_.device << '\n';

			return false;
		}

		struct termios tty;
		std::memset(&tty, 0, sizeof(tty));

		if(tcgetattr(fd_, &tty) != 0)
		{
			std::cerr << "[Uart] tcgetattr failed\n";

			::close(fd_);

			fd_ = -1;

			return false;
		}

		cfsetospeed(&tty, static_cast<speed_t>(config_.baudrate));
		cfsetispeed(&tty, static_cast<speed_t>(config_.baudrate));

		tty.c_cflag |= (CLOCAL | CREAD);
		tty.c_cflag &= ~CSIZE;
		tty.c_cflag |= CS8;
		tty.c_cflag &= ~PARENB;
		tty.c_cflag &= ~CSTOPB;
		tty.c_cflag &= ~CRTSCTS;

		tty.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);

		tty.c_iflag &= ~(IXON | IXOFF | IXANY);
		tty.c_iflag &= ~(INLCR | ICRNL | IGNCR);

		tty.c_oflag &= ~OPOST;

		tty.c_cc[VMIN] = 0;
		tty.c_cc[VTIME] = 10;

		if(tcsetattr(fd_, TCSANOW, &tty) != 0)
		{
			std::cerr << "[Uart] tcsetattr failed\n";

			::close(fd_);

			fd_ = -1;

			return false;
		}

		std::cout << "[Uart] Opened " << config_.device << " @ "
		          << config_.baudrate << " baud\n";

		return true;
	}

	void Uart::close()
	{
		if(fd_ != -1)
		{
			::close(fd_);

			fd_ = -1;
		}
	}

	bool Uart::send(const std::vector<std::uint8_t>& data)
	{
		if(fd_ == -1)
		{
			return false;
		}

		const auto written
		    = ::write(fd_, data.data(), data.size());

		return static_cast<size_t>(written) == data.size();
	}

	bool Uart::send(const std::string& text)
	{
		const std::vector<std::uint8_t> data(text.begin(), text.end());

		return send(data);
	}

	std::vector<std::uint8_t> Uart::receive(size_t max_len)
	{
		if(fd_ == -1)
		{
			return {};
		}

		std::vector<std::uint8_t> buf(max_len);

		const auto n = ::read(fd_, buf.data(), max_len);

		if(n > 0)
		{
			buf.resize(static_cast<size_t>(n));
		}
		else
		{
			buf.clear();
		}

		return buf;
	}

	bool Uart::isOpen() const
	{
		return fd_ != -1;
	}

} // namespace etest
