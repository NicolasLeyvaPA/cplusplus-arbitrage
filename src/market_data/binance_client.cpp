#include "market_data/binance_client.hpp"
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <cstring>
#include <random>

namespace arb {

namespace {
    // WebSocket frame helpers
    std::string create_ws_handshake(const std::string& host, const std::string& path) {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(0, 255);

        std::string key;
        for (int i = 0; i < 16; i++) {
            key += static_cast<char>(dis(gen));
        }

        // Base64 encode the key (simplified)
        static const char* b64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string encoded_key;
        for (size_t i = 0; i < key.size(); i += 3) {
            uint32_t n = static_cast<uint8_t>(key[i]) << 16;
            if (i + 1 < key.size()) n |= static_cast<uint8_t>(key[i + 1]) << 8;
            if (i + 2 < key.size()) n |= static_cast<uint8_t>(key[i + 2]);
            encoded_key += b64[(n >> 18) & 0x3F];
            encoded_key += b64[(n >> 12) & 0x3F];
            encoded_key += (i + 1 < key.size()) ? b64[(n >> 6) & 0x3F] : '=';
            encoded_key += (i + 2 < key.size()) ? b64[n & 0x3F] : '=';
        }

        std::string request = "GET " + path + " HTTP/1.1\r\n";
        request += "Host: " + host + "\r\n";
        request += "Upgrade: websocket\r\n";
        request += "Connection: Upgrade\r\n";
        request += "Sec-WebSocket-Key: " + encoded_key + "\r\n";
        request += "Sec-WebSocket-Version: 13\r\n";
        request += "\r\n";
        return request;
    }

    std::string create_ws_frame(const std::string& data, uint8_t opcode = 0x01) {
        std::string frame;
        frame += static_cast<char>(0x80 | opcode);  // FIN + opcode

        size_t len = data.size();
        if (len < 126) {
            frame += static_cast<char>(0x80 | len);  // Masked + length
        } else if (len < 65536) {
            frame += static_cast<char>(0x80 | 126);
            frame += static_cast<char>((len >> 8) & 0xFF);
            frame += static_cast<char>(len & 0xFF);
        } else {
            frame += static_cast<char>(0x80 | 127);
            for (int i = 7; i >= 0; i--) {
                frame += static_cast<char>((len >> (8 * i)) & 0xFF);
            }
        }

        // Masking key
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(0, 255);
        uint8_t mask[4];
        for (int i = 0; i < 4; i++) {
            mask[i] = static_cast<uint8_t>(dis(gen));
            frame += static_cast<char>(mask[i]);
        }

        // Masked data
        for (size_t i = 0; i < data.size(); i++) {
            frame += static_cast<char>(data[i] ^ mask[i % 4]);
        }

        return frame;
    }
}

BinanceClient::BinanceClient(const ConnectionConfig& config)
    : config_(config)
{
    // Build WebSocket URL for bookTicker stream
    ws_url_ = config_.binance_ws_url + "/" + config_.binance_symbol + "@bookTicker";
    spdlog::info("BinanceClient initialized with URL: {}", ws_url_);
}

BinanceClient::~BinanceClient() {
    disconnect();
}

void BinanceClient::connect() {
    if (running_.load()) {
        spdlog::warn("BinanceClient already running");
        return;
    }

    running_ = true;
    status_ = ConnectionStatus::CONNECTING;
    if (on_status_) on_status_(status_.load());

    recv_thread_ = std::thread(&BinanceClient::run_connection_loop, this);
}

void BinanceClient::disconnect() {
    running_ = false;
    if (recv_thread_.joinable()) {
        recv_thread_.join();
    }
    disconnect_socket();
    status_ = ConnectionStatus::DISCONNECTED;
    if (on_status_) on_status_(status_.load());
}

bool BinanceClient::connect_socket() {
    // Parse URL
    std::string host = "stream.binance.com";
    int port = 9443;
    std::string path = "/ws/" + config_.binance_symbol + "@bookTicker";

    // Create socket
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        spdlog::error("Failed to create socket: {}", strerror(errno));
        return false;
    }

    // Resolve hostname
    struct hostent* server = gethostbyname(host.c_str());
    if (!server) {
        spdlog::error("Failed to resolve host: {}", host);
        close(sock);
        return false;
    }

    // Connect
    struct sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    memcpy(&server_addr.sin_addr.s_addr, server->h_addr, server->h_length);

    if (::connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        spdlog::error("Failed to connect: {}", strerror(errno));
        close(sock);
        return false;
    }

    // Initialize SSL
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();

    ssl_ctx_ = SSL_CTX_new(TLS_client_method());
    if (!ssl_ctx_) {
        spdlog::error("Failed to create SSL context");
        close(sock);
        return false;
    }

    ssl_ = SSL_new(static_cast<SSL_CTX*>(ssl_ctx_));
    SSL_set_fd(static_cast<SSL*>(ssl_), sock);

    if (SSL_connect(static_cast<SSL*>(ssl_)) <= 0) {
        spdlog::error("SSL handshake failed");
        SSL_free(static_cast<SSL*>(ssl_));
        SSL_CTX_free(static_cast<SSL_CTX*>(ssl_ctx_));
        close(sock);
        ssl_ = nullptr;
        ssl_ctx_ = nullptr;
        return false;
    }

    socket_ = reinterpret_cast<void*>(static_cast<intptr_t>(sock));

    // Send WebSocket handshake
    std::string handshake = create_ws_handshake(host, path);
    if (SSL_write(static_cast<SSL*>(ssl_), handshake.c_str(), handshake.size()) <= 0) {
        spdlog::error("Failed to send WebSocket handshake");
        disconnect_socket();
        return false;
    }

    // Read handshake response
    char buffer[4096];
    int bytes = SSL_read(static_cast<SSL*>(ssl_), buffer, sizeof(buffer) - 1);
    if (bytes <= 0) {
        spdlog::error("Failed to receive WebSocket handshake response");
        disconnect_socket();
        return false;
    }
    buffer[bytes] = '\0';

    // Check for 101 Switching Protocols
    if (strstr(buffer, "101") == nullptr) {
        spdlog::error("WebSocket handshake failed: {}", buffer);
        disconnect_socket();
        return false;
    }

    spdlog::info("Binance WebSocket connected");
    return true;
}

void BinanceClient::disconnect_socket() {
    if (ssl_) {
        SSL_shutdown(static_cast<SSL*>(ssl_));
        SSL_free(static_cast<SSL*>(ssl_));
        ssl_ = nullptr;
    }
    if (ssl_ctx_) {
        SSL_CTX_free(static_cast<SSL_CTX*>(ssl_ctx_));
        ssl_ctx_ = nullptr;
    }
    if (socket_) {
        close(static_cast<int>(reinterpret_cast<intptr_t>(socket_)));
        socket_ = nullptr;
    }
}

std::string BinanceClient::recv_frame() {
    if (!ssl_) return "";

    SSL* ssl = static_cast<SSL*>(ssl_);

    // Read frame header (2 bytes minimum)
    uint8_t header[2];
    int bytes = SSL_read(ssl, header, 2);
    if (bytes < 2) return "";

    bool fin = (header[0] & 0x80) != 0;
    uint8_t opcode = header[0] & 0x0F;
    bool masked = (header[1] & 0x80) != 0;
    uint64_t payload_len = header[1] & 0x7F;

    // Extended payload length
    if (payload_len == 126) {
        uint8_t ext[2];
        if (SSL_read(ssl, ext, 2) < 2) return "";
        payload_len = (ext[0] << 8) | ext[1];
    } else if (payload_len == 127) {
        uint8_t ext[8];
        if (SSL_read(ssl, ext, 8) < 8) return "";
        payload_len = 0;
        for (int i = 0; i < 8; i++) {
            payload_len = (payload_len << 8) | ext[i];
        }
    }

    // Masking key (if present)
    uint8_t mask[4] = {0};
    if (masked) {
        if (SSL_read(ssl, mask, 4) < 4) return "";
    }

    // Read payload
    if (payload_len > 1024 * 1024) {
        spdlog::error("Frame too large: {}", payload_len);
        return "";
    }

    std::string payload(payload_len, '\0');
    size_t read_total = 0;
    while (read_total < payload_len) {
        int chunk = SSL_read(ssl, &payload[read_total], payload_len - read_total);
        if (chunk <= 0) break;
        read_total += chunk;
    }

    // Unmask if needed
    if (masked) {
        for (size_t i = 0; i < payload.size(); i++) {
            payload[i] ^= mask[i % 4];
        }
    }

    // Handle control frames
    if (opcode == 0x08) {
        // Close frame
        spdlog::info("Received WebSocket close frame");
        return "";
    } else if (opcode == 0x09) {
        // Ping frame - send pong
        send_pong(payload);
        return recv_frame();  // Continue reading
    } else if (opcode == 0x0A) {
        // Pong frame - ignore
        return recv_frame();
    }

    return payload;
}

void BinanceClient::send_pong(const std::string& payload) {
    if (!ssl_) return;
    std::string frame = create_ws_frame(payload, 0x0A);  // Pong opcode
    SSL_write(static_cast<SSL*>(ssl_), frame.c_str(), frame.size());
}

bool BinanceClient::send_raw(const std::string& data) {
    if (!ssl_) return false;
    std::string frame = create_ws_frame(data);
    return SSL_write(static_cast<SSL*>(ssl_), frame.c_str(), frame.size()) > 0;
}

void BinanceClient::run_connection_loop() {
    int reconnect_attempts = 0;

    while (running_.load()) {
        if (!connect_socket()) {
            status_ = ConnectionStatus::RECONNECTING;
            if (on_status_) on_status_(status_.load());

            reconnect_attempts++;
            if (reconnect_attempts > config_.max_reconnect_attempts) {
                spdlog::error("Max reconnect attempts reached");
                status_ = ConnectionStatus::ERROR;
                if (on_status_) on_status_(status_.load());
                if (on_error_) on_error_("Max reconnect attempts reached");
                break;
            }

            int delay = config_.reconnect_delay_ms * (1 << std::min(reconnect_attempts - 1, 5));
            spdlog::info("Reconnecting in {}ms (attempt {})", delay, reconnect_attempts);
            std::this_thread::sleep_for(std::chrono::milliseconds(delay));
            continue;
        }

        status_ = ConnectionStatus::CONNECTED;
        if (on_status_) on_status_(status_.load());
        reconnect_attempts = 0;

        // Main receive loop
        while (running_.load() && ssl_) {
            std::string msg = recv_frame();
            if (msg.empty()) {
                if (!running_.load()) break;
                spdlog::warn("Empty frame received, reconnecting");
                break;
            }

            Timestamp recv_time = now();
            messages_received_++;
            {
                std::lock_guard<std::mutex> lock(price_mutex_);
                last_update_time_ = recv_time;
            }

            // Parse the message
            parse_book_ticker(msg, recv_time);
        }

        disconnect_socket();
        if (running_.load()) {
            status_ = ConnectionStatus::RECONNECTING;
            if (on_status_) on_status_(status_.load());
        }
    }
}

void BinanceClient::parse_book_ticker(const std::string& msg, Timestamp recv_time) {
    try {
        auto j = nlohmann::json::parse(msg);

        // bookTicker format:
        // {"u":12345,"s":"BTCUSDT","b":"42000.00","B":"1.5","a":"42001.00","A":"2.0"}
        if (!j.contains("b") || !j.contains("a")) {
            return;
        }

        BtcPrice price;
        price.bid = std::stod(j["b"].get<std::string>());
        price.ask = std::stod(j["a"].get<std::string>());
        price.mid = (price.bid + price.ask) / 2.0;
        price.timestamp = recv_time;

        if (j.contains("E")) {
            price.exchange_time_ms = j["E"].get<int64_t>();
        }

        {
            std::lock_guard<std::mutex> lock(price_mutex_);
            price.last = current_price_.last;  // Preserve last trade price
            current_price_ = price;
        }

        if (on_price_) {
            on_price_(price);
        }

    } catch (const std::exception& e) {
        spdlog::debug("Failed to parse bookTicker: {} - {}", e.what(), msg.substr(0, 100));
    }
}

void BinanceClient::parse_trade(const std::string& msg, Timestamp recv_time) {
    try {
        auto j = nlohmann::json::parse(msg);

        if (!j.contains("p")) {
            return;
        }

        Price last_price = std::stod(j["p"].get<std::string>());

        {
            std::lock_guard<std::mutex> lock(price_mutex_);
            current_price_.last = last_price;
            current_price_.timestamp = recv_time;
        }

    } catch (const std::exception& e) {
        spdlog::debug("Failed to parse trade: {}", e.what());
    }
}

BtcPrice BinanceClient::current_price() const {
    std::lock_guard<std::mutex> lock(price_mutex_);
    return current_price_;
}

Timestamp BinanceClient::last_update_time() const {
    std::lock_guard<std::mutex> lock(price_mutex_);
    return current_price_.timestamp;
}

} // namespace arb
