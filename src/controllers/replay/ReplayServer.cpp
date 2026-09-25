// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
// SPDX-License-Identifier: MIT
#include "controllers/replay/ReplayServer.hpp"

#include "common/QLogging.hpp"
#include "util/PostToThread.hpp"

#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/buffers_to_string.hpp>
#include <boost/beast/websocket.hpp>

namespace chatterino {
namespace {
namespace asio = boost::asio;
namespace beast = boost::beast;
using tcp = asio::ip::tcp;

class Session : public std::enable_shared_from_this<Session>
{
public:
    Session(tcp::socket socket, std::uint64_t id,
            ReplayServer::Handler handler)
        : ws_(std::move(socket))
        , id_(id)
        , handler_(std::move(handler))
    {
    }

    void start()
    {
        auto self = this->shared_from_this();
        this->ws_.async_accept([self](boost::system::error_code ec) {
            if (!ec)
            {
                self->read();
            }
        });
    }

private:
    void read()
    {
        auto self = this->shared_from_this();
        this->ws_.async_read(this->buffer_,
                             [self](boost::system::error_code ec, std::size_t) {
                                 if (ec)
                                 {
                                     self->handler_(self->id_, {});
                                     return;
                                 }
                                 if (self->ws_.got_text())
                                 {
                                     auto data = beast::buffers_to_string(
                                         self->buffer_.data());
                                     self->handler_(self->id_, data);
                                 }
                                 self->buffer_.consume(self->buffer_.size());
                                 self->read();
                             });
    }

    beast::websocket::stream<tcp::socket> ws_;
    beast::flat_buffer buffer_;
    std::uint64_t id_;
    ReplayServer::Handler handler_;
};
}  // namespace

ReplayServer::ReplayServer(int port, Handler handler)
    : handler_(std::move(handler))
{
    boost::system::error_code ec;
    this->acceptor_.open(tcp::v4(), ec);
    if (!ec)
    {
        this->acceptor_.set_option(asio::socket_base::reuse_address(true), ec);
    }
    if (!ec)
    {
        this->acceptor_.bind(
            tcp::endpoint(asio::ip::address_v4::loopback(),
                          static_cast<unsigned short>(port)),
            ec);
    }
    if (!ec)
    {
        this->acceptor_.listen(asio::socket_base::max_listen_connections,
                               ec);
    }
    if (ec)
    {
        qCWarning(chatterinoWebsocket) << "Replay server could not listen on"
                                       << port << ":" << ec.message().c_str();
        return;
    }
    this->accept();
    this->thread_ = std::thread([this] { this->io_.run(); });
}

ReplayServer::~ReplayServer()
{
    this->io_.stop();
    if (this->thread_.joinable())
    {
        this->thread_.join();
    }
}

void ReplayServer::accept()
{
    this->acceptor_.async_accept([this](boost::system::error_code ec,
                                        tcp::socket socket) {
        if (!ec)
        {
            std::make_shared<Session>(std::move(socket), this->nextConnection_++,
                                      this->handler_)
                ->start();
        }
        if (this->acceptor_.is_open())
        {
            this->accept();
        }
    });
}
}  // namespace chatterino
