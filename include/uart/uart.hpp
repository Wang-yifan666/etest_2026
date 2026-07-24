#pragma once

#include "core/config.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace etest
{

class Uart
{
public:
    explicit Uart(UartConfig config = {});
    ~Uart();

    bool open() noexcept;
    void close() noexcept;

    bool send(
        const std::vector<std::uint8_t>& data) noexcept;

    bool send(
        const std::string& text) noexcept;

    std::vector<std::uint8_t> receive(
        std::size_t max_len = 256) noexcept;

    bool isOpen() const noexcept;

    Uart(const Uart&) = delete;
    Uart& operator=(const Uart&) = delete;

private:
    UartConfig config_;
    int fd_ = -1;
};

} // namespace etest
