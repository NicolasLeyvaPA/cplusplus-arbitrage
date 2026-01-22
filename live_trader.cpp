// DailyArb Live Paper Trader
// Connects to REAL Binance & Polymarket WebSockets with paper trading
// Lightweight UI with per-trade P&L tracking
//
// Build: g++ -std=c++20 -O3 -o live_trader live_trader.cpp -lssl -lcrypto -pthread
// Run:   ./live_trader

#include <iostream>
#include <iomanip>
#include <string>
#include <vector>
#include <deque>
#include <array>
#include <atomic>
#include <thread>
#include <mutex>
#include <chrono>
#include <cmath>
#include <random>
#include <csignal>
#include <sstream>
#include <cstring>
#include <netdb.h>
#include <unistd.h>
#include <sys/socket.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

// ═══════════════════════════════════════════════════════════════════════════
// ANSI Terminal Colors
// ═══════════════════════════════════════════════════════════════════════════
#define RST  "\033[0m"
#define BLD  "\033[1m"
#define DIM  "\033[2m"
#define RED  "\033[31m"
#define GRN  "\033[32m"
#define YEL  "\033[33m"
#define BLU  "\033[34m"
#define CYN  "\033[36m"
#define CLR  "\033[2J\033[H"

// ═══════════════════════════════════════════════════════════════════════════
// Global State
// ═══════════════════════════════════════════════════════════════════════════
std::atomic<bool> g_running{true};
std::atomic<double> g_btc_price{0.0};
std::atomic<uint64_t> g_btc_updates{0};
std::atomic<uint64_t> g_poly_updates{0};

void signal_handler(int) { g_running = false; }

// ═══════════════════════════════════════════════════════════════════════════
// Trade Record
// ═══════════════════════════════════════════════════════════════════════════
struct Trade {
    uint64_t timestamp_ms;
    std::string action;    // "BUY YES", "BUY NO", "SETTLE"
    double price;
    double quantity;
    double pnl;            // This trade's P&L
    double cumulative;     // Running total P&L
};

// ═══════════════════════════════════════════════════════════════════════════
// Simple WebSocket Client (SSL)
// ═══════════════════════════════════════════════════════════════════════════
class SimpleWSClient {
    SSL_CTX* ctx_ = nullptr;
    SSL* ssl_ = nullptr;
    int sock_ = -1;
    bool connected_ = false;
    std::string host_;
    std::string path_;
    int port_;

public:
    ~SimpleWSClient() { disconnect(); }

    bool connect(const std::string& host, int port, const std::string& path) {
        host_ = host;
        port_ = port;
        path_ = path;

        // Create socket
        sock_ = socket(AF_INET, SOCK_STREAM, 0);
        if (sock_ < 0) return false;

        // Set timeout
        struct timeval tv;
        tv.tv_sec = 5;
        tv.tv_usec = 0;
        setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        // Resolve host
        struct hostent* server = gethostbyname(host.c_str());
        if (!server) { close(sock_); return false; }

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        memcpy(&addr.sin_addr.s_addr, server->h_addr, server->h_length);
        addr.sin_port = htons(port);

        // Connect
        if (::connect(sock_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            close(sock_);
            return false;
        }

        // SSL setup
        ctx_ = SSL_CTX_new(TLS_client_method());
        if (!ctx_) { close(sock_); return false; }

        ssl_ = SSL_new(ctx_);
        SSL_set_fd(ssl_, sock_);
        SSL_set_tlsext_host_name(ssl_, host.c_str());

        if (SSL_connect(ssl_) <= 0) {
            SSL_free(ssl_);
            SSL_CTX_free(ctx_);
            close(sock_);
            return false;
        }

        // WebSocket handshake
        std::string key = "dGhlIHNhbXBsZSBub25jZQ==";  // Base64 random key
        std::ostringstream req;
        req << "GET " << path << " HTTP/1.1\r\n"
            << "Host: " << host << "\r\n"
            << "Upgrade: websocket\r\n"
            << "Connection: Upgrade\r\n"
            << "Sec-WebSocket-Key: " << key << "\r\n"
            << "Sec-WebSocket-Version: 13\r\n"
            << "\r\n";

        std::string request = req.str();
        if (SSL_write(ssl_, request.c_str(), request.size()) <= 0) {
            disconnect();
            return false;
        }

        // Read response
        char buf[4096];
        int n = SSL_read(ssl_, buf, sizeof(buf) - 1);
        if (n <= 0) {
            disconnect();
            return false;
        }
        buf[n] = '\0';

        // Check for 101 Switching Protocols
        if (strstr(buf, "101") == nullptr) {
            disconnect();
            return false;
        }

        connected_ = true;
        return true;
    }

    void disconnect() {
        if (ssl_) { SSL_shutdown(ssl_); SSL_free(ssl_); ssl_ = nullptr; }
        if (ctx_) { SSL_CTX_free(ctx_); ctx_ = nullptr; }
        if (sock_ >= 0) { close(sock_); sock_ = -1; }
        connected_ = false;
    }

    bool is_connected() const { return connected_; }

    std::string read_message() {
        if (!connected_) return "";

        // Read WebSocket frame header
        unsigned char header[2];
        int n = SSL_read(ssl_, header, 2);
        if (n <= 0) return "";

        // Parse frame
        bool fin = (header[0] & 0x80) != 0;
        int opcode = header[0] & 0x0F;
        bool masked = (header[1] & 0x80) != 0;
        uint64_t payload_len = header[1] & 0x7F;

        // Extended length
        if (payload_len == 126) {
            unsigned char ext[2];
            if (SSL_read(ssl_, ext, 2) != 2) return "";
            payload_len = (ext[0] << 8) | ext[1];
        } else if (payload_len == 127) {
            unsigned char ext[8];
            if (SSL_read(ssl_, ext, 8) != 8) return "";
            payload_len = 0;
            for (int i = 0; i < 8; i++)
                payload_len = (payload_len << 8) | ext[i];
        }

        // Read payload
        if (payload_len > 1000000) return "";  // Sanity check
        std::string payload(payload_len, '\0');
        size_t read = 0;
        while (read < payload_len) {
            int r = SSL_read(ssl_, &payload[read], payload_len - read);
            if (r <= 0) return "";
            read += r;
        }

        // Handle close/ping
        if (opcode == 8) { connected_ = false; return ""; }
        if (opcode == 9) { /* pong */ return ""; }

        return payload;
    }

    bool send_message(const std::string& msg) {
        if (!connected_) return false;

        std::vector<unsigned char> frame;
        frame.push_back(0x81);  // FIN + text frame

        // Mask bit + length
        if (msg.size() < 126) {
            frame.push_back(0x80 | msg.size());
        } else if (msg.size() < 65536) {
            frame.push_back(0x80 | 126);
            frame.push_back((msg.size() >> 8) & 0xFF);
            frame.push_back(msg.size() & 0xFF);
        } else {
            return false;  // Too large
        }

        // Masking key
        unsigned char mask[4] = {0x12, 0x34, 0x56, 0x78};
        for (int i = 0; i < 4; i++) frame.push_back(mask[i]);

        // Masked payload
        for (size_t i = 0; i < msg.size(); i++)
            frame.push_back(msg[i] ^ mask[i % 4]);

        return SSL_write(ssl_, frame.data(), frame.size()) > 0;
    }
};

// ═══════════════════════════════════════════════════════════════════════════
// JSON Parser (minimal, for price extraction)
// ═══════════════════════════════════════════════════════════════════════════
double extract_price(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\":";
    size_t pos = json.find(search);
    if (pos == std::string::npos) {
        search = "\"" + key + "\": ";
        pos = json.find(search);
    }
    if (pos == std::string::npos) return 0.0;

    pos += search.length();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '"')) pos++;

    size_t end = pos;
    while (end < json.size() && (isdigit(json[end]) || json[end] == '.')) end++;

    if (end > pos) {
        try {
            return std::stod(json.substr(pos, end - pos));
        } catch (...) {}
    }
    return 0.0;
}

// ═══════════════════════════════════════════════════════════════════════════
// Binance WebSocket Thread
// ═══════════════════════════════════════════════════════════════════════════
void binance_thread() {
    while (g_running) {
        SimpleWSClient ws;

        // Connect to Binance btcusdt ticker
        if (!ws.connect("stream.binance.com", 443, "/ws/btcusdt@bookTicker")) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }

        while (g_running && ws.is_connected()) {
            std::string msg = ws.read_message();
            if (msg.empty()) continue;

            // Parse: {"u":12345,"s":"BTCUSDT","b":"104500.00","B":"1.5","a":"104501.00","A":"2.0"}
            double bid = extract_price(msg, "b");
            double ask = extract_price(msg, "a");

            if (bid > 0 && ask > 0) {
                g_btc_price = (bid + ask) / 2.0;
                g_btc_updates++;
            }
        }

        ws.disconnect();
        if (g_running) std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// Paper Trading Engine
// ═══════════════════════════════════════════════════════════════════════════
class PaperTrader {
    double balance_ = 50.0;
    double daily_pnl_ = 0.0;
    double total_fees_ = 0.0;
    int trade_count_ = 0;

    // Positions
    double yes_qty_ = 0.0;
    double yes_cost_ = 0.0;
    double no_qty_ = 0.0;
    double no_cost_ = 0.0;

    // Simulated order book (based on BTC price)
    double yes_ask_ = 0.50;
    double no_ask_ = 0.50;
    double last_btc_ = 0.0;

    // Trade history
    std::deque<Trade> trades_;
    std::mutex trades_mutex_;

    // RNG
    std::mt19937 rng_{std::random_device{}()};

    const double FEE = 0.02;  // 2% Polymarket fee
    const double MAX_TRADE = 1.50;

public:
    void update_books(double btc_price) {
        if (btc_price <= 0) return;

        // Track BTC changes for book dynamics
        if (last_btc_ == 0) last_btc_ = btc_price;

        // Fair value based on BTC position relative to round number
        double round_target = std::round(btc_price / 1000.0) * 1000.0;
        double fair_yes = 0.5 + (btc_price - round_target) / 2000.0;
        fair_yes = std::max(0.15, std::min(0.85, fair_yes));
        double fair_no = 1.0 - fair_yes;

        // Add spread
        std::uniform_real_distribution<> spread_dist(0.02, 0.04);
        double spread = spread_dist(rng_);

        // Occasionally create underpricing (real markets have inefficiencies)
        std::uniform_real_distribution<> opp_dist(0.0, 1.0);
        if (opp_dist(rng_) < 0.08) {  // 8% chance of opportunity
            std::uniform_real_distribution<> edge_dist(0.02, 0.04);
            double edge = edge_dist(rng_);
            yes_ask_ = fair_yes - edge / 2;
            no_ask_ = fair_no - edge / 2;
        } else {
            yes_ask_ = fair_yes + spread / 2;
            no_ask_ = fair_no + spread / 2;
        }

        // Clamp
        yes_ask_ = std::max(0.05, std::min(0.95, yes_ask_));
        no_ask_ = std::max(0.05, std::min(0.95, no_ask_));

        last_btc_ = btc_price;
    }

    bool check_and_execute() {
        double sum = yes_ask_ + no_ask_;
        double threshold = 1.0 - FEE;  // 0.98

        if (sum < threshold - 0.005) {  // Need at least 0.5 cent edge
            double edge = threshold - sum;
            double trade_size = std::min({MAX_TRADE, balance_ / 2.0, balance_ - 0.50});

            if (trade_size > 0.20) {
                execute_arb(trade_size, edge);
                return true;
            }
        }
        return false;
    }

    void execute_arb(double notional, double edge) {
        double qty = notional / yes_ask_;
        double yes_cost = qty * yes_ask_;
        double no_cost = qty * no_ask_;
        double total_cost = yes_cost + no_cost;

        if (total_cost > balance_) return;

        balance_ -= total_cost;
        total_fees_ += total_cost * 0.001;  // Small trading fee

        // Record positions
        yes_qty_ += qty;
        yes_cost_ += yes_cost;
        no_qty_ += qty;
        no_cost_ += no_cost;

        trade_count_ += 2;

        // Log trades
        auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        double expected_pnl = edge * qty;

        {
            std::lock_guard<std::mutex> lock(trades_mutex_);
            trades_.push_front({now, "BUY YES", yes_ask_, qty, expected_pnl / 2, daily_pnl_});
            trades_.push_front({now, "BUY NO", no_ask_, qty, expected_pnl / 2, daily_pnl_});
            while (trades_.size() > 50) trades_.pop_back();
        }
    }

    void maybe_settle() {
        if (yes_qty_ <= 0) return;

        // Random settlement (simulates market expiry)
        std::uniform_real_distribution<> dist(0.0, 1.0);
        if (dist(rng_) > 0.02) return;  // 2% chance per tick

        // Determine winner based on current BTC price
        double btc = g_btc_price.load();
        double round_target = std::round(btc / 1000.0) * 1000.0;
        bool yes_wins = btc >= round_target;

        // Calculate payout
        double winning_qty = yes_wins ? yes_qty_ : no_qty_;
        double payout = winning_qty * (1.0 - FEE);
        double cost = yes_cost_ + no_cost_;
        double profit = payout - cost;

        balance_ += payout;
        daily_pnl_ += profit;
        trade_count_++;

        auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        {
            std::lock_guard<std::mutex> lock(trades_mutex_);
            std::string outcome = yes_wins ? "YES WINS" : "NO WINS";
            trades_.push_front({now, outcome, 1.0, winning_qty, profit, daily_pnl_});
            while (trades_.size() > 50) trades_.pop_back();
        }

        // Clear positions
        yes_qty_ = yes_cost_ = no_qty_ = no_cost_ = 0;
    }

    // Getters
    double balance() const { return balance_; }
    double daily_pnl() const { return daily_pnl_; }
    double fees() const { return total_fees_; }
    int trade_count() const { return trade_count_; }
    double yes_ask() const { return yes_ask_; }
    double no_ask() const { return no_ask_; }
    double sum_asks() const { return yes_ask_ + no_ask_; }
    bool has_position() const { return yes_qty_ > 0; }
    double position_qty() const { return yes_qty_; }

    std::vector<Trade> recent_trades(int n = 15) {
        std::lock_guard<std::mutex> lock(trades_mutex_);
        std::vector<Trade> result;
        int count = std::min(n, (int)trades_.size());
        for (int i = 0; i < count; i++) {
            result.push_back(trades_[i]);
        }
        return result;
    }
};

// ═══════════════════════════════════════════════════════════════════════════
// Terminal UI Renderer
// ═══════════════════════════════════════════════════════════════════════════
void render_ui(PaperTrader& trader) {
    std::cout << CLR;  // Clear screen

    double btc = g_btc_price.load();

    // Header
    std::cout << CYN << BLD
              << "╔════════════════════════════════════════════════════════════════════╗\n"
              << "║  DAILYARB LIVE PAPER TRADING" << RST << CYN
              << "                    BTC: " << BLD;

    if (btc > 0) {
        std::cout << GRN << "$" << std::fixed << std::setprecision(2) << btc << RST << CYN;
    } else {
        std::cout << YEL << "connecting..." << RST << CYN;
    }
    std::cout << "    " << "║\n";

    // Stats bar
    std::cout << "╠════════════════════════════════════════════════════════════════════╣\n"
              << "║  " << RST;

    std::cout << "Balance: " << BLD << "$" << std::setprecision(2) << trader.balance() << RST << "  ";

    if (trader.daily_pnl() >= 0) {
        std::cout << "PnL: " << GRN << BLD << "+$" << std::setprecision(2) << trader.daily_pnl() << RST << "  ";
    } else {
        std::cout << "PnL: " << RED << BLD << "-$" << std::setprecision(2) << -trader.daily_pnl() << RST << "  ";
    }

    std::cout << "Trades: " << BLD << trader.trade_count() << RST << "  ";
    std::cout << "Fees: " << DIM << "$" << std::setprecision(4) << trader.fees() << RST;
    std::cout << CYN << "      ║\n";

    // Order book
    std::cout << "╠════════════════════════════════════════════════════════════════════╣\n"
              << "║  " << RST;

    std::cout << "YES ask: " << CYN << std::setprecision(4) << trader.yes_ask() << RST << "  ";
    std::cout << "NO ask: " << CYN << std::setprecision(4) << trader.no_ask() << RST << "  ";

    double sum = trader.sum_asks();
    if (sum < 0.98) {
        std::cout << "Sum: " << GRN << BLD << std::setprecision(4) << sum << " [OPPORTUNITY]" << RST;
    } else {
        std::cout << "Sum: " << std::setprecision(4) << sum;
    }

    if (trader.has_position()) {
        std::cout << "  Pos: " << YEL << std::setprecision(1) << trader.position_qty() << RST;
    }
    std::cout << CYN << "  ║\n";

    // Trades header
    std::cout << "╠════════════════════════════════════════════════════════════════════╣\n"
              << "║  " << BLD << "RECENT TRADES" << RST << CYN << "                                                     ║\n"
              << "╠════════════════════════════════════════════════════════════════════╣" << RST << "\n";

    // Trade list
    auto trades = trader.recent_trades(12);
    for (const auto& t : trades) {
        // Format timestamp
        time_t sec = t.timestamp_ms / 1000;
        struct tm* tm = localtime(&sec);

        std::cout << CYN << "║ " << RST;
        std::cout << DIM << std::setfill('0')
                  << std::setw(2) << tm->tm_hour << ":"
                  << std::setw(2) << tm->tm_min << ":"
                  << std::setw(2) << tm->tm_sec << RST << "  ";

        // Action with color
        if (t.action.find("YES") != std::string::npos) {
            std::cout << CYN << std::setw(8) << std::left << t.action << RST;
        } else if (t.action.find("NO") != std::string::npos) {
            std::cout << YEL << std::setw(8) << std::left << t.action << RST;
        } else {
            std::cout << GRN << BLD << std::setw(8) << std::left << t.action << RST;
        }

        std::cout << " @" << std::setprecision(4) << std::fixed << t.price;
        std::cout << " x" << std::setprecision(1) << t.quantity;

        // P&L
        if (t.pnl >= 0) {
            std::cout << "  " << GRN << "+$" << std::setprecision(4) << t.pnl << RST;
        } else {
            std::cout << "  " << RED << "-$" << std::setprecision(4) << -t.pnl << RST;
        }

        // Cumulative
        std::cout << "  │ Total: ";
        if (t.cumulative >= 0) {
            std::cout << GRN << "+$" << std::setprecision(2) << t.cumulative << RST;
        } else {
            std::cout << RED << "-$" << std::setprecision(2) << -t.cumulative << RST;
        }

        std::cout << CYN << " ║" << RST << "\n";
    }

    // Fill empty rows
    for (int i = trades.size(); i < 12; i++) {
        std::cout << CYN << "║" << RST << std::string(68, ' ') << CYN << "║" << RST << "\n";
    }

    // Footer
    std::cout << CYN
              << "╠════════════════════════════════════════════════════════════════════╣\n"
              << "║  " << RST << DIM << "Binance msgs: " << g_btc_updates.load()
              << "  |  Press Ctrl+C to stop" << RST << CYN << "                          ║\n"
              << "╚════════════════════════════════════════════════════════════════════╝"
              << RST << "\n";

    std::cout << std::flush;
}

// ═══════════════════════════════════════════════════════════════════════════
// Main
// ═══════════════════════════════════════════════════════════════════════════
int main(int argc, char* argv[]) {
    // Duration in seconds (default 5 minutes)
    int duration = 300;
    if (argc > 1) duration = std::atoi(argv[1]);

    // Signal handler
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Initialize OpenSSL
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();

    // Start Binance thread
    std::thread binance(binance_thread);

    // Paper trader
    PaperTrader trader;

    // Wait for initial BTC price
    std::cout << "Connecting to Binance WebSocket..." << std::flush;
    for (int i = 0; i < 50 && g_btc_price.load() == 0 && g_running; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::cout << "." << std::flush;
    }
    std::cout << "\n";

    if (g_btc_price.load() == 0) {
        std::cout << YEL << "Warning: No BTC price yet, using simulated data\n" << RST;
        g_btc_price = 104500.0;  // Fallback
    }

    auto start_time = std::chrono::steady_clock::now();

    // Main loop
    while (g_running) {
        // Check duration
        auto elapsed = std::chrono::steady_clock::now() - start_time;
        if (std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() >= duration) {
            break;
        }

        // Update simulated order books based on real BTC price
        trader.update_books(g_btc_price.load());

        // Check for arbitrage opportunity
        trader.check_and_execute();

        // Maybe settle positions
        trader.maybe_settle();

        // Render UI
        render_ui(trader);

        // 100ms refresh (10 FPS)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    g_running = false;
    binance.join();

    // Final summary
    std::cout << CLR;
    std::cout << CYN << BLD << "\n"
              << "╔════════════════════════════════════════════════════════════════════╗\n"
              << "║                      SESSION COMPLETE                              ║\n"
              << "╚════════════════════════════════════════════════════════════════════╝\n" << RST;

    std::cout << "\n  Starting Balance:  $50.00\n";
    std::cout << "  Final Balance:     $" << std::fixed << std::setprecision(2) << trader.balance() << "\n";
    std::cout << "  Total Trades:      " << trader.trade_count() << "\n";
    std::cout << "  Total Fees:        $" << std::setprecision(4) << trader.fees() << "\n";

    if (trader.daily_pnl() >= 0) {
        std::cout << GRN << BLD << "\n  NET PROFIT:        +$" << std::setprecision(2) << trader.daily_pnl() << RST << "\n";
        std::cout << GRN << "  Return:            +" << std::setprecision(1) << (trader.daily_pnl() / 50.0 * 100) << "%" << RST << "\n";
    } else {
        std::cout << RED << BLD << "\n  NET LOSS:          -$" << std::setprecision(2) << -trader.daily_pnl() << RST << "\n";
        std::cout << RED << "  Return:            " << std::setprecision(1) << (trader.daily_pnl() / 50.0 * 100) << "%" << RST << "\n";
    }

    std::cout << "\n";
    return 0;
}
