#include "market_data/polymarket_client.hpp"
#include "utils/crypto.hpp"
#include "utils/time_utils.hpp"
#include <spdlog/spdlog.h>
#include <curl/curl.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <cstring>
#include <random>
#include <regex>

namespace arb {

namespace {
    // CURL write callback
    size_t write_callback(void* contents, size_t size, size_t nmemb, std::string* output) {
        size_t total_size = size * nmemb;
        output->append(static_cast<char*>(contents), total_size);
        return total_size;
    }

    // WebSocket frame creation (same as Binance client)
    std::string create_ws_handshake(const std::string& host, const std::string& path) {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(0, 255);

        std::string key;
        for (int i = 0; i < 16; i++) {
            key += static_cast<char>(dis(gen));
        }

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
        frame += static_cast<char>(0x80 | opcode);

        size_t len = data.size();
        if (len < 126) {
            frame += static_cast<char>(0x80 | len);
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

        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(0, 255);
        uint8_t mask[4];
        for (int i = 0; i < 4; i++) {
            mask[i] = static_cast<uint8_t>(dis(gen));
            frame += static_cast<char>(mask[i]);
        }

        for (size_t i = 0; i < data.size(); i++) {
            frame += static_cast<char>(data[i] ^ mask[i % 4]);
        }

        return frame;
    }
}

PolymarketClient::PolymarketClient(const ConnectionConfig& config)
    : config_(config)
{
    curl_global_init(CURL_GLOBAL_ALL);
    spdlog::info("PolymarketClient initialized");
}

PolymarketClient::~PolymarketClient() {
    disconnect();
    curl_global_cleanup();
}

std::string PolymarketClient::http_get(const std::string& url) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        throw std::runtime_error("Failed to initialize CURL");
    }

    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    // Add headers
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Accept: application/json");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    CURLcode res = curl_easy_perform(curl);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        throw std::runtime_error(std::string("CURL request failed: ") + curl_easy_strerror(res));
    }

    return response;
}

std::string PolymarketClient::http_post(const std::string& url, const std::string& body) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        throw std::runtime_error("Failed to initialize CURL");
    }

    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: application/json");

    // Add L2 authentication headers if we have credentials
    if (!api_key_.empty()) {
        std::string timestamp = std::to_string(time_utils::epoch_ms());
        std::string signature = generate_l2_signature(timestamp, "POST", url, body);

        headers = curl_slist_append(headers, ("POLY_API_KEY: " + api_key_).c_str());
        headers = curl_slist_append(headers, ("POLY_TIMESTAMP: " + timestamp).c_str());
        headers = curl_slist_append(headers, ("POLY_SIGNATURE: " + signature).c_str());
        headers = curl_slist_append(headers, ("POLY_PASSPHRASE: " + api_passphrase_).c_str());
    }

    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    CURLcode res = curl_easy_perform(curl);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        throw std::runtime_error(std::string("CURL POST failed: ") + curl_easy_strerror(res));
    }

    return response;
}

std::string PolymarketClient::generate_l2_signature(const std::string& timestamp,
                                                     const std::string& method,
                                                     const std::string& path,
                                                     const std::string& body) {
    // Extract path from URL
    std::string request_path = path;
    auto pos = path.find("://");
    if (pos != std::string::npos) {
        auto slash_pos = path.find('/', pos + 3);
        if (slash_pos != std::string::npos) {
            request_path = path.substr(slash_pos);
        }
    }

    std::string message = timestamp + method + request_path + body;
    auto decoded = crypto::base64_decode(api_secret_);
    std::string key(decoded.begin(), decoded.end());
    return crypto::hmac_sha256(key, message);
}

std::vector<Market> PolymarketClient::fetch_markets() {
    std::vector<Market> markets;

    try {
        // Use Gamma API to get markets
        std::string url = config_.polymarket_gamma_url + "/markets?closed=false&active=true";
        std::string response = http_get(url);

        auto j = nlohmann::json::parse(response);

        if (!j.is_array()) {
            spdlog::error("Unexpected markets response format");
            return markets;
        }

        for (const auto& item : j) {
            Market market;
            market.condition_id = item.value("conditionId", "");
            market.question = item.value("question", "");
            market.slug = item.value("slug", "");
            market.active = item.value("active", true);

            if (item.contains("tokens") && item["tokens"].is_array()) {
                for (const auto& token : item["tokens"]) {
                    std::string outcome = token.value("outcome", "");
                    std::string token_id = token.value("token_id", "");

                    if (outcome == "Yes") {
                        market.yes_outcome.token_id = token_id;
                        market.yes_outcome.name = "YES";
                    } else if (outcome == "No") {
                        market.no_outcome.token_id = token_id;
                        market.no_outcome.name = "NO";
                    }
                }
            }

            if (!market.condition_id.empty() &&
                !market.yes_outcome.token_id.empty() &&
                !market.no_outcome.token_id.empty()) {
                markets.push_back(market);
            }
        }

        spdlog::info("Fetched {} markets from Polymarket", markets.size());

    } catch (const std::exception& e) {
        spdlog::error("Failed to fetch markets: {}", e.what());
    }

    return markets;
}

std::vector<Market> PolymarketClient::fetch_filtered_markets(const std::string& pattern) {
    auto all_markets = fetch_markets();

    // If pattern is empty, return ALL active markets (for maximum opportunity discovery)
    if (pattern.empty()) {
        spdlog::info("No filter pattern - returning all {} active markets for S2 strategy", all_markets.size());
        for (const auto& market : all_markets) {
            spdlog::debug("Available market: {}", market.question);
        }
        return all_markets;
    }

    std::vector<Market> filtered_markets;

    try {
        // Note: std::regex::icase handles case insensitivity
        std::regex filter_pattern(pattern, std::regex::icase);

        for (const auto& market : all_markets) {
            if (std::regex_search(market.question, filter_pattern) ||
                std::regex_search(market.slug, filter_pattern)) {
                filtered_markets.push_back(market);
                spdlog::info("Found matching market: {}", market.question);
            }
        }
    } catch (const std::regex_error& e) {
        spdlog::error("Invalid regex pattern '{}': {}", pattern, e.what());
        return all_markets;  // Fallback to all markets on regex error
    }

    spdlog::info("Filter '{}' matched {} of {} total markets",
                 pattern, filtered_markets.size(), all_markets.size());
    return filtered_markets;
}

void PolymarketClient::fetch_order_book(const std::string& token_id, OrderBook& book) {
    try {
        std::string url = config_.polymarket_rest_url + "/book?token_id=" + token_id;
        std::string response = http_get(url);

        auto j = nlohmann::json::parse(response);

        std::vector<PriceLevel> bids, asks;

        if (j.contains("bids") && j["bids"].is_array()) {
            for (const auto& bid : j["bids"]) {
                PriceLevel level;
                level.price = std::stod(bid.value("price", "0"));
                level.size = std::stod(bid.value("size", "0"));
                if (level.price > 0 && level.size > 0) {
                    bids.push_back(level);
                }
            }
        }

        if (j.contains("asks") && j["asks"].is_array()) {
            for (const auto& ask : j["asks"]) {
                PriceLevel level;
                level.price = std::stod(ask.value("price", "0"));
                level.size = std::stod(ask.value("size", "0"));
                if (level.price > 0 && level.size > 0) {
                    asks.push_back(level);
                }
            }
        }

        book.apply_snapshot(bids, asks);

    } catch (const std::exception& e) {
        spdlog::error("Failed to fetch order book for {}: {}", token_id, e.what());
    }
}

void PolymarketClient::connect() {
    if (running_.load()) {
        spdlog::warn("PolymarketClient already running");
        return;
    }

    running_ = true;
    status_ = ConnectionStatus::CONNECTING;
    if (on_status_) on_status_(status_.load());

    recv_thread_ = std::thread(&PolymarketClient::run_connection_loop, this);
}

void PolymarketClient::disconnect() {
    running_ = false;
    if (recv_thread_.joinable()) {
        recv_thread_.join();
    }
    disconnect_socket();
    status_ = ConnectionStatus::DISCONNECTED;
    if (on_status_) on_status_(status_.load());
}

bool PolymarketClient::connect_socket() {
    std::string host = "ws-subscriptions-clob.polymarket.com";
    int port = 443;
    std::string path = "/ws/market";

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        spdlog::error("Failed to create socket: {}", strerror(errno));
        return false;
    }

    struct hostent* server = gethostbyname(host.c_str());
    if (!server) {
        spdlog::error("Failed to resolve host: {}", host);
        close(sock);
        return false;
    }

    struct sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    memcpy(&server_addr.sin_addr.s_addr, server->h_addr, server->h_length);

    if (::connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        spdlog::error("Failed to connect: {}", strerror(errno));
        close(sock);
        return false;
    }

    SSL_library_init();
    SSL_load_error_strings();

    ssl_ctx_ = SSL_CTX_new(TLS_client_method());
    if (!ssl_ctx_) {
        spdlog::error("Failed to create SSL context");
        close(sock);
        return false;
    }

    ssl_ = SSL_new(static_cast<SSL_CTX*>(ssl_ctx_));
    SSL_set_fd(static_cast<SSL*>(ssl_), sock);
    SSL_set_tlsext_host_name(static_cast<SSL*>(ssl_), host.c_str());

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

    std::string handshake = create_ws_handshake(host, path);
    if (SSL_write(static_cast<SSL*>(ssl_), handshake.c_str(), handshake.size()) <= 0) {
        spdlog::error("Failed to send WebSocket handshake");
        disconnect_socket();
        return false;
    }

    char buffer[4096];
    int bytes = SSL_read(static_cast<SSL*>(ssl_), buffer, sizeof(buffer) - 1);
    if (bytes <= 0) {
        spdlog::error("Failed to receive WebSocket handshake response");
        disconnect_socket();
        return false;
    }
    buffer[bytes] = '\0';

    if (strstr(buffer, "101") == nullptr) {
        spdlog::error("WebSocket handshake failed: {}", buffer);
        disconnect_socket();
        return false;
    }

    spdlog::info("Polymarket WebSocket connected");
    return true;
}

void PolymarketClient::disconnect_socket() {
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

std::string PolymarketClient::recv_frame() {
    if (!ssl_) return "";

    SSL* ssl = static_cast<SSL*>(ssl_);

    uint8_t header[2];
    int bytes = SSL_read(ssl, header, 2);
    if (bytes < 2) return "";

    uint8_t opcode = header[0] & 0x0F;
    bool masked = (header[1] & 0x80) != 0;
    uint64_t payload_len = header[1] & 0x7F;

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

    uint8_t mask[4] = {0};
    if (masked) {
        if (SSL_read(ssl, mask, 4) < 4) return "";
    }

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

    if (masked) {
        for (size_t i = 0; i < payload.size(); i++) {
            payload[i] ^= mask[i % 4];
        }
    }

    if (opcode == 0x08) {
        spdlog::info("Received WebSocket close frame");
        return "";
    } else if (opcode == 0x09) {
        send_pong(payload);
        return recv_frame();
    } else if (opcode == 0x0A) {
        return recv_frame();
    }

    return payload;
}

void PolymarketClient::send_pong(const std::string& payload) {
    if (!ssl_) return;
    std::string frame = create_ws_frame(payload, 0x0A);
    SSL_write(static_cast<SSL*>(ssl_), frame.c_str(), frame.size());
}

bool PolymarketClient::send_raw(const std::string& data) {
    if (!ssl_) return false;
    std::string frame = create_ws_frame(data);
    return SSL_write(static_cast<SSL*>(ssl_), frame.c_str(), frame.size()) > 0;
}

void PolymarketClient::subscribe_market(const std::string& token_id) {
    nlohmann::json sub_msg = {
        {"type", "subscribe"},
        {"channel", "market"},
        {"assets_ids", {token_id}}
    };

    std::string msg = sub_msg.dump();
    spdlog::info("Subscribing to market: {}", token_id);

    if (!send_raw(msg)) {
        spdlog::error("Failed to send subscription for {}", token_id);
    }
}

void PolymarketClient::unsubscribe_market(const std::string& token_id) {
    nlohmann::json unsub_msg = {
        {"type", "unsubscribe"},
        {"channel", "market"},
        {"assets_ids", {token_id}}
    };

    send_raw(unsub_msg.dump());
}

void PolymarketClient::run_connection_loop() {
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
                std::lock_guard<std::mutex> lock(time_mutex_);
                last_update_time_ = recv_time;
            }

            parse_message(msg, recv_time);
        }

        disconnect_socket();
        if (running_.load()) {
            status_ = ConnectionStatus::RECONNECTING;
            if (on_status_) on_status_(status_.load());
        }
    }
}

void PolymarketClient::parse_message(const std::string& msg, Timestamp recv_time) {
    try {
        auto j = nlohmann::json::parse(msg);

        std::string event_type = j.value("event_type", "");

        if (event_type == "book") {
            parse_book_message(j, recv_time);
        } else if (event_type == "price_change") {
            parse_price_change(j, recv_time);
        } else if (event_type == "last_trade_price") {
            parse_trade_message(j, recv_time);
        }

    } catch (const std::exception& e) {
        spdlog::debug("Failed to parse message: {} - {}", e.what(), msg.substr(0, 100));
    }
}

void PolymarketClient::parse_book_message(const nlohmann::json& data, Timestamp recv_time) {
    std::string asset_id = data.value("asset_id", "");
    if (asset_id.empty()) return;

    std::lock_guard<std::mutex> lock(books_mutex_);

    auto it = token_to_market_.find(asset_id);
    if (it == token_to_market_.end()) return;

    std::string market_id = it->second;
    auto book_it = market_books_.find(market_id);
    if (book_it == market_books_.end()) return;

    BinaryMarketBook* book = book_it->second.get();

    // Determine if YES or NO
    OrderBook* target_book = nullptr;
    if (book->yes_book().symbol().find(asset_id) != std::string::npos ||
        asset_id == book->yes_book().symbol()) {
        target_book = &book->yes_book();
    } else {
        target_book = &book->no_book();
    }

    std::vector<PriceLevel> bids, asks;

    if (data.contains("bids") && data["bids"].is_array()) {
        for (const auto& bid : data["bids"]) {
            PriceLevel level;
            level.price = std::stod(bid.value("price", "0"));
            level.size = std::stod(bid.value("size", "0"));
            if (level.price > 0) bids.push_back(level);
        }
    }

    if (data.contains("asks") && data["asks"].is_array()) {
        for (const auto& ask : data["asks"]) {
            PriceLevel level;
            level.price = std::stod(ask.value("price", "0"));
            level.size = std::stod(ask.value("size", "0"));
            if (level.price > 0) asks.push_back(level);
        }
    }

    target_book->apply_snapshot(bids, asks);

    if (on_book_update_) {
        on_book_update_(market_id, asset_id);
    }
}

void PolymarketClient::parse_price_change(const nlohmann::json& data, Timestamp recv_time) {
    // price_change updates individual price levels
    std::string asset_id = data.value("asset_id", "");
    if (asset_id.empty()) return;

    std::lock_guard<std::mutex> lock(books_mutex_);

    auto it = token_to_market_.find(asset_id);
    if (it == token_to_market_.end()) return;

    std::string market_id = it->second;
    auto book_it = market_books_.find(market_id);
    if (book_it == market_books_.end()) return;

    // Update individual levels if provided
    if (on_book_update_) {
        on_book_update_(market_id, asset_id);
    }
}

void PolymarketClient::parse_trade_message(const nlohmann::json& data, Timestamp recv_time) {
    Fill fill;
    fill.token_id = data.value("asset_id", "");
    fill.price = std::stod(data.value("price", "0"));
    fill.size = std::stod(data.value("size", "0"));

    std::string side_str = data.value("side", "");
    fill.side = (side_str == "buy" || side_str == "BUY") ? Side::BUY : Side::SELL;

    fill.fill_time = recv_time;
    if (data.contains("timestamp")) {
        fill.exchange_time_ms = data["timestamp"].get<int64_t>();
    }

    if (on_trade_) {
        on_trade_(fill);
    }
}

BinaryMarketBook* PolymarketClient::get_market_book(const std::string& market_id) {
    std::lock_guard<std::mutex> lock(books_mutex_);
    auto it = market_books_.find(market_id);
    if (it != market_books_.end()) {
        return it->second.get();
    }

    // Create new book
    auto book = std::make_unique<BinaryMarketBook>(market_id);
    BinaryMarketBook* ptr = book.get();
    market_books_[market_id] = std::move(book);
    return ptr;
}

void PolymarketClient::set_api_credentials(const std::string& key,
                                            const std::string& secret,
                                            const std::string& passphrase) {
    api_key_ = key;
    api_secret_ = secret;
    api_passphrase_ = passphrase;
    spdlog::info("API credentials set (key: {}...)", key.substr(0, 8));
}

PolymarketClient::OrderResponse PolymarketClient::place_order(const OrderRequest& req) {
    OrderResponse response;

    if (!has_credentials()) {
        response.error_message = "No API credentials set";
        return response;
    }

    try {
        nlohmann::json order_body = {
            {"tokenId", req.token_id},
            {"side", req.side == Side::BUY ? "BUY" : "SELL"},
            {"price", std::to_string(req.price)},
            {"size", std::to_string(req.size)},
            {"type", order_type_to_string(req.type)}
        };

        std::string url = config_.polymarket_rest_url + "/order";
        std::string result = http_post(url, order_body.dump());

        auto j = nlohmann::json::parse(result);

        if (j.contains("orderId") || j.contains("orderID")) {
            response.success = true;
            response.order_id = j.value("orderId", j.value("orderID", ""));
        } else {
            response.error_message = j.value("error", j.value("message", "Unknown error"));
        }

    } catch (const std::exception& e) {
        response.error_message = e.what();
        spdlog::error("Failed to place order: {}", e.what());
    }

    return response;
}

bool PolymarketClient::cancel_order(const std::string& order_id) {
    if (!has_credentials()) {
        return false;
    }

    try {
        nlohmann::json body = {{"orderId", order_id}};
        std::string url = config_.polymarket_rest_url + "/order/" + order_id;

        // Would need DELETE method - simplified for now
        spdlog::info("Cancel order request for: {}", order_id);
        return true;

    } catch (const std::exception& e) {
        spdlog::error("Failed to cancel order: {}", e.what());
        return false;
    }
}

Timestamp PolymarketClient::last_update_time() const {
    std::lock_guard<std::mutex> lock(time_mutex_);
    return last_update_time_;
}

} // namespace arb
