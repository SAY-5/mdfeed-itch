#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "core/depth_book.h"
#include "core/parser.h"
#include "core/types.h"
#include "recovery/gap_detector.h"
#include "recovery/snapshot_client.h"
#include "sim/publisher.h"

namespace mdfeed::core {

// Per-message-type applied counters, indexed by MessageType. Add / Modify /
// Cancel / Execute carry different book-mutation costs, so the bench reports
// them separately.
struct PerTypeCounters {
    std::uint64_t add{0};      // 'A' + 'F'
    std::uint64_t execute{0};  // 'E' + 'C'
    std::uint64_t cancel{0};   // 'X'
    std::uint64_t delete_{0};  // 'D'
    std::uint64_t replace{0};  // 'U'
    std::uint64_t other{0};    // 'S' 'R' 'P'
};

struct FeedHandlerStats {
    std::uint64_t bytes_in{0};
    std::uint64_t datagrams_in{0};
    std::uint64_t messages_applied{0};
    std::uint64_t parse_errors{0};
    std::uint64_t gaps_detected{0};
    std::uint64_t snapshots_requested{0};
    std::uint64_t snapshots_applied{0};
    std::uint64_t stale_msgs{0};
    PerTypeCounters by_type{};
};

// FeedHandler decodes the transport header, runs gap detection, and applies
// the ITCH payload to the depth book. When a gap is detected it invokes the
// RecoveryRequester. Snapshots returned by the recovery path are applied via
// apply_snapshot().
class FeedHandler {
  public:
    using RecoveryRequester =
        std::function<bool(const itch::SnapshotRequest&, itch::SnapshotResponse&)>;

    explicit FeedHandler(std::size_t depth = itch::kDefaultDepth) : book_(depth) {}

    void set_recovery_requester(RecoveryRequester r) { requester_ = std::move(r); }

    // Apply one multicast datagram payload (transport header + ITCH frame).
    // Returns true if the payload was applied (or successfully recovered).
    bool on_datagram(const std::uint8_t* data, std::size_t len);

    // Force-apply a snapshot. Does not consult the gap detector.
    void apply_snapshot(const itch::SnapshotResponse& snap);

    const itch::DepthBook& book() const { return book_; }
    itch::DepthBook& book() { return book_; }
    const FeedHandlerStats& stats() const { return stats_; }
    itch::GapDetector& gap_detector() { return gap_; }

  private:
    itch::DepthBook book_;
    itch::GapDetector gap_;
    RecoveryRequester requester_;
    FeedHandlerStats stats_;
};

}  // namespace mdfeed::core
