#include "uart/uart.hpp"

#include <cstring>
#include <string>

#include "core/logger.hpp"

#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

namespace etest
{

	Uart::Uart(UartConfig config): config_(std::move(config)) {}

	Uart::~Uart()
	{
		close();
	}

	bool Uart::open() noexcept
	{
		fd_ = ::open(config_.device.c_str(), O_RDWR | O_NOCTTY | O_NDELAY);

		if(fd_ == -1)
		{
			ETEST_LOG_ERROR("UART",
			                "Failed to open " + config_.device);

			return false;
		}

		struct termios tty;
		std::memset(&tty, 0, sizeof(tty));

		if(tcgetattr(fd_, &tty) != 0)
		{
			ETEST_LOG_ERROR("UART", "tcgetattr failed");

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
			ETEST_LOG_ERROR("UART", "tcsetattr failed");

			::close(fd_);

			fd_ = -1;

			return false;
		}

		ETEST_LOG_INFO("UART",
		               "Opened " + config_.device + " @ "
		                   + std::to_string(config_.baudrate) + " baud");

		return true;
	}

	void Uart::close() noexcept
	{
		if(fd_ != -1)
		{
			::close(fd_);

			fd_ = -1;
		}
	}

	bool Uart::send(const std::vector<std::uint8_t>& data) noexcept
	{
		if(fd_ == -1)
		{
			return false;
		}

		const auto written
		    = ::write(fd_, data.data(), data.size());

		return static_cast<size_t>(written) == data.size();
	}

	bool Uart::send(const std::string& text) noexcept
	{
		const std::vector<std::uint8_t> data(text.begin(), text.end());

		return send(data);
	}

	std::vector<std::uint8_t> Uart::receive(std::size_t max_len) noexcept
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

	bool Uart::isOpen() const noexcept
	{
		return fd_ != -1;
	}

} // namespace etest
