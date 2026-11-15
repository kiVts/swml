#pragma once
#include <compare>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

namespace swml4::editor_edit {
// Source-level transaction machinery, not a native editor binding. A native
// adapter must validate its ABI, paint transfer, body lifetimes, history cursor
// behavior and every destruction path before using it in the game.
struct Action {
    uint64_t document = 0, epoch = 0, record = 0, serial = 0;
    auto operator<=>(const Action&) const = default;
};
struct Snapshot {
    uint64_t body = 0;
    int32_t x = 0, y = 0, z = 0;
    std::vector<uint8_t> data; // owned, complete native serialization; no borrowed game memory
};
struct Change { Snapshot before, after; };
enum class Direction { undo, redo };
enum class Result { ok, no_change, invalid, unavailable, failed, rollback_failed };

class Backend {
public:
    virtual ~Backend() = default;
    // Must check the owning editor thread, document/body generations, actual
    // record lifetime, ordinary-placement scope and that recording is suppressed.
    virtual bool ready(const Action&) const noexcept = 0;
    // Verify all identities, transfer world-face paint, and prepare everything
    // that can fail before the first write. This must not change the document.
    virtual bool validate(std::span<const Change>) noexcept = 0;
    // May fail after a partial write. The engine restores even the failed cell.
    // Preserve the body; never delete/recreate it as a side effect of a swap.
    virtual bool restore(const Snapshot&) noexcept = 0;
    virtual bool refresh(std::span<const Change>) noexcept = 0;
    // Native placement replay must itself be atomic on failure. The adapter
    // owns cursor movement and must leave/restore it on any non-ok result.
    virtual bool native_replay(Direction) noexcept = 0;
};

class Transactions {
    enum class State { applied, undone, poisoned };
    struct Entry { std::vector<Change> changes; State state = State::applied; };
    std::map<Action, Entry> entries_;
    bool busy_ = false;
    bool faulted_ = false;
    uint64_t fault_document_ = 0, fault_epoch_ = 0;
    void latch(const Action& action) {
        faulted_ = true; fault_document_ = action.document; fault_epoch_ = action.epoch;
    }
    struct Guard { bool& flag; explicit Guard(bool& value) : flag(value) { flag = true; } ~Guard() { flag = false; } };
    static bool valid(const Action& action, std::span<const Change> changes) {
        if (!action.document || !action.epoch || !action.record || !action.serial) return false;
        std::set<std::tuple<uint64_t, int32_t, int32_t, int32_t>> cells;
        for (auto& change : changes) {
            auto& a = change.before;
            auto& b = change.after;
            if (!a.body || a.body != b.body || a.x != b.x || a.y != b.y || a.z != b.z ||
                a.data.empty() || b.data.empty() || a.data == b.data) return false;
            if (!cells.emplace(a.body, a.x, a.y, a.z).second) return false;
        }
        return true;
    }
    static bool rollback(Backend& backend, std::span<const Change> changes, size_t attempted, bool to_before) {
        bool restored = true;
        while (attempted) {
            auto& change = changes[--attempted];
            // Do not short-circuit: attempt every restoration even after a fault.
            if (!backend.restore(to_before ? change.before : change.after)) restored = false;
        }
        if (!backend.refresh(changes)) restored = false;
        return restored;
    }
    static Result write(Backend& backend, std::span<const Change> changes, bool to_before) {
        for (size_t i = 0; i < changes.size(); ++i)
            if (!backend.restore(to_before ? changes[i].before : changes[i].after))
                return rollback(backend, changes, i + 1, !to_before) ? Result::failed : Result::rollback_failed;
        if (!backend.refresh(changes))
            return rollback(backend, changes, changes.size(), !to_before) ? Result::failed : Result::rollback_failed;
        return Result::ok;
    }
public:
    bool replaying() const { return busy_; }
    bool faulted() const { return faulted_; }
    size_t size() const { return entries_.size(); }
    // Call only when the real placement record and its paired checkpoint have
    // been identified. Commit the complementary edits once; do not append a
    // second native action. On failure the ordinary placement is left alone.
    Result commit(const Action& action, std::vector<Change> changes, Backend& backend) {
        if (busy_ || faulted_) return Result::unavailable;
        if (changes.empty()) return Result::no_change;
        if (!valid(action, changes) || entries_.contains(action)) return Result::invalid;
        Guard guard(busy_);
        if (!backend.ready(action) || !backend.validate(changes)) return Result::unavailable;
        // Allocate and take ownership before the first mutation.
        auto [it, inserted] = entries_.emplace(action, Entry{std::move(changes)});
        if (!inserted) return Result::invalid;
        auto result = write(backend, it->second.changes, false);
        if (result == Result::rollback_failed) { it->second.state = State::poisoned; latch(action); }
        else if (result != Result::ok) entries_.erase(it);
        return result;
    }
    Result replay(const Action& action, Direction direction, Backend& backend) {
        if (busy_) return Result::unavailable;
        auto it = entries_.find(action);
        if (it == entries_.end()) return Result::unavailable;
        auto& entry = it->second;
        if (entry.state == State::poisoned ||
            (direction == Direction::undo && entry.state != State::applied) ||
            (direction == Direction::redo && entry.state != State::undone)) return Result::invalid;
        if (faulted_) return Result::unavailable;
        Guard guard(busy_);
        if (!backend.ready(action) || !backend.validate(entry.changes)) return Result::unavailable;
        const bool undo = direction == Direction::undo;
        // Undo neighbors while placed pipes still pin their body. On redo,
        // restore the placed pipes first for the same reason.
        if (!undo && !backend.native_replay(direction)) return Result::failed;
        auto result = write(backend, entry.changes, undo);
        if (result != Result::ok) {
            if (!undo && !backend.native_replay(Direction::undo)) result = Result::rollback_failed;
            if (result == Result::rollback_failed) { entry.state = State::poisoned; latch(action); }
            return result;
        }
        if (undo && !backend.native_replay(direction)) {
            if (!rollback(backend, entry.changes, entry.changes.size(), false)) {
                entry.state = State::poisoned; latch(action); return Result::rollback_failed;
            }
            return Result::failed;
        }
        entry.state = undo ? State::undone : State::applied;
        return Result::ok;
    }
    // The native adapter must call these before freeing records, on redo
    // truncation, eviction, clear/load and editor teardown. Epoch and serial
    // are required even when a record address has been reused.
    bool discard(const Action& action) { return !busy_ && entries_.erase(action) != 0; }
    bool clear_document(uint64_t document, uint64_t epoch) {
        if (busy_) return false;
        std::erase_if(entries_, [&](auto& item) { return item.first.document == document && item.first.epoch == epoch; });
        // Only document teardown may clear a failed rollback. Discarding a
        // single history entry is not evidence that the document is repaired.
        if (faulted_ && fault_document_ == document && fault_epoch_ == epoch) faulted_ = false;
        return true;
    }
};
}
