#pragma once
// Mod-local presentation models. No native pointers, I/O, renderer or background
// threads. A display quote never reserves or authorizes physical resources.
#include "swml_v4.h"
#include "checked_math.h"
#include "json.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace swml4::presentation {
inline bool text_valid(std::string_view value, size_t limit) {
    return value.size() <= limit && swml_json::valid_utf8(value) &&
        std::none_of(value.begin(), value.end(), [](unsigned char c) { return c < 32 || c == 127; });
}
inline bool identifier(std::string_view value) {
    return !value.empty() && text_valid(value, 128);
}
inline std::string folded(std::string_view value) {
    std::string out(value);
    for (char& c : out) if (c >= 'A' && c <= 'Z') c = char(c + 'a' - 'A');
    return out; // ASCII case folding; UTF-8 bytes otherwise retained exactly.
}

struct Unit {
    std::string symbol;
    uint32_t decimals = 0; // quantity is an integer count of the smallest unit
    uint32_t minimum_decimals = 0;
    std::string decimal_mark = ".", group_mark = ",";
    bool operator==(const Unit&) const = default;
    bool valid() const {
        auto mark = [](const std::string& s, bool empty) {
            return (empty || !s.empty()) && text_valid(s, 4) &&
                std::none_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
        };
        return decimals <= 9 && minimum_decimals <= decimals && text_valid(symbol, 24) &&
            mark(decimal_mark, false) && mark(group_mark, true) && decimal_mark != group_mark;
    }
};
inline std::string quantity(uint64_t value, const Unit& unit) {
    if (!unit.valid()) throw std::invalid_argument("invalid display unit");
    uint64_t scale = 1;
    for (uint32_t i = 0; i < unit.decimals; ++i) scale *= 10;
    auto digits = std::to_string(value / scale);
    std::string result;
    for (size_t i = 0; i < digits.size(); ++i) {
        if (i && (digits.size() - i) % 3 == 0) result += unit.group_mark;
        result += digits[i];
    }
    if (unit.decimals) {
        auto fraction = std::to_string(value % scale);
        fraction.insert(0, unit.decimals - fraction.size(), '0');
        while (fraction.size() > unit.minimum_decimals && fraction.back() == '0') fraction.pop_back();
        if (!fraction.empty()) result += unit.decimal_mark + fraction;
    }
    if (!unit.symbol.empty()) result += " " + unit.symbol;
    return result;
}
struct Requirement {
    std::string id, label;
    uint64_t required = 0, available = 0;
    Unit unit;
    bool operator==(const Requirement&) const = default;
    uint64_t missing() const { return required > available ? required - available : 0; }
    std::string display() const { return label + ": " + quantity(available, unit) + " / " + quantity(required, unit); }
};
inline bool requirements_valid(const std::vector<Requirement>& rows) {
    if (rows.size() > 128) return false;
    std::unordered_map<std::string, bool> ids;
    for (const auto& row : rows)
        if (!identifier(row.id) || row.label.empty() || !text_valid(row.label, 128) ||
            !row.unit.valid() || !ids.emplace(row.id, true).second) return false;
    return true;
}

struct WorldKey {
    std::array<uint8_t, 16> id{};
    uint64_t generation = 0;
    bool operator==(const WorldKey&) const = default;
    bool valid() const {
        return generation && std::any_of(id.begin(), id.end(), [](uint8_t b) { return b != 0; });
    }
    static WorldKey from(const swml_world_context& context) {
        WorldKey key;
        if (context.size >= sizeof(context) && (context.fields & SWML_CONTEXT_WORLD_ID)) {
            std::copy(std::begin(context.world_id), std::end(context.world_id), key.id.begin());
            key.generation = context.generation;
        }
        return key;
    }
};

struct Row {
    std::string id, label, category, location, detail;
    uint64_t amount = 0;
    Unit unit;
    bool operator==(const Row&) const = default;
    std::string display() const { return label + "  " + quantity(amount, unit); }
};
enum class Order { supplied, label };
class Catalogue {
    inline static std::atomic<uint64_t> next_identity_{1};
    const uint64_t identity_ = next_identity_.fetch_add(1, std::memory_order_relaxed);
    std::vector<Row> rows_;
    std::vector<std::string> searches_, labels_;
    std::unordered_map<std::string, size_t> ids_;
    std::vector<size_t> filtered_;
    std::string query_, category_, selected_;
    size_t page_ = 0, page_size_;
    Order order_ = Order::label;
    uint64_t revision_ = 0, view_ = 1;
    void clear_hidden_selection() {
        if (selected_.empty()) return;
        bool found = false;
        for (size_t i = 0; i < visible_size(); ++i) if (visible(i)->id == selected_) found = true;
        if (!found) selected_.clear();
    }
    void rebuild(bool follow_selection = false) {
        filtered_.clear();
        for (size_t i = 0; i < rows_.size(); ++i)
            if ((category_.empty() || rows_[i].category == category_) &&
                (query_.empty() || searches_[i].find(query_) != std::string::npos)) filtered_.push_back(i);
        if (order_ == Order::label) std::sort(filtered_.begin(), filtered_.end(), [&](size_t a, size_t b) {
            return labels_[a] == labels_[b] ? rows_[a].id < rows_[b].id : labels_[a] < labels_[b];
        });
        page_ = std::min(page_, pages() - 1);
        if (follow_selection && !selected_.empty()) for (size_t i = 0; i < filtered_.size(); ++i)
            if (rows_[filtered_[i]].id == selected_) { page_ = i / page_size_; break; }
        clear_hidden_selection();
        ++view_;
    }
public:
    explicit Catalogue(size_t page_size = 8) : page_size_(page_size) {
        if (!page_size || page_size > 16) throw std::invalid_argument("page size must be 1..16");
    }
    Catalogue(const Catalogue&) = delete;
    Catalogue& operator=(const Catalogue&) = delete;
    void reset() {
        rows_.clear(); ids_.clear(); searches_.clear(); labels_.clear(); filtered_.clear();
        query_.clear(); category_.clear(); selected_.clear(); page_ = 0; revision_ = 0; ++view_;
    }
    swml_result replace(std::vector<Row> rows, uint64_t revision) {
        if (!revision || revision < revision_) return SWML_CONFLICT;
        if (rows.size() > 4096) return SWML_LIMIT;
        if (revision == revision_) return rows == rows_ ? SWML_OK : SWML_CONFLICT;
        std::unordered_map<std::string, size_t> ids;
        std::vector<std::string> searches, labels;
        size_t bytes = 0;
        for (size_t i = 0; i < rows.size(); ++i) {
            const auto& row = rows[i];
            if (!identifier(row.id) || row.label.empty() || !text_valid(row.label, 128) ||
                !text_valid(row.category, 96) || !text_valid(row.location, 128) ||
                !text_valid(row.detail, 512) || !row.unit.valid() || !ids.emplace(row.id, i).second) return SWML_INVALID;
            bytes += row.id.size() + row.label.size() + row.category.size() + row.location.size() + row.detail.size();
            if (bytes > 4 * 1024 * 1024) return SWML_LIMIT;
            searches.push_back(folded(row.label + " " + row.category + " " + row.location + " " + row.detail));
            labels.push_back(folded(row.label));
        }
        rows_ = std::move(rows); ids_ = std::move(ids); searches_ = std::move(searches); labels_ = std::move(labels);
        revision_ = revision; rebuild(true); return SWML_OK;
    }
    swml_result search(std::string_view query) {
        if (!text_valid(query, 128)) return SWML_INVALID;
        auto value = folded(query);
        if (query_ != value) { query_ = std::move(value); page_ = 0; rebuild(); }
        return SWML_OK;
    }
    swml_result category(std::string_view category) {
        if (!text_valid(category, 96)) return SWML_INVALID;
        if (category_ != category) { category_ = category; page_ = 0; rebuild(); }
        return SWML_OK;
    }
    void order(Order value) { if (order_ != value) { order_ = value; rebuild(true); } }
    bool page(size_t index) {
        if (index >= pages() || index == page_) return false;
        page_ = index; clear_hidden_selection(); ++view_; return true;
    }
    bool select(std::string_view id) {
        for (size_t i = 0; i < visible_size(); ++i) if (visible(i)->id == id) {
            if (selected_ != id) { selected_ = id; ++view_; }
            return true;
        }
        return false;
    }
    const Row* selected() const {
        auto it = ids_.find(selected_); return it == ids_.end() ? nullptr : &rows_[it->second];
    }
    const Row* visible(size_t offset) const {
        size_t at = page_ * page_size_ + offset;
        return offset < page_size_ && at < filtered_.size() ? &rows_[filtered_[at]] : nullptr;
    }
    size_t visible_size() const { return std::min(page_size_, filtered_.size() - page_ * page_size_); }
    size_t count() const { return filtered_.size(); }
    size_t pages() const { return std::max(size_t(1), (filtered_.size() + page_size_ - 1) / page_size_); }
    size_t page() const { return page_; }
    size_t page_size() const { return page_size_; }
    uint64_t revision() const { return revision_; }
    uint64_t view() const { return view_; }
    uint64_t identity() const { return identity_; }
};

enum class ActionPhase { empty, ready, blocked, waiting, uncertain, succeeded, failed, stale };
enum class Outcome { succeeded, failed, unknown };
struct Quote {
    std::string id; // provider's quote ID; not a durable economic operation ID
    WorldKey world;
    uint64_t stock_revision = 0, expires_ms = 0, revision = 0;
    std::vector<Requirement> requirements;
    bool operator==(const Quote&) const = default;
};
class ActionGate {
    ActionPhase phase_ = ActionPhase::empty;
    Quote quote_;
    WorldKey world_;
    uint64_t observed_stock_ = 0, next_ = 1, pending_ = 0, deadline_ = 0, last_quote_ = 0;
    std::string reason_;
public:
    void observe(WorldKey world, uint64_t stock_revision, uint64_t now) {
        if (world != world_) {
            world_ = world; quote_ = {}; phase_ = ActionPhase::empty;
            pending_ = 0; deadline_ = 0; last_quote_ = 0; reason_.clear();
        }
        observed_stock_ = stock_revision;
        if (phase_ == ActionPhase::waiting && now >= deadline_) {
            phase_ = ActionPhase::uncertain; reason_ = "Checking the previous request";
        }
        if ((phase_ == ActionPhase::ready || phase_ == ActionPhase::blocked) &&
            (now >= quote_.expires_ms || observed_stock_ != quote_.stock_revision)) {
            phase_ = ActionPhase::stale; reason_ = "Refresh the offer";
        }
    }
    swml_result offer(Quote quote, uint64_t now) {
        if (pending_) return SWML_PENDING;
        if (!identifier(quote.id) || !requirements_valid(quote.requirements) ||
            !std::any_of(quote.requirements.begin(), quote.requirements.end(), [](const auto& r) { return r.required != 0; })) return SWML_INVALID;
        if (!world_.valid() || quote.world != world_ || !observed_stock_ || quote.revision < last_quote_ ||
            quote.stock_revision != observed_stock_ || now >= quote.expires_ms) return SWML_CONFLICT;
        if (quote.revision == last_quote_) return
            (phase_ == ActionPhase::ready || phase_ == ActionPhase::blocked) && quote == quote_ ? SWML_OK : SWML_CONFLICT;
        bool missing = std::any_of(quote.requirements.begin(), quote.requirements.end(), [](const auto& r) { return r.missing() != 0; });
        last_quote_ = quote.revision; quote_ = std::move(quote); phase_ = missing ? ActionPhase::blocked : ActionPhase::ready;
        reason_ = missing ? "More materials are needed" : ""; return SWML_OK;
    }
    swml_result begin(uint64_t now, uint64_t timeout_ms, uint64_t& ticket) {
        ticket = 0;
        observe(world_, observed_stock_, now);
        if (pending_) return SWML_PENDING;
        if (phase_ != ActionPhase::ready) return SWML_DENIED;
        if (!timeout_ms || timeout_ms > 60000 || !swml_math::add(now, timeout_ms, deadline_)) return SWML_INVALID;
        if (!next_) return SWML_LIMIT;
        pending_ = next_++; ticket = pending_; phase_ = ActionPhase::waiting;
        reason_ = "Waiting for confirmation"; return SWML_OK;
    }
    swml_result complete(uint64_t ticket, WorldKey world, Outcome outcome, std::string message) {
        if (!text_valid(message, 512)) return SWML_INVALID;
        if (!ticket || ticket != pending_ || world != world_) return SWML_CONFLICT;
        if (outcome == Outcome::unknown) { phase_ = ActionPhase::uncertain; reason_ = message.empty() ? "Checking the previous request" : std::move(message); return SWML_PENDING; }
        if (outcome != Outcome::succeeded && outcome != Outcome::failed) return SWML_INVALID;
        pending_ = 0; phase_ = outcome == Outcome::succeeded ? ActionPhase::succeeded : ActionPhase::failed;
        reason_ = message.empty() ? (outcome == Outcome::succeeded ? "Request completed" : "Request was declined") : std::move(message); return SWML_OK;
    }
    swml_result dismiss() {
        if (pending_) return SWML_PENDING; // hiding a panel never cancels a host operation
        phase_ = ActionPhase::empty; quote_ = {}; reason_.clear(); return SWML_OK;
    }
    ActionPhase phase() const { return phase_; }
    bool can_submit() const { return phase_ == ActionPhase::ready; } // call observe first
    uint64_t pending() const { return pending_; }
    const std::string& reason() const { return reason_; } // default English; caller may translate by phase
    const Quote& quote() const { return quote_; }
};

enum class Severity : uint32_t { information, success, warning, error };
struct Notice {
    std::string key, text;
    Severity severity;
    uint64_t expires_ms, sequence;
    uint32_t repeats = 1;
};
// Bounded cross-lane mailbox. Call activate on world changes before posting;
// old-world producers cannot resurrect notices after a switch.
class Notices {
    std::mutex mutex_;
    WorldKey world_;
    std::vector<Notice> rows_;
    uint64_t revision_ = 1, next_ = 1, next_expiry_ = std::numeric_limits<uint64_t>::max();
    void expire(uint64_t now) {
        if (now < next_expiry_) return;
        auto end = std::remove_if(rows_.begin(), rows_.end(), [&](const auto& n) { return now >= n.expires_ms; });
        if (end != rows_.end()) { rows_.erase(end, rows_.end()); ++revision_; }
        next_expiry_ = std::numeric_limits<uint64_t>::max();
        for (const auto& row : rows_) next_expiry_ = std::min(next_expiry_, row.expires_ms);
    }
public:
    void activate(WorldKey world) {
        std::lock_guard lock(mutex_);
        if (world == world_) return;
        world_ = world; rows_.clear(); next_expiry_ = std::numeric_limits<uint64_t>::max(); ++revision_;
    }
    swml_result post(WorldKey world, std::string key, std::string message, Severity severity, uint64_t now, uint64_t lifetime_ms = 5000) {
        uint64_t expiry = 0;
        if (!world.valid() || !identifier(key) || message.empty() || !text_valid(message, 512) ||
            severity > Severity::error || lifetime_ms < 250 || lifetime_ms > 60000 || !swml_math::add(now, lifetime_ms, expiry)) return SWML_INVALID;
        std::lock_guard lock(mutex_);
        if (world != world_) return SWML_CONFLICT;
        expire(now);
        auto found = std::find_if(rows_.begin(), rows_.end(), [&](const auto& row) { return row.key == key; });
        if (found != rows_.end()) {
            found->repeats = found->text == message ? std::min(999u, found->repeats + 1) : 1;
            found->text = std::move(message); found->severity = severity; found->expires_ms = expiry;
        } else {
            if (rows_.size() == 32) {
                auto victim = std::min_element(rows_.begin(), rows_.end(), [](const auto& a, const auto& b) {
                    return a.severity == b.severity ? a.sequence < b.sequence : a.severity < b.severity;
                });
                if (victim->severity > severity) return SWML_LIMIT;
                rows_.erase(victim);
            }
            rows_.push_back({std::move(key), std::move(message), severity, expiry, next_++});
        }
        next_expiry_ = std::min(next_expiry_, expiry); ++revision_; return SWML_OK;
    }
    swml_result dismiss(WorldKey world, std::string_view key) {
        std::lock_guard lock(mutex_);
        if (world != world_) return SWML_CONFLICT;
        auto found = std::find_if(rows_.begin(), rows_.end(), [&](const auto& row) { return row.key == key; });
        if (found == rows_.end()) return SWML_NOT_FOUND;
        rows_.erase(found); ++revision_; return SWML_OK;
    }
    // Unchanged frames allocate/copy nothing. Out contains at most three notices.
    swml_result snapshot(WorldKey world, uint64_t now, uint64_t& seen_revision, std::vector<Notice>& out) {
        std::lock_guard lock(mutex_);
        if (world != world_ || !world.valid()) return SWML_CONFLICT;
        expire(now);
        if (seen_revision == revision_) return SWML_PENDING;
        std::array<const Notice*, 32> ordered{}; size_t count = 0;
        for (const auto& row : rows_) ordered[count++] = &row;
        std::sort(ordered.begin(), ordered.begin() + count, [](const auto* a, const auto* b) {
            return a->severity == b->severity ? a->sequence < b->sequence : a->severity > b->severity;
        });
        out.clear();
        for (size_t i = 0; i < std::min(count, size_t(3)); ++i) out.push_back(*ordered[i]);
        seen_revision = revision_; return SWML_OK;
    }
};
} // namespace swml4::presentation
