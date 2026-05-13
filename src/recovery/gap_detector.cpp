#include "recovery/gap_detector.h"

namespace mdfeed::itch {

GapDetector::ObserveResult GapDetector::observe(StockLocate loc, SequenceNumber seq) {
    ObserveResult r{};
    auto it = expected_.find(loc);
    if (it == expected_.end()) {
        expected_[loc] = seq + 1;
        r.ok = true;
        return r;
    }
    const SequenceNumber expected = it->second;
    if (seq == expected) {
        it->second = expected + 1;
        r.ok = true;
        return r;
    }
    if (seq < expected) {
        r.is_stale = true;
        return r;
    }
    // seq > expected: gap.
    r.is_gap = true;
    r.gap.stock_locate = loc;
    r.gap.expected = expected;
    r.gap.received = seq;
    // Do not advance expected_; the recovery path is responsible for advancing
    // it via set_expected() once the gap-fill has been applied.
    return r;
}

}  // namespace mdfeed::itch
