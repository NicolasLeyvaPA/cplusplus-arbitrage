#include "core/state_manager.hpp"
#include <spdlog/spdlog.h>
#include <fstream>
#include <iomanip>
#include <random>
#include <algorithm>

namespace arb {

// JSON serialization implementations
void to_json(nlohmann::json& j, const PersistedPosition& p) {
    j = nlohmann::json{
        {"market_id", p.market_id},
        {"token_id", p.token_id},
        {"outcome", p.outcome},
        {"size", p.size},
        {"entry_price", p.entry_price},
        {"cost_basis", p.cost_basis},
        {"unrealized_pnl", p.unrealized_pnl},
        {"realized_pnl", p.realized_pnl},
        {"entry_time", std::chrono::duration_cast<std::chrono::milliseconds>(
            p.entry_time.time_since_epoch()).count()},
        {"last_update", std::chrono::duration_cast<std::chrono::milliseconds>(
            p.last_update.time_since_epoch()).count()}
    };
}

void from_json(const nlohmann::json& j, PersistedPosition& p) {
    j.at("market_id").get_to(p.market_id);
    j.at("token_id").get_to(p.token_id);
    j.at("outcome").get_to(p.outcome);
    j.at("size").get_to(p.size);
    j.at("entry_price").get_to(p.entry_price);
    j.at("cost_basis").get_to(p.cost_basis);
    j.at("unrealized_pnl").get_to(p.unrealized_pnl);
    j.at("realized_pnl").get_to(p.realized_pnl);

    int64_t entry_ms = j.at("entry_time").get<int64_t>();
    p.entry_time = WallClock(std::chrono::milliseconds(entry_ms));

    int64_t update_ms = j.at("last_update").get<int64_t>();
    p.last_update = WallClock(std::chrono::milliseconds(update_ms));
}

void to_json(nlohmann::json& j, const PersistedOrder& o) {
    j = nlohmann::json{
        {"order_id", o.order_id},
        {"client_order_id", o.client_order_id},
        {"market_id", o.market_id},
        {"token_id", o.token_id},
        {"side", side_to_string(o.side)},
        {"order_type", order_type_to_string(o.order_type)},
        {"state", order_state_to_string(o.state)},
        {"price", o.price},
        {"size", o.size},
        {"filled_size", o.filled_size},
        {"created_at", std::chrono::duration_cast<std::chrono::milliseconds>(
            o.created_at.time_since_epoch()).count()},
        {"last_update", std::chrono::duration_cast<std::chrono::milliseconds>(
            o.last_update.time_since_epoch()).count()}
    };

    if (o.paired_order_id) {
        j["paired_order_id"] = *o.paired_order_id;
    }
}

void from_json(const nlohmann::json& j, PersistedOrder& o) {
    j.at("order_id").get_to(o.order_id);
    j.at("client_order_id").get_to(o.client_order_id);
    j.at("market_id").get_to(o.market_id);
    j.at("token_id").get_to(o.token_id);

    std::string side_str = j.at("side").get<std::string>();
    o.side = (side_str == "BUY") ? Side::BUY : Side::SELL;

    std::string type_str = j.at("order_type").get<std::string>();
    if (type_str == "LIMIT") o.order_type = OrderType::LIMIT;
    else if (type_str == "MARKET") o.order_type = OrderType::MARKET;
    else if (type_str == "IOC") o.order_type = OrderType::IOC;
    else if (type_str == "FOK") o.order_type = OrderType::FOK;
    else o.order_type = OrderType::GTC;

    std::string state_str = j.at("state").get<std::string>();
    if (state_str == "PENDING") o.state = OrderState::PENDING;
    else if (state_str == "SENT") o.state = OrderState::SENT;
    else if (state_str == "ACKNOWLEDGED") o.state = OrderState::ACKNOWLEDGED;
    else if (state_str == "PARTIAL") o.state = OrderState::PARTIAL;
    else if (state_str == "FILLED") o.state = OrderState::FILLED;
    else if (state_str == "CANCELED") o.state = OrderState::CANCELED;
    else if (state_str == "REJECTED") o.state = OrderState::REJECTED;
    else o.state = OrderState::EXPIRED;

    j.at("price").get_to(o.price);
    j.at("size").get_to(o.size);
    j.at("filled_size").get_to(o.filled_size);

    int64_t created_ms = j.at("created_at").get<int64_t>();
    o.created_at = WallClock(std::chrono::milliseconds(created_ms));

    int64_t update_ms = j.at("last_update").get<int64_t>();
    o.last_update = WallClock(std::chrono::milliseconds(update_ms));

    if (j.contains("paired_order_id") && !j["paired_order_id"].is_null()) {
        o.paired_order_id = j["paired_order_id"].get<std::string>();
    }
}

void to_json(nlohmann::json& j, const SystemState& s) {
    j = nlohmann::json{
        {"version", s.version},
        {"positions", s.positions},
        {"open_orders", s.open_orders},
        {"balance", s.balance},
        {"starting_balance", s.starting_balance},
        {"daily_pnl", s.daily_pnl},
        {"total_pnl", s.total_pnl},
        {"total_exposure", s.total_exposure},
        {"session_id", s.session_id},
        {"session_start", std::chrono::duration_cast<std::chrono::milliseconds>(
            s.session_start.time_since_epoch()).count()},
        {"last_save", std::chrono::duration_cast<std::chrono::milliseconds>(
            s.last_save.time_since_epoch()).count()},
        {"save_count", s.save_count},
        {"kill_switch_active", s.kill_switch_active},
        {"kill_switch_reason", s.kill_switch_reason},
        {"total_orders", s.total_orders},
        {"total_fills", s.total_fills},
        {"total_cancels", s.total_cancels},
        {"total_fees", s.total_fees},
        {"total_volume", s.total_volume}
    };
}

void from_json(const nlohmann::json& j, SystemState& s) {
    s.version = j.value("version", 1);

    if (j.contains("positions")) {
        s.positions = j["positions"].get<std::vector<PersistedPosition>>();
    }
    if (j.contains("open_orders")) {
        s.open_orders = j["open_orders"].get<std::vector<PersistedOrder>>();
    }

    s.balance = j.value("balance", 0.0);
    s.starting_balance = j.value("starting_balance", 0.0);
    s.daily_pnl = j.value("daily_pnl", 0.0);
    s.total_pnl = j.value("total_pnl", 0.0);
    s.total_exposure = j.value("total_exposure", 0.0);
    s.session_id = j.value("session_id", "");

    if (j.contains("session_start")) {
        int64_t ms = j["session_start"].get<int64_t>();
        s.session_start = WallClock(std::chrono::milliseconds(ms));
    }
    if (j.contains("last_save")) {
        int64_t ms = j["last_save"].get<int64_t>();
        s.last_save = WallClock(std::chrono::milliseconds(ms));
    }

    s.save_count = j.value("save_count", 0);
    s.kill_switch_active = j.value("kill_switch_active", false);
    s.kill_switch_reason = j.value("kill_switch_reason", "");
    s.total_orders = j.value("total_orders", 0);
    s.total_fills = j.value("total_fills", 0);
    s.total_cancels = j.value("total_cancels", 0);
    s.total_fees = j.value("total_fees", 0.0);
    s.total_volume = j.value("total_volume", 0.0);
}

bool SystemState::is_valid() const {
    return validation_error().empty();
}

std::string SystemState::validation_error() const {
    if (version < 1 || version > 10) {
        return "Invalid version number";
    }
    if (balance < 0) {
        return "Negative balance";
    }
    if (starting_balance <= 0) {
        return "Invalid starting balance";
    }
    if (total_exposure < 0) {
        return "Negative exposure";
    }

    // Check position consistency
    double position_exposure = 0;
    for (const auto& pos : positions) {
        if (pos.size < 0) return "Negative position size";
        position_exposure += pos.size * pos.entry_price;
    }

    return "";
}

// StateManager implementation
StateManager::StateManager(const Config& config)
    : config_(config)
    , last_save_time_(wall_now())
{
    // Ensure state directory exists
    std::filesystem::create_directories(config_.state_dir);
    spdlog::info("StateManager initialized: dir={}", config_.state_dir);
}

StateManager::~StateManager() {
    if (dirty_.load()) {
        save();
    }
}

void StateManager::initialize(double starting_balance, const std::string& session_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    state_.starting_balance = starting_balance;
    state_.balance = starting_balance;
    state_.session_start = wall_now();

    if (session_id.empty()) {
        // Generate session ID
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(0, 15);
        const char* hex = "0123456789abcdef";
        std::string id;
        for (int i = 0; i < 8; ++i) {
            id += hex[dis(gen)];
        }
        state_.session_id = id;
    } else {
        state_.session_id = session_id;
    }

    dirty_.store(true);
    spdlog::info("State initialized: session={}, balance=${:.2f}",
                 state_.session_id, starting_balance);
}

void StateManager::update_position(const PersistedPosition& position) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = std::find_if(state_.positions.begin(), state_.positions.end(),
                           [&](const auto& p) { return p.token_id == position.token_id; });

    if (it != state_.positions.end()) {
        *it = position;
    } else {
        state_.positions.push_back(position);
    }

    dirty_.store(true);
}

void StateManager::remove_position(const std::string& token_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    state_.positions.erase(
        std::remove_if(state_.positions.begin(), state_.positions.end(),
                       [&](const auto& p) { return p.token_id == token_id; }),
        state_.positions.end()
    );

    dirty_.store(true);
}

void StateManager::update_order(const PersistedOrder& order) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = std::find_if(state_.open_orders.begin(), state_.open_orders.end(),
                           [&](const auto& o) { return o.order_id == order.order_id; });

    if (it != state_.open_orders.end()) {
        *it = order;
    } else {
        state_.open_orders.push_back(order);
    }

    dirty_.store(true);
}

void StateManager::remove_order(const std::string& order_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    state_.open_orders.erase(
        std::remove_if(state_.open_orders.begin(), state_.open_orders.end(),
                       [&](const auto& o) { return o.order_id == order_id; }),
        state_.open_orders.end()
    );

    dirty_.store(true);
}

void StateManager::update_balance(double balance) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_.balance = balance;
    dirty_.store(true);
}

void StateManager::update_daily_pnl(double daily_pnl) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_.daily_pnl = daily_pnl;
    dirty_.store(true);
}

void StateManager::update_total_pnl(double total_pnl) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_.total_pnl = total_pnl;
    dirty_.store(true);
}

void StateManager::update_exposure(double exposure) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_.total_exposure = exposure;
    dirty_.store(true);
}

void StateManager::set_kill_switch(bool active, const std::string& reason) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_.kill_switch_active = active;
    state_.kill_switch_reason = reason;
    dirty_.store(true);

    // Always save immediately on kill switch change
    save();
}

void StateManager::record_order() {
    std::lock_guard<std::mutex> lock(mutex_);
    state_.total_orders++;
    dirty_.store(true);
}

void StateManager::record_fill(double fee, double volume) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_.total_fills++;
    state_.total_fees += fee;
    state_.total_volume += volume;
    dirty_.store(true);

    // Save immediately on fills (critical event)
}

void StateManager::record_cancel() {
    std::lock_guard<std::mutex> lock(mutex_);
    state_.total_cancels++;
    dirty_.store(true);
}

bool StateManager::save() {
    SystemState state_copy;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.last_save = wall_now();
        state_.save_count++;
        state_copy = state_;
    }

    bool success = write_atomic(state_path(), state_copy);

    if (success) {
        dirty_.store(false);
        last_save_time_ = wall_now();
        spdlog::debug("State saved: save_count={}", state_copy.save_count);
    }

    return success;
}

bool StateManager::save_backup() {
    rotate_backups();

    SystemState state_copy;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_copy = state_;
        state_copy.last_save = wall_now();
    }

    return write_atomic(backup_path(0), state_copy);
}

bool StateManager::save_if_needed() {
    if (!dirty_.load()) return true;

    auto elapsed = wall_now() - last_save_time_;
    if (elapsed >= config_.auto_save_interval) {
        return save();
    }

    return true;
}

std::optional<SystemState> StateManager::load() {
    return read_file(state_path());
}

std::optional<SystemState> StateManager::load_latest_backup() {
    auto backups = list_backups();
    if (backups.empty()) {
        return std::nullopt;
    }

    // Backups are sorted newest first
    return read_file(backups[0]);
}

std::optional<SystemState> StateManager::load_best_available() {
    // Try primary first
    auto state = load();
    if (state && state->is_valid()) {
        spdlog::info("Loaded state from primary file");
        return state;
    }

    // Try backups
    auto backups = list_backups();
    for (const auto& backup : backups) {
        auto backup_state = read_file(backup);
        if (backup_state && backup_state->is_valid()) {
            spdlog::warn("Loaded state from backup: {}", backup);
            return backup_state;
        }
    }

    spdlog::warn("No valid state file found");
    return std::nullopt;
}

SystemState StateManager::current_state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

bool StateManager::has_unsaved_changes() const {
    return dirty_.load();
}

WallClock StateManager::last_save_time() const {
    return last_save_time_;
}

std::string StateManager::current_session_id() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_.session_id;
}

std::vector<std::string> StateManager::list_backups() const {
    std::vector<std::string> backups;

    for (int i = 0; i < config_.max_backups; ++i) {
        std::string path = backup_path(i);
        if (std::filesystem::exists(path)) {
            backups.push_back(path);
        }
    }

    // Sort by modification time (newest first)
    std::sort(backups.begin(), backups.end(),
              [](const std::string& a, const std::string& b) {
                  return std::filesystem::last_write_time(a) >
                         std::filesystem::last_write_time(b);
              });

    return backups;
}

void StateManager::cleanup_old_backups() {
    auto backups = list_backups();
    while (backups.size() > static_cast<size_t>(config_.max_backups)) {
        std::filesystem::remove(backups.back());
        backups.pop_back();
    }
}

bool StateManager::state_file_exists() const {
    return std::filesystem::exists(state_path());
}

size_t StateManager::state_file_size() const {
    if (!state_file_exists()) return 0;
    return std::filesystem::file_size(state_path());
}

bool StateManager::validate_state(const SystemState& state) const {
    return state.is_valid();
}

std::string StateManager::get_validation_errors(const SystemState& state) const {
    return state.validation_error();
}

std::string StateManager::state_path() const {
    return config_.state_dir + "/" + config_.state_file;
}

std::string StateManager::backup_path(int index) const {
    return config_.state_dir + "/" + config_.backup_prefix + std::to_string(index) + ".json";
}

std::string StateManager::temp_path() const {
    return state_path() + ".tmp";
}

bool StateManager::write_atomic(const std::string& path, const SystemState& state) {
    try {
        std::string temp = path + ".tmp";

        // Write to temp file
        std::ofstream file(temp);
        if (!file) {
            spdlog::error("Failed to open temp file: {}", temp);
            return false;
        }

        nlohmann::json j = state;
        file << std::setw(2) << j;
        file.close();

        if (!file) {
            spdlog::error("Failed to write to temp file: {}", temp);
            return false;
        }

        // Atomic rename
        std::filesystem::rename(temp, path);

        return true;

    } catch (const std::exception& e) {
        spdlog::error("State write failed: {}", e.what());
        return false;
    }
}

std::optional<SystemState> StateManager::read_file(const std::string& path) const {
    try {
        if (!std::filesystem::exists(path)) {
            return std::nullopt;
        }

        std::ifstream file(path);
        if (!file) {
            spdlog::error("Failed to open state file: {}", path);
            return std::nullopt;
        }

        nlohmann::json j;
        file >> j;

        SystemState state = j.get<SystemState>();

        if (!state.is_valid()) {
            spdlog::warn("Invalid state file: {}: {}", path, state.validation_error());
            return std::nullopt;
        }

        return state;

    } catch (const std::exception& e) {
        spdlog::error("Failed to read state file {}: {}", path, e.what());
        return std::nullopt;
    }
}

void StateManager::rotate_backups() {
    // Shift existing backups
    for (int i = config_.max_backups - 1; i > 0; --i) {
        std::string from = backup_path(i - 1);
        std::string to = backup_path(i);

        if (std::filesystem::exists(from)) {
            std::filesystem::rename(from, to);
        }
    }
}

} // namespace arb
