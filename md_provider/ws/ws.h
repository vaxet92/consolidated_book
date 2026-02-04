#pragma once

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <functional>
#include <memory>
#include <string>
#include <iostream>

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = boost::asio::ssl;
using tcp = boost::asio::ip::tcp;

#define WS_TIMER_RATE 30

class WebSocketSessionSSL : public std::enable_shared_from_this<WebSocketSessionSSL> {
public:
    explicit WebSocketSessionSSL(net::io_context& ioc, ssl::context& ctx,
                                 std::function<void(const std::string&)> onMessageCallback);
    ~WebSocketSessionSSL() = default;
    
    void Run(const char* inHost, const char* inPort, const char* inTarget);
    void Stop();

private:
    void OnResolve(beast::error_code ec, tcp::resolver::results_type results);
    void OnConnect(beast::error_code ec, tcp::resolver::results_type::endpoint_type ep);
    void OnSslHandshake(beast::error_code ec);
    void OnHandshake(beast::error_code ec);
    void OnRead(beast::error_code ec, std::size_t bytes_transferred);
    void OnClose(beast::error_code ec);

    tcp::resolver resolver;
    websocket::stream<beast::ssl_stream<beast::tcp_stream>> ws;
    beast::flat_buffer buffer;
    std::string host;
    std::string target;
    std::function<void(const std::string&)> onMessageCallback;
    bool stopped;
};

using WebSocketSessionSSLPtr = std::shared_ptr<WebSocketSessionSSL>;
