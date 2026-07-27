#include "uart/uart.hpp"

#include "core/logger.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <poll.h>
#include <string>
#include <termios.h>
#include <unistd.h>
#include <utility>

namespace
{

	speed_t toTermiosBaud(int baudrate) noexcept
	{
		switch(baudrate)
		{
		case 1200:
			return B1200;
		case 2400:
			return B2400;
		case 4800:
			return B4800;
		case 9600:
			return B9600;
		case 19200:
			return B19200;
		case 38400:
			return B38400;
		case 57600:
			return B57600;
		case 115200:
			return B115200;
#ifdef B230400
		case 230400:
			return B230400;
#endif
#ifdef B460800
		case 460800:
			return B460800;
#endif
#ifdef B500000
		case 500000:
			return B500000;
#endif
#ifdef B576000
		case 576000:
			return B576000;
#endif
#ifdef B921600
		case 921600:
			return B921600;
#endif
#ifdef B1000000
		case 1000000:
			return B1000000;
#endif
#ifdef B1500000
		case 1500000:
			return B1500000;
#endif
#ifdef B2000000
		case 2000000:
			return B2000000;
#endif
#ifdef B2500000
		case 2500000:
			return B2500000;
#endif
#ifdef B3000000
		case 3000000:
			return B3000000;
#endif
		default:
			return static_cast<speed_t>(0);
		}
	}

	std::vector<std::string> splitCommaPreserveEmpty(
	    const std::string& line)
	{
		std::vector<std::string> result;
		std::size_t begin = 0;

		while(true)
		{
			const std::size_t comma = line.find(',', begin);

			if(comma == std::string::npos)
			{
				result.emplace_back(line.substr(begin));
				break;
			}

			result.emplace_back(line.substr(begin, comma - begin));
			begin = comma + 1;
		}

		return result;
	}

	bool endsWith(const std::string& text, const char* suffix) noexcept
	{
		const std::size_t suffix_size = std::strlen(suffix);

		return text.size() >= suffix_size
		    && text.compare(text.size() - suffix_size, suffix_size,
		                    suffix)
		    == 0;
	}

	std::string blockBaseTag(const std::string& tag)
	{
		if(endsWith(tag, "_BEGIN"))
		{
			return tag.substr(0, tag.size() - 6);
		}

		if(endsWith(tag, "_END"))
		{
			return tag.substr(0, tag.size() - 4);
		}

		return {};
	}

	std::string errnoText(int error_number)
	{
		return std::string(std::strerror(error_number))
		    + " (errno=" + std::to_string(error_number) + ")";
	}

} // namespace

namespace etest
{

	bool UartMessage::isCritical() const noexcept
	{
		return type == UartMessageType::OK
		    || type == UartMessageType::ERROR
		    || type == UartMessageType::WARNING
		    || type == UartMessageType::DONE
		    || type == UartMessageType::BOOT
		    || type == UartMessageType::KEY_EVENT;
	}

	UartBlockAssembler::UartBlockAssembler(int timeout_ms) noexcept:
	timeout_ms_(std::max(1, timeout_ms))
	{
	}

	UartBlockResult UartBlockAssembler::consume(
	    const UartMessage& message, UartBlock& output) noexcept
	{
		try
		{
			if(message.type == UartMessageType::BLOCK_BEGIN)
			{
				UartBlockResult result = UartBlockResult::CONSUMED;

				if(active_)
				{
					output.tag = tag_;
					output.seq = seq_;
					output.lines = std::move(lines_);
					output.complete = false;
					output.error =
					    "new block began before previous block ended";

					ETEST_LOG_ERROR("UART_BLOCK",
					                "abort incomplete block: tag="
					                    + tag_ + ", seq=" + seq_);

					result = UartBlockResult::ABORTED;
				}

				active_ = true;
				tag_ = blockBaseTag(message.tag);
				seq_ = message.fields.empty() ? ""
				                              : message.fields.front();
				lines_.clear();
				started_at_ = std::chrono::steady_clock::now();

				return result;
			}

			if(!active_)
			{
				return UartBlockResult::IGNORED;
			}

			// 协议允许异步消息在任意时刻出现。
			if(message.isCritical())
			{
				return UartBlockResult::IGNORED;
			}

			if(message.type == UartMessageType::BLOCK_END)
			{
				const std::string end_tag = blockBaseTag(message.tag);

				const std::string end_seq = message.fields.empty()
				    ? ""
				    : message.fields.front();

				output.tag = tag_;
				output.seq = seq_;
				output.lines = std::move(lines_);

				const bool tag_match = end_tag == tag_;
				const bool seq_match =
				    seq_.empty() || end_seq.empty() || seq_ == end_seq;

				output.complete = tag_match && seq_match;
				output.error.clear();

				if(!tag_match)
				{
					output.error = "block tag mismatch: begin=" + tag_
					    + ", end=" + end_tag;
				}
				else if(!seq_match)
				{
					output.error = "block seq mismatch: begin=" + seq_
					    + ", end=" + end_seq;
				}

				if(!output.complete)
				{
					ETEST_LOG_ERROR("UART_BLOCK", output.error);
				}

				reset();

				return output.complete ? UartBlockResult::COMPLETED
				                       : UartBlockResult::ABORTED;
			}

			if(message.type == UartMessageType::UNKNOWN)
			{
				ETEST_LOG_WARN(
				    "UART_BLOCK",
				    "unknown line ignored in block: " + message.raw);
				return UartBlockResult::IGNORED;
			}

			lines_.push_back(message);
			return UartBlockResult::CONSUMED;
		}
		catch(const std::exception& error)
		{
			ETEST_LOG_ERROR(
			    "UART_BLOCK",
			    std::string("consume exception: ") + error.what());
			reset();
			return UartBlockResult::ABORTED;
		}
		catch(...)
		{
			ETEST_LOG_ERROR("UART_BLOCK", "unknown consume exception");
			reset();
			return UartBlockResult::ABORTED;
		}
	}

	UartBlockResult UartBlockAssembler::checkTimeout(
	    UartBlock& output) noexcept
	{
		try
		{
			if(!active_)
			{
				return UartBlockResult::IGNORED;
			}

			const auto elapsed_ms =
			    std::chrono::duration_cast<std::chrono::milliseconds>(
			        std::chrono::steady_clock::now() - started_at_)
			        .count();

			if(elapsed_ms < timeout_ms_)
			{
				return UartBlockResult::IGNORED;
			}

			output.tag = tag_;
			output.seq = seq_;
			output.lines = std::move(lines_);
			output.complete = false;
			output.error = "block timeout after "
			    + std::to_string(elapsed_ms) + " ms";

			ETEST_LOG_ERROR(
			    "UART_BLOCK",
			    output.error + ", tag=" + tag_ + ", seq=" + seq_);

			reset();
			return UartBlockResult::ABORTED;
		}
		catch(...)
		{
			ETEST_LOG_ERROR("UART_BLOCK", "timeout check exception");
			reset();
			return UartBlockResult::ABORTED;
		}
	}

	void UartBlockAssembler::reset() noexcept
	{
		active_ = false;
		tag_.clear();
		seq_.clear();
		lines_.clear();
		started_at_ = {};
	}

	bool UartBlockAssembler::active() const noexcept
	{
		return active_;
	}

	Uart::Uart(UartConfig config): config_(std::move(config)) {}

	Uart::~Uart()
	{
		stop();
	}

	bool Uart::start() noexcept
	{
		bool expected = false;

		if(!running_.compare_exchange_strong(expected, true))
		{
			return true;
		}

		try
		{
			receive_thread_ = std::thread(&Uart::receiveLoop, this);

			ETEST_LOG_INFO("UART", "receive worker started");

			return true;
		}
		catch(const std::exception& error)
		{
			running_ = false;

			ETEST_LOG_ERROR(
			    "UART",
			    std::string("failed to start receive worker: ")
			        + error.what());

			return false;
		}
		catch(...)
		{
			running_ = false;

			ETEST_LOG_ERROR("UART",
			                "unknown error starting receive worker");

			return false;
		}
	}

	void Uart::stop() noexcept
	{
		running_ = false;
		close();
		queue_cv_.notify_all();

		try
		{
			if(receive_thread_.joinable())
			{
				receive_thread_.join();
			}
		}
		catch(const std::exception& error)
		{
			ETEST_LOG_ERROR(
			    "UART",
			    std::string("worker join failed: ") + error.what());
		}
		catch(...)
		{
			ETEST_LOG_ERROR("UART", "unknown worker join error");
		}
	}

	bool Uart::open() noexcept
	{
		try
		{
			std::lock_guard<std::mutex> lock(fd_mutex_);

			if(fd_ >= 0)
			{
				return true;
			}

			return openUnlocked();
		}
		catch(const std::exception& error)
		{
			ETEST_LOG_ERROR(
			    "UART", std::string("open exception: ") + error.what());
			return false;
		}
		catch(...)
		{
			ETEST_LOG_ERROR("UART", "unknown open exception");
			return false;
		}
	}

	bool Uart::openUnlocked() noexcept
	{
		const speed_t baud = toTermiosBaud(config_.baudrate);

		if(baud == static_cast<speed_t>(0))
		{
			ETEST_LOG_ERROR("UART",
			                "unsupported baudrate: "
			                    + std::to_string(config_.baudrate));

			return false;
		}

		const int opened_fd =
		    ::open(config_.device.c_str(),
		           O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);

		if(opened_fd < 0)
		{
			ETEST_LOG_ERROR("UART",
			                "open " + config_.device
			                    + " failed: " + errnoText(errno));

			return false;
		}

		termios tty{};

		if(::tcgetattr(opened_fd, &tty) != 0)
		{
			const int saved_errno = errno;

			ETEST_LOG_ERROR("UART",
			                "tcgetattr " + config_.device
			                    + " failed: " + errnoText(saved_errno));

			::close(opened_fd);
			return false;
		}

		::cfmakeraw(&tty);

		tty.c_cflag |= CLOCAL | CREAD;
		tty.c_cflag &= ~CSIZE;
		tty.c_cflag |= CS8;
		tty.c_cflag &= ~PARENB;
		tty.c_cflag &= ~CSTOPB;

#ifdef CRTSCTS
		tty.c_cflag &= ~CRTSCTS;
#endif

		// 读超时由 poll() 控制。
		tty.c_cc[VMIN] = 0;
		tty.c_cc[VTIME] = 0;

		if(::cfsetispeed(&tty, baud) != 0
		   || ::cfsetospeed(&tty, baud) != 0)
		{
			const int saved_errno = errno;

			ETEST_LOG_ERROR(
			    "UART",
			    "set baudrate failed: " + errnoText(saved_errno));

			::close(opened_fd);
			return false;
		}

		if(::tcsetattr(opened_fd, TCSANOW, &tty) != 0)
		{
			const int saved_errno = errno;

			ETEST_LOG_ERROR("UART",
			                "tcsetattr " + config_.device
			                    + " failed: " + errnoText(saved_errno));

			::close(opened_fd);
			return false;
		}

		if(::tcflush(opened_fd, TCIOFLUSH) != 0)
		{
			ETEST_LOG_WARN("UART",
			               "tcflush " + config_.device
			                   + " failed: " + errnoText(errno));
		}

		fd_ = opened_fd;
		connected_ = true;
		rx_buffer_.clear();
		discarding_overlong_line_ = false;

		ETEST_LOG_INFO("UART",
		               "opened " + config_.device + " @ "
		                   + std::to_string(config_.baudrate));

		return true;
	}

	void Uart::close() noexcept
	{
		try
		{
			std::lock_guard<std::mutex> lock(fd_mutex_);
			closeUnlocked();
		}
		catch(const std::exception& error)
		{
			ETEST_LOG_ERROR(
			    "UART",
			    std::string("close exception: ") + error.what());
		}
		catch(...)
		{
			ETEST_LOG_ERROR("UART", "unknown close exception");
		}
	}

	void Uart::closeUnlocked() noexcept
	{
		if(fd_ < 0)
		{
			connected_ = false;
			return;
		}

		const int old_fd = fd_;
		fd_ = -1;
		connected_ = false;

		if(::close(old_fd) != 0)
		{
			ETEST_LOG_ERROR("UART",
			                "close " + config_.device
			                    + " failed: " + errnoText(errno));
		}
	}

	bool Uart::send(const std::vector<std::uint8_t>& data) noexcept
	{
		return sendBytes(data.data(), data.size());
	}

	bool Uart::send(const std::string& text) noexcept
	{
		return sendBytes(
		    reinterpret_cast<const std::uint8_t*>(text.data()),
		    text.size());
	}

	bool Uart::sendLine(const std::string& line) noexcept
	{
		try
		{
			if(line.empty())
			{
				ETEST_LOG_ERROR("UART_TX", "refuse empty command");
				return false;
			}

			if(line.find('\r') != std::string::npos
			   || line.find('\n') != std::string::npos)
			{
				ETEST_LOG_ERROR(
				    "UART_TX",
				    "refuse command containing CR/LF: " + line);
				return false;
			}

			const std::string framed = line + "\r\n";

			if(!send(framed))
			{
				return false;
			}

			ETEST_LOG_DEBUG("UART_TX", line);

			return true;
		}
		catch(const std::exception& error)
		{
			ETEST_LOG_ERROR(
			    "UART_TX",
			    std::string("sendLine exception: ") + error.what());
			return false;
		}
		catch(...)
		{
			ETEST_LOG_ERROR("UART_TX", "unknown sendLine exception");
			return false;
		}
	}

	bool Uart::sendBytes(const std::uint8_t* data,
	                     std::size_t size) noexcept
	{
		if(size == 0)
		{
			return true;
		}

		if(data == nullptr)
		{
			ETEST_LOG_ERROR("UART_TX",
			                "null pointer with non-zero size");
			return false;
		}

		try
		{
			std::lock_guard<std::mutex> tx_lock(tx_mutex_);
			std::lock_guard<std::mutex> fd_lock(fd_mutex_);

			if(fd_ < 0)
			{
				ETEST_LOG_ERROR("UART_TX",
				                "send failed: port is not open");
				return false;
			}

			std::size_t total = 0;

			while(total < size)
			{
				pollfd descriptor{};
				descriptor.fd = fd_;
				descriptor.events = POLLOUT;

				const int poll_result =
				    ::poll(&descriptor, 1, config_.write_timeout_ms);

				if(poll_result == 0)
				{
					ETEST_LOG_ERROR(
					    "UART_TX",
					    "write timeout after "
					        + std::to_string(config_.write_timeout_ms)
					        + " ms");
					return false;
				}

				if(poll_result < 0)
				{
					if(errno == EINTR)
					{
						continue;
					}

					ETEST_LOG_ERROR(
					    "UART_TX",
					    "poll(POLLOUT) failed: " + errnoText(errno));

					closeUnlocked();
					return false;
				}

				if((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL))
				   != 0)
				{
					ETEST_LOG_ERROR(
					    "UART_TX",
					    "port error while writing, revents="
					        + std::to_string(descriptor.revents));

					closeUnlocked();
					return false;
				}

				const ssize_t written =
				    ::write(fd_, data + total, size - total);

				if(written > 0)
				{
					total += static_cast<std::size_t>(written);
					continue;
				}

				if(written < 0
				   && (errno == EINTR || errno == EAGAIN
				       || errno == EWOULDBLOCK))
				{
					continue;
				}

				ETEST_LOG_ERROR(
				    "UART_TX",
				    written == 0 ? "write returned 0"
				                 : "write failed: " + errnoText(errno));

				closeUnlocked();
				return false;
			}

			return true;
		}
		catch(const std::exception& error)
		{
			ETEST_LOG_ERROR(
			    "UART_TX",
			    std::string("send exception: ") + error.what());
			return false;
		}
		catch(...)
		{
			ETEST_LOG_ERROR("UART_TX", "unknown send exception");
			return false;
		}
	}

	bool Uart::tryPop(UartMessage& message) noexcept
	{
		try
		{
			std::lock_guard<std::mutex> lock(queue_mutex_);

			if(queue_.empty())
			{
				return false;
			}

			message = std::move(queue_.front());
			queue_.pop_front();
			return true;
		}
		catch(const std::exception& error)
		{
			ETEST_LOG_ERROR(
			    "UART_QUEUE",
			    std::string("tryPop exception: ") + error.what());
			return false;
		}
		catch(...)
		{
			ETEST_LOG_ERROR("UART_QUEUE", "unknown tryPop exception");
			return false;
		}
	}

	bool Uart::waitPop(UartMessage& message, int timeout_ms) noexcept
	{
		try
		{
			std::unique_lock<std::mutex> lock(queue_mutex_);

			const bool ready = queue_cv_.wait_for(
			    lock,
			    std::chrono::milliseconds(std::max(0, timeout_ms)),
			    [this] {
				    return !queue_.empty() || !running_;
			    });

			if(!ready || queue_.empty())
			{
				return false;
			}

			message = std::move(queue_.front());
			queue_.pop_front();
			return true;
		}
		catch(const std::exception& error)
		{
			ETEST_LOG_ERROR(
			    "UART_QUEUE",
			    std::string("waitPop exception: ") + error.what());
			return false;
		}
		catch(...)
		{
			ETEST_LOG_ERROR("UART_QUEUE", "unknown waitPop exception");
			return false;
		}
	}

	std::size_t Uart::pendingCount() const noexcept
	{
		try
		{
			std::lock_guard<std::mutex> lock(queue_mutex_);
			return queue_.size();
		}
		catch(...)
		{
			return 0;
		}
	}

	bool Uart::isOpen() const noexcept
	{
		return connected_.load();
	}

	bool Uart::isRunning() const noexcept
	{
		return running_.load();
	}

	UartMessage Uart::parseLine(const std::string& line) noexcept
	{
		UartMessage message;
		message.raw = line;

		try
		{
			const std::vector<std::string> parts =
			    splitCommaPreserveEmpty(line);

			if(parts.empty() || parts.front().empty())
			{
				message.type = UartMessageType::UNKNOWN;
				return message;
			}

			message.tag = parts.front();
			message.fields.assign(parts.begin() + 1, parts.end());

			if(message.tag == "OK")
			{
				message.type = UartMessageType::OK;
			}
			else if(message.tag == "ERR")
			{
				message.type = UartMessageType::ERROR;
			}
			else if(message.tag == "WARN")
			{
				message.type = UartMessageType::WARNING;
			}
			else if(message.tag == "DONE")
			{
				message.type = UartMessageType::DONE;
			}
			else if(message.tag == "BOOT")
			{
				message.type = UartMessageType::BOOT;
			}
			else if(message.tag == "M0001" || message.tag == "M0002"
			        || message.tag == "M0003" || message.tag == "M0004")
			{
				message.type = UartMessageType::KEY_EVENT;
			}
			else if(endsWith(message.tag, "_BEGIN"))
			{
				message.type = UartMessageType::BLOCK_BEGIN;
			}
			else if(endsWith(message.tag, "_END"))
			{
				message.type = UartMessageType::BLOCK_END;
			}
			else if(parts.size() > 1)
			{
				message.type = UartMessageType::DATA;
			}
			else
			{
				message.type = UartMessageType::UNKNOWN;
			}

			return message;
		}
		catch(const std::exception& error)
		{
			ETEST_LOG_ERROR("UART_PARSE",
			                std::string("parse exception for '") + line
			                    + "': " + error.what());

			message.type = UartMessageType::UNKNOWN;
			return message;
		}
		catch(...)
		{
			ETEST_LOG_ERROR("UART_PARSE",
			                "unknown parse exception for: " + line);

			message.type = UartMessageType::UNKNOWN;
			return message;
		}
	}

	void Uart::receiveLoop() noexcept
	{
		bool first_attempt = true;

		while(running_)
		{
			try
			{
				if(!isOpen())
				{
					if(!first_attempt && !config_.auto_reconnect)
					{
						sleepReconnect();
						continue;
					}

					first_attempt = false;

					if(!open())
					{
						sleepReconnect();
						continue;
					}
				}

				receiveIteration();
			}
			catch(const std::exception& error)
			{
				ETEST_LOG_ERROR("UART_RX",
				                std::string("receive loop exception: ")
				                    + error.what());

				close();
				sleepReconnect();
			}
			catch(...)
			{
				ETEST_LOG_ERROR("UART_RX",
				                "unknown receive loop exception");

				close();
				sleepReconnect();
			}
		}
	}

	void Uart::receiveIteration() noexcept
	{
		int current_fd = -1;

		{
			std::lock_guard<std::mutex> lock(fd_mutex_);
			current_fd = fd_;
		}

		if(current_fd < 0)
		{
			return;
		}

		pollfd descriptor{};
		descriptor.fd = current_fd;
		descriptor.events = POLLIN;

		const int poll_result =
		    ::poll(&descriptor, 1, config_.timeout_ms);

		if(poll_result == 0)
		{
			return;
		}

		if(poll_result < 0)
		{
			if(errno == EINTR)
			{
				return;
			}

			ETEST_LOG_ERROR("UART_RX",
			                "poll(POLLIN) failed: " + errnoText(errno));

			close();
			return;
		}

		if((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
		{
			ETEST_LOG_ERROR("UART_RX",
			                "port disconnected, revents="
			                    + std::to_string(descriptor.revents));

			close();
			return;
		}

		if((descriptor.revents & POLLIN) == 0)
		{
			return;
		}

		char buffer[256];

		while(running_)
		{
			const ssize_t count =
			    ::read(current_fd, buffer, sizeof(buffer));

			if(count > 0)
			{
				processBytes(buffer, static_cast<std::size_t>(count));
				continue;
			}

			if(count == 0)
			{
				// 对配置为 VMIN=0/VTIME=0 的 TTY，0 也可能表示当前无可读字节，
				// 不能直接判定为断线。真正的掉线由 poll() 的 HUP/ERR/NVAL
				// 或后续系统调用错误处理。
				return;
			}

			if(errno == EINTR)
			{
				continue;
			}

			if(errno == EAGAIN || errno == EWOULDBLOCK)
			{
				return;
			}

			ETEST_LOG_ERROR("UART_RX",
			                "read failed: " + errnoText(errno));

			close();
			return;
		}
	}

	void Uart::processBytes(const char* data, std::size_t size) noexcept
	{
		try
		{
			for(std::size_t i = 0; i < size; ++i)
			{
				const char ch = data[i];

				if(ch == '\n')
				{
					if(discarding_overlong_line_)
					{
						discarding_overlong_line_ = false;
						rx_buffer_.clear();
						continue;
					}

					if(!rx_buffer_.empty() && rx_buffer_.back() == '\r')
					{
						rx_buffer_.pop_back();
					}

					if(!rx_buffer_.empty())
					{
						UartMessage message = parseLine(rx_buffer_);

						if(message.type == UartMessageType::UNKNOWN)
						{
							ETEST_LOG_WARN(
							    "UART_RX",
							    "unknown line: " + rx_buffer_);
						}
						else
						{
							ETEST_LOG_DEBUG("UART_RX", rx_buffer_);
						}

						pushMessage(std::move(message));
					}

					rx_buffer_.clear();
					continue;
				}

				if(discarding_overlong_line_)
				{
					continue;
				}

				const std::size_t max_line = static_cast<std::size_t>(
				    std::max(1, config_.max_line_length));

				if(rx_buffer_.size() >= max_line)
				{
					ETEST_LOG_ERROR("UART_RX",
					                "line exceeds max_line_length="
					                    + std::to_string(max_line)
					                    + "; drop until newline");

					rx_buffer_.clear();
					discarding_overlong_line_ = true;
					continue;
				}

				rx_buffer_.push_back(ch);
			}
		}
		catch(const std::exception& error)
		{
			ETEST_LOG_ERROR("UART_RX",
			                std::string("byte processing exception: ")
			                    + error.what());

			rx_buffer_.clear();
			discarding_overlong_line_ = true;
		}
		catch(...)
		{
			ETEST_LOG_ERROR("UART_RX",
			                "unknown byte processing exception");

			rx_buffer_.clear();
			discarding_overlong_line_ = true;
		}
	}

	bool Uart::isCriticalType(UartMessageType type) noexcept
	{
		return type == UartMessageType::OK
		    || type == UartMessageType::ERROR
		    || type == UartMessageType::WARNING
		    || type == UartMessageType::DONE
		    || type == UartMessageType::BOOT
		    || type == UartMessageType::KEY_EVENT;
	}

	void Uart::pushMessage(UartMessage message) noexcept
	{
		try
		{
			std::lock_guard<std::mutex> lock(queue_mutex_);

			const std::size_t capacity = static_cast<std::size_t>(
			    std::max(1, config_.queue_capacity));

			if(queue_.size() >= capacity)
			{
				if(isCriticalType(message.type))
				{
					const auto removable = std::find_if(
					    queue_.begin(), queue_.end(),
					    [](const UartMessage& queued) {
						    return !Uart::isCriticalType(queued.type);
					    });

					if(removable != queue_.end())
					{
						ETEST_LOG_ERROR(
						    "UART_QUEUE",
						    "queue full; drop old non-critical: "
						        + removable->raw);

						queue_.erase(removable);
					}
					else
					{
						ETEST_LOG_ERROR(
						    "UART_QUEUE",
						    "queue full of critical messages; "
						    "drop oldest: "
						        + queue_.front().raw);

						queue_.pop_front();
					}
				}
				else
				{
					ETEST_LOG_ERROR(
					    "UART_QUEUE",
					    "queue full; drop new non-critical: "
					        + message.raw);
					return;
				}
			}

			queue_.push_back(std::move(message));
			queue_cv_.notify_one();
		}
		catch(const std::exception& error)
		{
			ETEST_LOG_ERROR(
			    "UART_QUEUE",
			    std::string("push exception: ") + error.what());
		}
		catch(...)
		{
			ETEST_LOG_ERROR("UART_QUEUE", "unknown push exception");
		}
	}

	void Uart::sleepReconnect() const noexcept
	{
		const int total_ms =
		    std::max(50, config_.reconnect_interval_ms);

		int slept_ms = 0;

		while(running_ && slept_ms < total_ms)
		{
			constexpr int slice_ms = 50;

			std::this_thread::sleep_for(
			    std::chrono::milliseconds(slice_ms));

			slept_ms += slice_ms;
		}
	}

} // namespace etest