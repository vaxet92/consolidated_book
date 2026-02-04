#include "ws.h"
#include "root_certificates.hpp"

static void fail(beast::error_code ec, const char* what) {
    std::cerr << "[WebSocket Error] " << what << ": " << ec.message() << "\n";
}

WebSocketSessionSSL::WebSocketSessionSSL(net::io_context& ioc, ssl::context& ctx,
                                         std::function<void(const std::string&)> onMessageCallback)
    : resolver(net::make_strand(ioc)),
      ws(net::make_strand(ioc), ctx),
      onMessageCallback(std::move(onMessageCallback)),
      stopped(false) {}

void WebSocketSessionSSL::Run(const char* inHost, const char* inPort, const char* inTarget) {
    if (stopped) return;
    
    host = inHost;
    target = inTarget;

    resolver.async_resolve(
        inHost, inPort,
        beast::bind_front_handler(&WebSocketSessionSSL::OnResolve, shared_from_this()));
}

void WebSocketSessionSSL::OnResolve(beast::error_code ec, tcp::resolver::results_type results) {
    if (ec) return fail(ec, "resolve");
    if (stopped) return;

    beast::get_lowest_layer(ws).expires_after(std::chrono::seconds(WS_TIMER_RATE));

    beast::get_lowest_layer(ws).async_connect(
        results,
        beast::bind_front_handler(&WebSocketSessionSSL::OnConnect, shared_from_this()));
}

void WebSocketSessionSSL::OnConnect(beast::error_code ec, tcp::resolver::results_type::endpoint_type ep) {
    if (ec) return fail(ec, "connect");
    if (stopped) return;

    beast::get_lowest_layer(ws).expires_after(std::chrono::seconds(WS_TIMER_RATE));

    // Set SNI hostname
    if (!SSL_set_tlsext_host_name(ws.next_layer().native_handle(), host.c_str())) {
        ec = beast::error_code(static_cast<int>(::ERR_get_error()), net::error::get_ssl_category());
        return fail(ec, "SSL SNI");
    }

    // Update host with port for handshake
    std::string host_with_port = host + ':' + std::to_string(ep.port());

    ws.next_layer().async_handshake(
        ssl::stream_base::client,
        beast::bind_front_handler(&WebSocketSessionSSL::OnSslHandshake, shared_from_this()));
}

void WebSocketSessionSSL::OnSslHandshake(beast::error_code ec) {
    if (ec) return fail(ec, "ssl_handshake");
    if (stopped) return;

    beast::get_lowest_layer(ws).expires_never();

    ws.set_option(websocket::stream_base::timeout::suggested(beast::role_type::client));
    ws.set_option(websocket::stream_base::decorator([](websocket::request_type& req) {
        req.set(http::field::user_agent, std::string(BOOST_BEAST_VERSION_STRING) + " consolidated-candles");
    }));

    ws.async_handshake(
        host, target,
        beast::bind_front_handler(&WebSocketSessionSSL::OnHandshake, shared_from_this()));
}

void WebSocketSessionSSL::OnHandshake(beast::error_code ec) {
    if (ec) return fail(ec, "handshake");
    if (stopped) return;

    std::cout << "[WebSocket] Connected to " << host << target << std::endl;

    ws.async_read(
        buffer,
        beast::bind_front_handler(&WebSocketSessionSSL::OnRead, shared_from_this()));
}

void WebSocketSessionSSL::OnRead(beast::error_code ec, std::size_t bytes_transferred) {
    if (ec == websocket::error::closed) {
        std::cout << "[WebSocket] Connection closed\n";
        return;
    }
    if (ec) return fail(ec, "read");
    if (stopped) return;

    if (bytes_transferred > 0 && onMessageCallback) {
        std::string message = beast::buffers_to_string(buffer.data());
        buffer.consume(bytes_transferred);
        
        // Call the message callback
        onMessageCallback(message);
    }

    // Continue reading
    ws.async_read(
        buffer,
        beast::bind_front_handler(&WebSocketSessionSSL::OnRead, shared_from_this()));
}

void WebSocketSessionSSL::Stop() {
    stopped = true;
    beast::error_code ec;
    ws.close(websocket::close_code::normal, ec);
    if (ec) {
        fail(ec, "close");
    }
}

void WebSocketSessionSSL::OnClose(beast::error_code ec) {
    if (ec) return fail(ec, "close");
    std::cout << "[WebSocket] Closed successfully\n";
}
