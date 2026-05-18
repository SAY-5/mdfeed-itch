// pcap_replay: replay a libpcap capture of ITCH multicast packets through the
// FeedHandler.
//
// Reads a libpcap-format .pcap of UDP-wrapped ITCH transport datagrams, strips
// the Ethernet/IPv4/UDP headers, and feeds each datagram to a FeedHandler. The
// final book state and the handler stats are printed as JSON.
//
// Flags:
//   --pcap <path>          input capture (required)
//   --speedup <N>          replay-rate multiplier vs. captured timestamps
//                          (default 1.0). N=0 means as-fast-as-possible.
//   --start-offset <secs>  skip packets in the first <secs> of capture time
//   --stop-after <count>   stop after replaying <count> datagrams
//   --skip-gaps            on a sequence gap, step past it and keep replaying
//                          (gap accounting, no recovery); reports each gap

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "core/feed_handler.h"
#include "pcap/pcap_io.h"

using namespace mdfeed;

int main(int argc, char** argv) {
    std::string pcap_path;
    double speedup = 1.0;
    double start_offset = 0.0;
    std::uint64_t stop_after = 0;  // 0 = unlimited
    bool skip_gaps = false;

    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--pcap") && i + 1 < argc) {
            pcap_path = argv[++i];
        } else if (!std::strcmp(argv[i], "--speedup") && i + 1 < argc) {
            speedup = std::strtod(argv[++i], nullptr);
        } else if (!std::strcmp(argv[i], "--start-offset") && i + 1 < argc) {
            start_offset = std::strtod(argv[++i], nullptr);
        } else if (!std::strcmp(argv[i], "--stop-after") && i + 1 < argc) {
            stop_after = static_cast<std::uint64_t>(std::atoll(argv[++i]));
        } else if (!std::strcmp(argv[i], "--skip-gaps")) {
            skip_gaps = true;
        } else {
            std::fprintf(stderr, "pcap_replay: unknown argument %s\n", argv[i]);
            return 2;
        }
    }
    if (pcap_path.empty()) {
        std::fprintf(stderr, "pcap_replay: --pcap <path> is required\n");
        return 2;
    }

    pcap::PcapReader reader;
    std::string err;
    if (!reader.open(pcap_path, &err)) {
        std::fprintf(stderr, "pcap_replay: %s\n", err.c_str());
        return 2;
    }

    core::FeedHandler handler(10);
    handler.set_skip_gaps(skip_gaps);

    struct GapRange {
        std::uint16_t loc;
        std::uint64_t first_missing;
        std::uint64_t last_missing;
    };
    std::vector<GapRange> gaps;
    handler.set_gap_observer(
        [&gaps](itch::StockLocate loc, itch::SequenceNumber first, itch::SequenceNumber last) {
            gaps.push_back({loc, first, last});
        });

    pcap::PcapRecord rec;
    std::uint64_t replayed = 0;
    std::uint64_t skipped = 0;
    bool have_first_ts = false;
    std::uint64_t first_ts_us = 0;
    std::uint64_t prev_cap_us = 0;
    const auto wall0 = std::chrono::steady_clock::now();

    while (reader.next(rec)) {
        if (!have_first_ts) {
            first_ts_us = rec.ts_us;
            have_first_ts = true;
            prev_cap_us = rec.ts_us;
        }
        // --start-offset: drop packets inside the leading window.
        const double rel_sec = static_cast<double>(rec.ts_us - first_ts_us) / 1e6;
        if (rel_sec < start_offset) {
            ++skipped;
            prev_cap_us = rec.ts_us;
            continue;
        }
        // --speedup: pace replay against captured inter-packet gaps.
        if (speedup > 0.0) {
            const std::uint64_t cap_delta_us =
                rec.ts_us > prev_cap_us ? rec.ts_us - prev_cap_us : 0;
            const double sleep_us = static_cast<double>(cap_delta_us) / speedup;
            if (sleep_us >= 1.0) {
                std::this_thread::sleep_for(
                    std::chrono::microseconds(static_cast<std::int64_t>(sleep_us)));
            }
        }
        prev_cap_us = rec.ts_us;

        handler.on_datagram(rec.payload.data(), rec.payload.size());
        ++replayed;
        if (stop_after != 0 && replayed >= stop_after) break;
    }

    const auto wall1 = std::chrono::steady_clock::now();
    const std::uint64_t wall_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(wall1 - wall0).count());

    const auto& s = handler.stats();
    std::printf(
        "{\n"
        "  \"pcap\": \"%s\",\n"
        "  \"speedup\": %.3f,\n"
        "  \"start_offset_sec\": %.3f,\n"
        "  \"skip_gaps\": %s,\n"
        "  \"replayed\": %llu,\n"
        "  \"skipped\": %llu,\n"
        "  \"wall_ns\": %llu,\n"
        "  \"datagrams_in\": %llu,\n"
        "  \"messages_applied\": %llu,\n"
        "  \"parse_errors\": %llu,\n"
        "  \"gaps_detected\": %llu,\n"
        "  \"stale_msgs\": %llu,\n"
        "  \"symbols\": %zu,\n"
        "  \"gap_ranges\": [",
        pcap_path.c_str(), speedup, start_offset, skip_gaps ? "true" : "false",
        (unsigned long long)replayed, (unsigned long long)skipped, (unsigned long long)wall_ns,
        (unsigned long long)s.datagrams_in, (unsigned long long)s.messages_applied,
        (unsigned long long)s.parse_errors, (unsigned long long)s.gaps_detected,
        (unsigned long long)s.stale_msgs, handler.book().symbol_count());
    for (std::size_t i = 0; i < gaps.size(); ++i) {
        std::printf(
            "%s\n    {\"stock_locate\": %u, \"first_missing\": %llu, \"last_missing\": %llu}",
            i == 0 ? "" : ",", gaps[i].loc, (unsigned long long)gaps[i].first_missing,
            (unsigned long long)gaps[i].last_missing);
    }
    std::printf("%s]\n}\n", gaps.empty() ? "" : "\n  ");
    return 0;
}
