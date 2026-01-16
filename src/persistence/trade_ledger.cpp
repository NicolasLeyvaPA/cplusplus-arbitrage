#include "persistence/trade_ledger.hpp"
#include "utils/time_utils.hpp"
#include <spdlog/spdlog.h>
#include <filesystem>
#include <fstream>

namespace arb {

// JSON serialization helpers

void to_json(nlohmann::json& j, const Fill& f) {
    j = nlohmann::json{
        {"order_id", f.order_id},
        {"trade_id", f.trade_id},
        {"market_id", f.market_id},
        {"token_id", f.token_id},
        {"side", side_to_string(f.side)},
        {"price", f.price},
        {"size", f.size},
        {"notional", f.notional},
        {"fee", f.fee},
        {"fill_time", time_utils::to_iso8601(
            std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                std::chrono::system_clock::now() +
                std::chrono::duration_cast<std::chrono::system_clock::duration>(
                    f.fill_time - now())))},
        {"exchange_time_ms", f.exchange_time_ms}
    };
}

void from_json(const nlohmann::json& j, Fill& f) {
    f.order_id = j.value("order_id", "");
    f.trade_id = j.value("trade_id", "");
    f.market_id = j.value("market_id", "");
    f.token_id = j.value("token_id", "");
    std::string side_str = j.value("side", "BUY");
    f.side = (side_str == "SELL") ? Side::SELL : Side::BUY;
    f.price = j.value("price", 0.0);
    f.size = j.value("size", 0.0);
    f.notional = j.value("notional", 0.0);
    f.fee = j.value("fee", 0.0);
    f.exchange_time_ms = j.value("exchange_time_ms", 0LL);
}

void to_json(nlohmann::json& j, const Order& o) {
    j = nlohmann::json{
        {"client_order_id", o.client_order_id},
        {"exchange_order_id", o.exchange_order_id},
        {"strategy_name", o.strategy_name},
        {"market_id", o.market_id},
        {"token_id", o.token_id},
        {"side", side_to_string(o.side)},
        {"type", order_type_to_string(o.type)},
        {"price", o.price},
        {"original_size", o.original_size},
        {"filled_size", o.filled_size},
        {"remaining_size", o.remaining_size},
        {"state", order_state_to_string(o.state)},
        {"total_fees", o.total_fees},
        {"reject_reason", o.reject_reason}
    };
}

void from_json(const nlohmann::json& j, Order& o) {
    o.client_order_id = j.value("client_order_id", "");
    o.exchange_order_id = j.value("exchange_order_id", "");
    o.strategy_name = j.value("strategy_name", "");
    o.market_id = j.value("market_id", "");
    o.token_id = j.value("token_id", "");

    std::string side_str = j.value("side", "BUY");
    o.side = (side_str == "SELL") ? Side::SELL : Side::BUY;

    o.price = j.value("price", 0.0);
    o.original_size = j.value("original_size", 0.0);
    o.filled_size = j.value("filled_size", 0.0);
    o.remaining_size = j.value("remaining_size", 0.0);
    o.total_fees = j.value("total_fees", 0.0);
    o.reject_reason = j.value("reject_reason", "");
}

void to_json(nlohmann::json& j, const Signal& s) {
    j = nlohmann::json{
        {"strategy_name", s.strategy_name},
        {"market_id", s.market_id},
        {"token_id", s.token_id},
        {"side", side_to_string(s.side)},
        {"target_price", s.target_price},
        {"target_size", s.target_size},
        {"expected_edge", s.expected_edge},
        {"confidence", s.confidence},
        {"reason", s.reason}
    };
}

void from_json(const nlohmann::json& j, Signal& s) {
    s.strategy_name = j.value("strategy_name", "");
    s.market_id = j.value("market_id", "");
    s.token_id = j.value("token_id", "");
    std::string side_str = j.value("side", "BUY");
    s.side = (side_str == "SELL") ? Side::SELL : Side::BUY;
    s.target_price = j.value("target_price", 0.0);
    s.target_size = j.value("target_size", 0.0);
    s.expected_edge = j.value("expected_edge", 0.0);
    s.confidence = j.value("confidence", 0.0);
    s.reason = j.value("reason", "");
}

void to_json(nlohmann::json& j, const Position& p) {
    j = nlohmann::json{
        {"token_id", p.token_id},
        {"market_id", p.market_id},
        {"outcome_name", p.outcome_name},
        {"size", p.size},
        {"avg_entry_price", p.avg_entry_price},
        {"cost_basis", p.cost_basis},
        {"realized_pnl", p.realized_pnl},
        {"total_fees", p.total_fees},
        {"last_mark_price", p.last_mark_price},
        {"unrealized_pnl", p.unrealized_pnl}
    };
}

void from_json(const nlohmann::json& j, Position& p) {
    p.token_id = j.value("token_id", "");
    p.market_id = j.value("market_id", "");
    p.outcome_name = j.value("outcome_name", "");
    p.size = j.value("size", 0.0);
    p.avg_entry_price = j.value("avg_entry_price", 0.0);
    p.cost_basis = j.value("cost_basis", 0.0);
    p.realized_pnl = j.value("realized_pnl", 0.0);
    p.total_fees = j.value("total_fees", 0.0);
    p.last_mark_price = j.value("last_mark_price", 0.0);
    p.unrealized_pnl = j.value("unrealized_pnl", 0.0);
}

// TradeLedger implementation

TradeLedger::TradeLedger(const std::string& path)
    : base_path_(path)
    , current_path_(path)
{
    open_file();
}

TradeLedger::~TradeLedger() {
    flush();
    if (file_.is_open()) {
        file_.close();
    }
}

void TradeLedger::open_file() {
    // Create directory if needed
    std::filesystem::path p(current_path_);
    if (p.has_parent_path()) {
        std::filesystem::create_directories(p.parent_path());
    }

    file_.open(current_path_, std::ios::app);
    if (!file_.is_open()) {
        spdlog::error("Failed to open trade ledger: {}", current_path_);
    } else {
        spdlog::info("Trade ledger opened: {}", current_path_);
    }
}

void TradeLedger::write_line(const nlohmann::json& j) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_.is_open()) {
        file_ << j.dump() << "\n";
    }
}

void TradeLedger::record_fill(const Fill& fill) {
    nlohmann::json j;
    j["event_type"] = "fill";
    j["timestamp"] = time_utils::now_iso8601();
    j["data"] = fill;
    write_line(j);
}

void TradeLedger::record_order(const Order& order) {
    nlohmann::json j;
    j["event_type"] = "order";
    j["timestamp"] = time_utils::now_iso8601();
    j["data"] = order;
    write_line(j);
}

void TradeLedger::record_signal(const Signal& signal) {
    nlohmann::json j;
    j["event_type"] = "signal";
    j["timestamp"] = time_utils::now_iso8601();
    j["data"] = signal;
    write_line(j);
}

void TradeLedger::record_position_snapshot(const Position& position) {
    nlohmann::json j;
    j["event_type"] = "position_snapshot";
    j["timestamp"] = time_utils::now_iso8601();
    j["data"] = position;
    write_line(j);
}

void TradeLedger::record_event(const std::string& event_type, const nlohmann::json& data) {
    nlohmann::json j;
    j["event_type"] = event_type;
    j["timestamp"] = time_utils::now_iso8601();
    j["data"] = data;
    write_line(j);
}

void TradeLedger::flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_.is_open()) {
        file_.flush();
    }
}

void TradeLedger::rotate() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (file_.is_open()) {
        file_.close();
    }

    // Create new filename with timestamp
    auto now_time = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm = *std::localtime(&now_time);

    std::ostringstream ss;
    ss << base_path_ << "." << std::put_time(&tm, "%Y%m%d_%H%M%S");
    current_path_ = ss.str();

    open_file();
}

size_t TradeLedger::file_size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_.is_open()) {
        return std::filesystem::file_size(current_path_);
    }
    return 0;
}

std::vector<Fill> TradeLedger::get_fills(WallClock start, WallClock end) const {
    std::vector<Fill> fills;

    std::ifstream file(current_path_);
    std::string line;

    while (std::getline(file, line)) {
        try {
            auto j = nlohmann::json::parse(line);
            if (j["event_type"] == "fill") {
                Fill fill;
                from_json(j["data"], fill);
                fills.push_back(fill);
            }
        } catch (...) {
            // Skip malformed lines
        }
    }

    return fills;
}

std::vector<Order> TradeLedger::get_orders(WallClock start, WallClock end) const {
    std::vector<Order> orders;

    std::ifstream file(current_path_);
    std::string line;

    while (std::getline(file, line)) {
        try {
            auto j = nlohmann::json::parse(line);
            if (j["event_type"] == "order") {
                Order order;
                from_json(j["data"], order);
                orders.push_back(order);
            }
        } catch (...) {
            // Skip malformed lines
        }
    }

    return orders;
}

TradeLedger::DailySummary TradeLedger::get_daily_summary(WallClock date) const {
    DailySummary summary;
    summary.date = date;

    auto fills = get_fills(date, date + std::chrono::hours(24));
    for (const auto& fill : fills) {
        summary.trades++;
        summary.volume += fill.notional;
        summary.fees += fill.fee;
    }

    return summary;
}

void TradeLedger::export_to_csv(const std::string& path, WallClock start, WallClock end) const {
    std::ofstream csv(path);
    csv << "timestamp,event_type,order_id,side,price,size,notional,fee\n";

    auto fills = get_fills(start, end);
    for (const auto& fill : fills) {
        csv << time_utils::to_iso8601(fill.exchange_time_ms) << ","
            << "fill,"
            << fill.order_id << ","
            << side_to_string(fill.side) << ","
            << fill.price << ","
            << fill.size << ","
            << fill.notional << ","
            << fill.fee << "\n";
    }

    spdlog::info("Exported {} fills to {}", fills.size(), path);
}

// StateSnapshot implementation

StateSnapshot::StateSnapshot(const std::string& path)
    : path_(path)
{
}

void StateSnapshot::save(
    const std::vector<Order>& open_orders,
    const std::vector<Position>& positions,
    double balance,
    double daily_pnl)
{
    std::lock_guard<std::mutex> lock(mutex_);

    nlohmann::json j;
    j["timestamp"] = time_utils::now_iso8601();
    j["balance"] = balance;
    j["daily_pnl"] = daily_pnl;

    nlohmann::json orders_json = nlohmann::json::array();
    for (const auto& order : open_orders) {
        nlohmann::json o;
        to_json(o, order);
        orders_json.push_back(o);
    }
    j["open_orders"] = orders_json;

    nlohmann::json positions_json = nlohmann::json::array();
    for (const auto& pos : positions) {
        nlohmann::json p;
        to_json(p, pos);
        positions_json.push_back(p);
    }
    j["positions"] = positions_json;

    // Create directory if needed
    std::filesystem::path p(path_);
    if (p.has_parent_path()) {
        std::filesystem::create_directories(p.parent_path());
    }

    std::ofstream file(path_);
    file << j.dump(2);

    spdlog::info("State snapshot saved: {} positions, {} orders",
                positions.size(), open_orders.size());
}

StateSnapshot::State StateSnapshot::load() const {
    std::lock_guard<std::mutex> lock(mutex_);

    State state;

    if (!std::filesystem::exists(path_)) {
        return state;
    }

    std::ifstream file(path_);
    nlohmann::json j;
    file >> j;

    state.balance = j.value("balance", 0.0);
    state.daily_pnl = j.value("daily_pnl", 0.0);

    if (j.contains("timestamp")) {
        state.timestamp = time_utils::from_iso8601(j["timestamp"].get<std::string>());
    }

    if (j.contains("open_orders")) {
        for (const auto& o : j["open_orders"]) {
            Order order;
            from_json(o, order);
            state.open_orders.push_back(order);
        }
    }

    if (j.contains("positions")) {
        for (const auto& p : j["positions"]) {
            Position pos;
            from_json(p, pos);
            state.positions.push_back(pos);
        }
    }

    state.valid = true;

    spdlog::info("State snapshot loaded: {} positions, {} orders",
                state.positions.size(), state.open_orders.size());

    return state;
}

bool StateSnapshot::has_recent_snapshot(Duration max_age) const {
    if (!std::filesystem::exists(path_)) {
        return false;
    }

    auto state = load();
    if (!state.valid) return false;

    auto age = std::chrono::system_clock::now() - state.timestamp;
    return age < max_age;
}

} // namespace arb
