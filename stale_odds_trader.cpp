// S1 Stale Odds Strategy - Where REAL Edge Exists
// Detects when BTC moves but Polymarket odds lag behind
//
// THE EDGE: When BTC moves 25+ bps quickly, prediction markets are slow to update.
// Buy the side that SHOULD be more likely given the BTC move, sell when market catches up.
//
// Build: g++ -std=c++20 -O3 -o stale_odds_trader stale_odds_trader.cpp -lssl -lcrypto -pthread
// Run:   ./stale_odds_trader [duration_seconds]

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
#define MAG  "\033[35m"
#define CYN  "\033[36m"
#define CLR  "\033[2J\033[H"

// ═══════════════════════════════════════════════════════════════════════════
// Global State
// ═══════════════════════════════════════════════════════════════════════════
std::atomic<bool> g_running{true};
std::atomic<double> g_btc_price{0.0};
std::atomic<double> g_btc_price_1s_ago{0.0};
std::atomic<double> g_btc_price_5s_ago{0.0};
std::atomic<uint64_t> g_btc_updates{0};
std::atomic<uint64_t> g_last_btc_update_ms{0};

void signal_handler(int) { g_running = false; }

uint64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// ═══════════════════════════════════════════════════════════════════════════
// Trade Record
// ═══════════════════════════════════════════════════════════════════════════
struct Trade {
    uint64_t timestamp_ms;
    std::string action;    // "BUY YES", "SELL YES", "BUY NO", "SELL NO"
    std::string reason;    // "STALE_UP", "STALE_DOWN", "EXIT"
    double price;
    double quantity;
    double pnl;
    double cumulative;
};

// ═══════════════════════════════════════════════════════════════════════════
// BTC Price History (for detecting moves)
// ═══════════════════════════════════════════════════════════════════════════
class PriceHistory {
    std::deque<std::pair<uint64_t, double>> prices_;  // timestamp_ms, price
    std::mutex mutex_;
    static constexpr size_t MAX_HISTORY = 600;  // 60 seconds at 10Hz

public:
    void record(double price) {
        std::lock_guard<std::mutex> lock(mutex_);
        uint64_t now = now_ms();
        prices_.push_back({now, price});
        while (prices_.size() > MAX_HISTORY) prices_.pop_front();

        // Update atomic references for quick access
        g_btc_price = price;
        g_last_btc_update_ms = now;
    }

    // Get price from N milliseconds ago
    double price_at(int ms_ago) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (prices_.empty()) return 0.0;

        uint64_t target = now_ms() - ms_ago;
        for (auto it = prices_.rbegin(); it != prices_.rend(); ++it) {
            if (it->first <= target) return it->second;
        }
        return prices_.front().second;
    }

    // Calculate move in basis points over last N ms
    double move_bps(int ms_window) {
        double current = g_btc_price.load();
        double past = price_at(ms_window);
        if (past <= 0 || current <= 0) return 0.0;
        return (current - past) / past * 10000.0;  // basis points
    }

    // Get volatility (std dev of returns) over window
    double volatility(int ms_window) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (prices_.size() < 10) return 0.0;

        uint64_t cutoff = now_ms() - ms_window;
        std::vector<double> returns;

        double prev = 0;
        for (const auto& [ts, price] : prices_) {
            if (ts < cutoff) continue;
            if (prev > 0) {
                returns.push_back((price - prev) / prev);
            }
            prev = price;
        }

        if (returns.size() < 5) return 0.0;

        double mean = 0;
        for (double r : returns) mean += r;
        mean /= returns.size();

        double var = 0;
        for (double r : returns) var += (r - mean) * (r - mean);
        var /= returns.size();

        return std::sqrt(var) * 10000;  // in bps
    }
};

PriceHistory g_price_history;

// ═══════════════════════════════════════════════════════════════════════════
// Simple WebSocket Client (SSL)
// ═══════════════════════════════════════════════════════════════════════════
class SimpleWSClient {
    SSL_CTX* ctx_ = nullptr;
    SSL* ssl_ = nullptr;
    int sock_ = -1;
    bool connected_ = false;

public:
    ~SimpleWSClient() { disconnect(); }

    bool connect(const std::string& host, int port, const std::string& path) {
        sock_ = socket(AF_INET, SOCK_STREAM, 0);
        if (sock_ < 0) return false;

        struct timeval tv;
        tv.tv_sec = 5;
        tv.tv_usec = 0;
        setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        struct hostent* server = gethostbyname(host.c_str());
        if (!server) { close(sock_); return false; }

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        memcpy(&addr.sin_addr.s_addr, server->h_addr, server->h_length);
        addr.sin_port = htons(port);

        if (::connect(sock_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            close(sock_);
            return false;
        }

        ctx_ = SSL_CTX_new(TLS_client_method());
        if (!ctx_) { close(sock_); return false; }

        ssl_ = SSL_new(ctx_);
        SSL_set_fd(ssl_, sock_);
        SSL_set_tlsext_host_name(ssl_, host.c_str());

        if (SSL_connect(ssl_) <= 0) {
            SSL_free(ssl_); SSL_CTX_free(ctx_); close(sock_);
            return false;
        }

        std::string key = "dGhlIHNhbXBsZSBub25jZQ==";
        std::ostringstream req;
        req << "GET " << path << " HTTP/1.1\r\n"
            << "Host: " << host << "\r\n"
            << "Upgrade: websocket\r\n"
            << "Connection: Upgrade\r\n"
            << "Sec-WebSocket-Key: " << key << "\r\n"
            << "Sec-WebSocket-Version: 13\r\n\r\n";

        std::string request = req.str();
        if (SSL_write(ssl_, request.c_str(), request.size()) <= 0) {
            disconnect();
            return false;
        }

        char buf[4096];
        int n = SSL_read(ssl_, buf, sizeof(buf) - 1);
        if (n <= 0 || strstr(buf, "101") == nullptr) {
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

        unsigned char header[2];
        int n = SSL_read(ssl_, header, 2);
        if (n <= 0) return "";

        uint64_t payload_len = header[1] & 0x7F;

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

        if (payload_len > 1000000) return "";
        std::string payload(payload_len, '\0');
        size_t read = 0;
        while (read < payload_len) {
            int r = SSL_read(ssl_, &payload[read], payload_len - read);
            if (r <= 0) return "";
            read += r;
        }

        int opcode = header[0] & 0x0F;
        if (opcode == 8) { connected_ = false; return ""; }

        return payload;
    }
};

// ═══════════════════════════════════════════════════════════════════════════
// JSON Parser (minimal)
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
        try { return std::stod(json.substr(pos, end - pos)); }
        catch (...) {}
    }
    return 0.0;
}

// ═══════════════════════════════════════════════════════════════════════════
// Binance WebSocket Thread
// ═══════════════════════════════════════════════════════════════════════════
void binance_thread() {
    while (g_running) {
        SimpleWSClient ws;

        if (!ws.connect("stream.binance.com", 443, "/ws/btcusdt@bookTicker")) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }

        while (g_running && ws.is_connected()) {
            std::string msg = ws.read_message();
            if (msg.empty()) continue;

            double bid = extract_price(msg, "b");
            double ask = extract_price(msg, "a");

            if (bid > 0 && ask > 0) {
                double mid = (bid + ask) / 2.0;
                g_price_history.record(mid);
                g_btc_updates++;
            }
        }

        ws.disconnect();
        if (g_running) std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// S1 Stale Odds Paper Trader
// ═══════════════════════════════════════════════════════════════════════════
class StaleOddsTrader {
    // Account
    double balance_ = 50.0;
    double starting_balance_ = 50.0;
    double total_fees_ = 0.0;
    int trade_count_ = 0;
    int signals_generated_ = 0;
    int signals_traded_ = 0;

    // Market state - "Will BTC be above $X at close?"
    double target_price_ = 0.0;  // Set based on current BTC
    double yes_price_ = 0.50;    // Current YES price (simulated Polymarket)
    double no_price_ = 0.50;     // Current NO price
    uint64_t last_market_update_ms_ = 0;

    // Position
    std::string position_side_ = "";  // "YES", "NO", or ""
    double position_qty_ = 0.0;
    double position_entry_ = 0.0;
    uint64_t position_entry_time_ = 0;

    // Strategy parameters
    const double MOVE_THRESHOLD_BPS = 15.0;   // BTC move to trigger signal
    const double STALENESS_MS = 500.0;        // How long market must be stale
    const double MIN_EDGE_BPS = 10.0;         // Minimum expected edge
    const double MAX_HOLD_MS = 30000.0;       // Max hold time (30s)
    const double TAKE_PROFIT_BPS = 20.0;      // Take profit threshold
    const double STOP_LOSS_BPS = 15.0;        // Stop loss threshold
    const double MAX_POSITION = 2.00;         // Max position size
    const double FEE_RATE = 0.002;            // 0.2% per trade

    // Trade history
    std::deque<Trade> trades_;
    std::mutex trades_mutex_;

    std::mt19937 rng_{std::random_device{}()};

public:
    void initialize_market() {
        double btc = g_btc_price.load();
        if (btc <= 0) btc = 104500.0;  // Fallback

        // Set target to nearest $1000 round number
        target_price_ = std::round(btc / 1000.0) * 1000.0;

        // Initialize fair odds based on distance to target
        update_fair_odds();
        last_market_update_ms_ = now_ms();
    }

    void update_fair_odds() {
        double btc = g_btc_price.load();
        if (btc <= 0) return;

        // Fair probability based on distance to target
        // Using simple linear model (real would use options pricing)
        double distance_pct = (btc - target_price_) / target_price_ * 100;

        // Map distance to probability (rough approximation)
        // At target: 50%, 1% above: ~60%, 1% below: ~40%
        double fair_yes = 0.5 + distance_pct * 0.1;
        fair_yes = std::max(0.05, std::min(0.95, fair_yes));

        // Add small spread
        yes_price_ = fair_yes;
        no_price_ = 1.0 - fair_yes;
    }

    // Simulate stale market (updates with lag)
    void simulate_market_lag() {
        uint64_t now = now_ms();

        // Market updates randomly every 200-2000ms (simulating slow Polymarket)
        std::uniform_real_distribution<> update_dist(0.0, 1.0);
        double update_prob = (now - last_market_update_ms_) / 1000.0 * 0.5;  // ~50% per second

        if (update_dist(rng_) < update_prob) {
            update_fair_odds();
            last_market_update_ms_ = now;
        }
    }

    // Check for stale odds opportunity
    bool check_stale_signal(std::string& side, double& expected_edge) {
        double btc = g_btc_price.load();
        if (btc <= 0) return false;

        // Get BTC move over last 1 second
        double move_1s = g_price_history.move_bps(1000);

        // Get BTC move over last 5 seconds
        double move_5s = g_price_history.move_bps(5000);

        // Check if significant move occurred
        if (std::abs(move_1s) < MOVE_THRESHOLD_BPS) return false;

        // Check if market is stale (hasn't updated recently)
        uint64_t staleness = now_ms() - last_market_update_ms_;
        if (staleness < STALENESS_MS) return false;

        // Calculate where price SHOULD be vs where it IS
        double distance_pct = (btc - target_price_) / target_price_ * 100;
        double fair_yes = 0.5 + distance_pct * 0.1;
        fair_yes = std::max(0.05, std::min(0.95, fair_yes));

        // Calculate edge
        double edge_yes = (fair_yes - yes_price_) * 100;  // In cents
        double edge_no = ((1.0 - fair_yes) - no_price_) * 100;

        // Determine which side to trade
        if (move_1s > MOVE_THRESHOLD_BPS && edge_yes > MIN_EDGE_BPS / 100) {
            // BTC went UP, YES should be higher, market is stale
            side = "YES";
            expected_edge = edge_yes;
            return true;
        } else if (move_1s < -MOVE_THRESHOLD_BPS && edge_no > MIN_EDGE_BPS / 100) {
            // BTC went DOWN, NO should be higher, market is stale
            side = "NO";
            expected_edge = edge_no;
            return true;
        }

        return false;
    }

    void enter_position(const std::string& side, double edge) {
        if (!position_side_.empty()) return;  // Already in position
        if (balance_ < 1.0) return;  // Not enough balance

        double price = (side == "YES") ? yes_price_ : no_price_;
        double size = std::min(MAX_POSITION, balance_ * 0.5);  // 50% of balance max
        double qty = size / price;
        double fee = size * FEE_RATE;

        balance_ -= size + fee;
        total_fees_ += fee;

        position_side_ = side;
        position_qty_ = qty;
        position_entry_ = price;
        position_entry_time_ = now_ms();

        trade_count_++;
        signals_traded_++;

        log_trade("BUY " + side, "STALE", price, qty, -fee);
    }

    void check_exit() {
        if (position_side_.empty()) return;

        double current_price = (position_side_ == "YES") ? yes_price_ : no_price_;
        double pnl_bps = (current_price - position_entry_) / position_entry_ * 10000;
        uint64_t hold_time = now_ms() - position_entry_time_;

        bool should_exit = false;
        std::string reason;

        // Take profit
        if (pnl_bps > TAKE_PROFIT_BPS) {
            should_exit = true;
            reason = "TP";
        }
        // Stop loss
        else if (pnl_bps < -STOP_LOSS_BPS) {
            should_exit = true;
            reason = "SL";
        }
        // Max hold time
        else if (hold_time > MAX_HOLD_MS) {
            should_exit = true;
            reason = "TIME";
        }

        if (should_exit) {
            exit_position(reason);
        }
    }

    void exit_position(const std::string& reason) {
        if (position_side_.empty()) return;

        double exit_price = (position_side_ == "YES") ? yes_price_ : no_price_;
        double proceeds = position_qty_ * exit_price;
        double fee = proceeds * FEE_RATE;
        double pnl = proceeds - (position_qty_ * position_entry_) - fee;

        balance_ += proceeds - fee;
        total_fees_ += fee;
        trade_count_++;

        log_trade("SELL " + position_side_, reason, exit_price, position_qty_, pnl);

        position_side_ = "";
        position_qty_ = 0;
        position_entry_ = 0;
    }

    void log_trade(const std::string& action, const std::string& reason,
                   double price, double qty, double pnl) {
        std::lock_guard<std::mutex> lock(trades_mutex_);
        double cum_pnl = balance_ - starting_balance_;
        trades_.push_front({now_ms(), action, reason, price, qty, pnl, cum_pnl});
        while (trades_.size() > 50) trades_.pop_back();
    }

    void tick() {
        // Simulate market lag
        simulate_market_lag();

        // Check for exit if in position
        check_exit();

        // Check for new signal if not in position
        if (position_side_.empty()) {
            std::string side;
            double edge;
            if (check_stale_signal(side, edge)) {
                signals_generated_++;
                enter_position(side, edge);
            }
        }
    }

    // Getters
    double balance() const { return balance_; }
    double true_pnl() const { return balance_ - starting_balance_; }
    double fees() const { return total_fees_; }
    int trade_count() const { return trade_count_; }
    int signals() const { return signals_generated_; }
    double yes_price() const { return yes_price_; }
    double no_price() const { return no_price_; }
    double target() const { return target_price_; }
    bool has_position() const { return !position_side_.empty(); }
    std::string position_side() const { return position_side_; }
    double position_qty() const { return position_qty_; }
    double position_entry() const { return position_entry_; }
    uint64_t market_staleness() const { return now_ms() - last_market_update_ms_; }

    double position_pnl() const {
        if (position_side_.empty()) return 0.0;
        double current = (position_side_ == "YES") ? yes_price_ : no_price_;
        return (current - position_entry_) * position_qty_;
    }

    std::vector<Trade> recent_trades(int n = 12) {
        std::lock_guard<std::mutex> lock(trades_mutex_);
        std::vector<Trade> result;
        int count = std::min(n, (int)trades_.size());
        for (int i = 0; i < count; i++) {
            result.push_back(trades_[i]);
        }
        return result;
    }

    void force_exit() {
        if (!position_side_.empty()) {
            exit_position("END");
        }
    }
};

// ═══════════════════════════════════════════════════════════════════════════
// Terminal UI Renderer
// ═══════════════════════════════════════════════════════════════════════════
void render_ui(StaleOddsTrader& trader) {
    std::cout << CLR;

    double btc = g_btc_price.load();
    double move_1s = g_price_history.move_bps(1000);
    double move_5s = g_price_history.move_bps(5000);

    // Header
    std::cout << CYN << BLD
              << "╔════════════════════════════════════════════════════════════════════════╗\n"
              << "║  S1 STALE ODDS STRATEGY" << RST << CYN
              << "                          BTC: " << BLD;

    if (btc > 0) {
        std::cout << GRN << "$" << std::fixed << std::setprecision(2) << btc << RST;
    } else {
        std::cout << YEL << "connecting..." << RST;
    }
    std::cout << CYN << "   ║\n";

    // BTC Movement
    std::cout << "╠════════════════════════════════════════════════════════════════════════╣\n"
              << "║  " << RST;

    std::cout << "1s Move: ";
    if (move_1s > 15) std::cout << GRN << BLD << "+" << std::setprecision(1) << move_1s << " bps" << RST;
    else if (move_1s < -15) std::cout << RED << BLD << std::setprecision(1) << move_1s << " bps" << RST;
    else std::cout << std::setprecision(1) << move_1s << " bps";

    std::cout << "  5s Move: ";
    if (move_5s > 25) std::cout << GRN << "+" << std::setprecision(1) << move_5s << RST;
    else if (move_5s < -25) std::cout << RED << std::setprecision(1) << move_5s << RST;
    else std::cout << std::setprecision(1) << move_5s;

    std::cout << "  Staleness: ";
    uint64_t stale = trader.market_staleness();
    if (stale > 500) std::cout << YEL << stale << "ms" << RST;
    else std::cout << stale << "ms";

    std::cout << CYN << "      ║\n";

    // Account stats
    double true_pnl = trader.true_pnl();
    std::cout << "╠════════════════════════════════════════════════════════════════════════╣\n"
              << "║  " << RST;

    std::cout << "Balance: " << BLD << "$" << std::setprecision(2) << trader.balance() << RST << "  ";

    if (true_pnl >= 0) {
        std::cout << "PnL: " << GRN << BLD << "+$" << std::setprecision(2) << true_pnl << RST << "  ";
    } else {
        std::cout << "PnL: " << RED << BLD << "-$" << std::setprecision(2) << -true_pnl << RST << "  ";
    }

    std::cout << "Signals: " << trader.signals() << "  ";
    std::cout << "Trades: " << trader.trade_count();
    std::cout << CYN << "              ║\n";

    // Market state
    std::cout << "╠════════════════════════════════════════════════════════════════════════╣\n"
              << "║  " << RST;

    std::cout << "Market: BTC > $" << std::setprecision(0) << trader.target() << "?  ";
    std::cout << "YES: " << CYN << std::setprecision(4) << trader.yes_price() << RST << "  ";
    std::cout << "NO: " << MAG << std::setprecision(4) << trader.no_price() << RST;

    if (trader.has_position()) {
        std::cout << "  │ " << YEL << BLD << trader.position_side() << RST;
        std::cout << " x" << std::setprecision(1) << trader.position_qty();
        double pos_pnl = trader.position_pnl();
        if (pos_pnl >= 0) std::cout << " " << GRN << "+$" << std::setprecision(4) << pos_pnl << RST;
        else std::cout << " " << RED << "-$" << std::setprecision(4) << -pos_pnl << RST;
    }
    std::cout << CYN << "  ║\n";

    // Trades header
    std::cout << "╠════════════════════════════════════════════════════════════════════════╣\n"
              << "║  " << BLD << "RECENT TRADES" << RST << CYN
              << "                                                       ║\n"
              << "╠════════════════════════════════════════════════════════════════════════╣" << RST << "\n";

    // Trade list
    auto trades = trader.recent_trades(10);
    for (const auto& t : trades) {
        time_t sec = t.timestamp_ms / 1000;
        struct tm* tm = localtime(&sec);

        std::cout << CYN << "║ " << RST;
        std::cout << DIM << std::setfill('0')
                  << std::setw(2) << tm->tm_hour << ":"
                  << std::setw(2) << tm->tm_min << ":"
                  << std::setw(2) << tm->tm_sec << RST << " ";

        // Action with color
        if (t.action.find("BUY") != std::string::npos) {
            std::cout << GRN << std::setw(9) << std::left << t.action << RST;
        } else {
            std::cout << RED << std::setw(9) << std::left << t.action << RST;
        }

        std::cout << " [" << std::setw(5) << t.reason << "]";
        std::cout << " @" << std::setprecision(4) << std::fixed << t.price;

        // P&L
        if (t.pnl >= 0) {
            std::cout << "  " << GRN << "+$" << std::setprecision(4) << t.pnl << RST;
        } else {
            std::cout << "  " << RED << "-$" << std::setprecision(4) << -t.pnl << RST;
        }

        // Cumulative
        std::cout << "  │ Tot: ";
        if (t.cumulative >= 0) {
            std::cout << GRN << "+$" << std::setprecision(2) << t.cumulative << RST;
        } else {
            std::cout << RED << "-$" << std::setprecision(2) << -t.cumulative << RST;
        }

        std::cout << CYN << " ║" << RST << "\n";
    }

    // Fill empty rows
    for (size_t i = trades.size(); i < 10; i++) {
        std::cout << CYN << "║" << RST << std::string(72, ' ') << CYN << "║" << RST << "\n";
    }

    // Footer
    std::cout << CYN
              << "╠════════════════════════════════════════════════════════════════════════╣\n"
              << "║  " << RST << DIM << "Strategy: Buy when BTC moves >15bps but market stale >500ms"
              << RST << CYN << "      ║\n"
              << "║  " << RST << DIM << "Binance: " << g_btc_updates.load() << " msgs | Ctrl+C to stop"
              << RST << CYN << "                                  ║\n"
              << "╚════════════════════════════════════════════════════════════════════════╝"
              << RST << "\n";

    std::cout << std::flush;
}

// ═══════════════════════════════════════════════════════════════════════════
// Main
// ═══════════════════════════════════════════════════════════════════════════
int main(int argc, char* argv[]) {
    int duration = 300;  // 5 minutes default
    if (argc > 1) duration = std::atoi(argv[1]);

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();

    std::thread binance(binance_thread);

    StaleOddsTrader trader;

    // Wait for initial BTC price
    std::cout << "Connecting to Binance WebSocket..." << std::flush;
    for (int i = 0; i < 50 && g_btc_price.load() == 0 && g_running; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::cout << "." << std::flush;
    }
    std::cout << "\n";

    if (g_btc_price.load() == 0) {
        std::cout << YEL << "Warning: No BTC price, using simulated data\n" << RST;
        g_price_history.record(104500.0);
    }

    // Initialize market based on current BTC
    trader.initialize_market();

    auto start_time = std::chrono::steady_clock::now();

    // Main loop
    while (g_running) {
        auto elapsed = std::chrono::steady_clock::now() - start_time;
        if (std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() >= duration) {
            break;
        }

        trader.tick();
        render_ui(trader);

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    g_running = false;
    binance.join();

    // Exit any open position
    trader.force_exit();

    // Final summary
    double true_pnl = trader.true_pnl();

    std::cout << CLR;
    std::cout << CYN << BLD << "\n"
              << "╔════════════════════════════════════════════════════════════════════════╗\n"
              << "║                      S1 STALE ODDS - SESSION COMPLETE                  ║\n"
              << "╚════════════════════════════════════════════════════════════════════════╝\n" << RST;

    std::cout << "\n  Starting Balance:  $50.00\n";
    std::cout << "  Final Balance:     $" << std::fixed << std::setprecision(2) << trader.balance() << "\n";
    std::cout << "  Signals Generated: " << trader.signals() << "\n";
    std::cout << "  Total Trades:      " << trader.trade_count() << "\n";
    std::cout << "  Total Fees:        $" << std::setprecision(4) << trader.fees() << "\n";

    if (true_pnl >= 0) {
        std::cout << GRN << BLD << "\n  NET PROFIT:        +$" << std::setprecision(2) << true_pnl << RST << "\n";
        std::cout << GRN << "  Return:            +" << std::setprecision(1) << (true_pnl / 50.0 * 100) << "%" << RST << "\n";
    } else {
        std::cout << RED << BLD << "\n  NET LOSS:          -$" << std::setprecision(2) << -true_pnl << RST << "\n";
        std::cout << RED << "  Return:            " << std::setprecision(1) << (true_pnl / 50.0 * 100) << "%" << RST << "\n";
    }

    std::cout << "\n";
    return 0;
}
