#pragma once

#include <cstdint>
#include <unordered_map>

#include "core/types.h"

namespace mdfeed::itch {

struct GapEvent {
    StockLocate stock_locate;
    SequenceNumber expected;
    SequenceNumber received;
};

// Per-stock-locate sequence tracking. Sequence numbers are 1-based; the first
// observed message must equal 1 (or whatever start() configures).
class GapDetector {
  public:
    void start(StockLocate loc, SequenceNumber first = 1) { expected_[loc] = first; }

    // observe() returns std::nullopt for in-order messages. For out-of-order
    // messages it returns a GapEvent describing the missing range. Duplicates
    // and stale messages return std::nullopt and are silently dropped.
    struct ObserveResult {
        bool ok;        // true if this message advanced the sequence
        bool is_gap;    // true if a gap was detected
        bool is_stale;  // true if this message was older than expected
        GapEvent gap;   // valid only if is_gap
    };
    ObserveResult observe(StockLocate loc, SequenceNumber seq);

    SequenceNumber expected(StockLocate loc) const {
        auto it = expected_.find(loc);
        return it == expected_.end() ? 0 : it->second;
    }

    // Force the expected next sequence after a recovery completes.
    void set_expected(StockLocate loc, SequenceNumber next) { expected_[loc] = next; }

    std::size_t tracked_streams() const { return expected_.size(); }

  private:
    std::unordered_map<StockLocate, SequenceNumber> expected_;
};

}  // namespace mdfeed::itch
