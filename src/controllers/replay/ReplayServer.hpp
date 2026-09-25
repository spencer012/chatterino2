// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
// SPDX-License-Identifier: MIT
#pragma once

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <functional>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

namespace chatterino {

class ReplayServer
{
public:
    using Handler = std::function<void(std::uint64_t, const std::string &)>;
    ReplayServer(int port, Handler handler);
    ~ReplayServer();
    ReplayServer(const ReplayServer &) = delete;
    ReplayServer &operator=(const ReplayServer &) = delete;

private:
    void accept();
    boost::asio::io_context io_{1};
    boost::asio::ip::tcp::acceptor acceptor_{io_};
    Handler handler_;
    std::thread thread_;
    std::uint64_t nextConnection_ = 1;
};

}  // namespace chatterino
