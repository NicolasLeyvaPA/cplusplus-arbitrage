#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <chrono>
#include <optional>
#include <filesystem>
#include <nlohmann/json.hpp>
#include "common/types.hpp"
#include "execution/order.hpp"

namespace arb {

/**
 * Position state for persistence.
 */
struct PersistedPosition {
    std::string market_id;
    std::string token_id;
    std::string outcome;  // "YES" or "NO"
    double size{0.0};
    double entry_price{0.0};
    double cost_basis{0.0};
    double unrealized_pnl{0.0};
    double realized_pnl{0.0};
    WallClock entry_time;
    WallClock last_update;
};

/**
 * Order state for persistence.
 */
struct PersistedOrder {
    std::string order_id;
    std::string client_order_id;
    std::string market_id;
    std::string token_id;
    Side side;
    OrderType order_type;
    OrderState state;
    double price{0.0};
    double size{0.0};
    double filled_size{0.0};
    WallClock created_at;
    WallClock last_update;
    std::optional<std::string> paired_order_id;
};

/**
 * Complete system state snapshot.
 */
struct SystemState {
    // Version for forward compatibility
    int version{2};

    // Core trading state
    std::vector<PersistedPosition> positions;
    std::vector<PersistedOrder> open_orders;

    // Balance and PnL
    double balance{0.0};
    double starting_balance{0.0};
    double daily_pnl{0.0};
    double total_pnl{0.0};
    double total_exposure{0.0};

    // Session info
    std::string session_id;
    WallClock session_start;
    WallClock last_save;
    int save_count{0};

    // Kill switch state
    bool kill_switch_active{false};
    std::string kill_switch_reason;

    // Statistics
    int total_orders{0};
    int total_fills{0};
    int total_cancels{0};
    double total_fees{0.0};
    double total_volume{0.0};

    // Validation
    bool is_valid() const;
    std::string validation_error() const;
};

// JSON serialization
void to_json(nlohmann::json& j, const PersistedPosition& p);
void from_json(const nlohmann::json& j, PersistedPosition& p);
void to_json(nlohmann::json& j, const PersistedOrder& o);
void from_json(const nlohmann::json& j, PersistedOrder& o);
void to_json(nlohmann::json& j, const SystemState& s);
void from_json(const nlohmann::json& j, SystemState& s);

/**
 * State manager handles all persistence operations.
 *
 * DESIGN:
 * - Writes state to disk on every fill (most critical event)
 * - Also writes periodically and on shutdown
 * - Maintains backup files for crash recovery
 * - Supports atomic writes via temp file + rename
 */
class StateManager {
public:
    struct Config {
        std::string state_dir{"./data"};
        std::string state_file{"state.json"};
        std::string backup_prefix{"state_backup_"};
        int max_backups{5};
        std::chrono::seconds auto_save_interval{30};
        bool compress_backups{false};
    };

    explicit StateManager(const Config& config = Config{});
    ~StateManager();

    // Initialize with starting balance
    void initialize(double starting_balance, const std::string& session_id = "");

    // State updates (call after each relevant event)
    void update_position(const PersistedPosition& position);
    void remove_position(const std::string& token_id);
    void update_order(const PersistedOrder& order);
    void remove_order(const std::string& order_id);
    void update_balance(double balance);
    void update_daily_pnl(double daily_pnl);
    void update_total_pnl(double total_pnl);
    void update_exposure(double exposure);
    void set_kill_switch(bool active, const std::string& reason = "");

    // Record statistics
    void record_order();
    void record_fill(double fee, double volume);
    void record_cancel();

    // Persistence operations
    bool save();                        // Save to primary file
    bool save_backup();                 // Save to backup file
    bool save_if_needed();              // Save if auto-save interval elapsed

    // Load operations
    std::optional<SystemState> load();  // Load from primary file
    std::optional<SystemState> load_latest_backup();
    std::optional<SystemState> load_best_available();  // Try primary, then backups

    // State queries
    SystemState current_state() const;
    bool has_unsaved_changes() const;
    WallClock last_save_time() const;
    std::string current_session_id() const;

    // File management
    std::vector<std::string> list_backups() const;
    void cleanup_old_backups();
    bool state_file_exists() const;
    size_t state_file_size() const;

    // Validation
    bool validate_state(const SystemState& state) const;
    std::string get_validation_errors(const SystemState& state) const;

private:
    Config config_;
    mutable std::mutex mutex_;

    SystemState state_;
    std::atomic<bool> dirty_{false};
    WallClock last_save_time_;

    std::string state_path() const;
    std::string backup_path(int index) const;
    std::string temp_path() const;

    bool write_atomic(const std::string& path, const SystemState& state);
    std::optional<SystemState> read_file(const std::string& path) const;
    void rotate_backups();
};

/**
 * RAII helper for auto-saving state on scope exit.
 */
class StateSaveGuard {
public:
    explicit StateSaveGuard(StateManager& manager) : manager_(manager) {}
    ~StateSaveGuard() { manager_.save(); }

    // Non-copyable
    StateSaveGuard(const StateSaveGuard&) = delete;
    StateSaveGuard& operator=(const StateSaveGuard&) = delete;

private:
    StateManager& manager_;
};

} // namespace arb
