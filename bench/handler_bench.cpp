// Single-threaded end-to-end bench. The publisher and handler share a process
// to keep the comparison fair (no network jitter); the bench reports parse +
// book-apply throughput on an unmeasured pass and per-message latency on a
// sampled pass so timer overhead does not dominate throughput.
//
// Flags:
//   --count N        number of generated ITCH messages (default 1,000,000)
//   --out PATH       write the JSON result to PATH in addition to stdout
//   --regress PATH   compare the current run against a baseline JSON at PATH
//                    and exit non-zero if throughput regressed beyond the
//                    drift threshold (see --drift)
//   --drift F        allowed fractional throughput drift (default 0.30)

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include "core/depth_book.h"
#include "core/feed_handler.h"
#include "core/parser.h"
#include "obs/clock.h"
#include "obs/histogram.h"
#include "sim/publisher.h"

using namespace mdfeed;

namespace {

struct LatencyStats {
    std::uint64_t min, max, p50, p95, p99, p999;
    double mean;
};

LatencyStats latency_from(const obs::Histogram& h) {
    return {h.min(),
            h.max(),
            h.percentile(0.50),
            h.percentile(0.95),
            h.percentile(0.99),
            h.percentile(0.999),
            h.mean()};
}

// Per-type throughput. Each message type carries a distinct book-mutation
// cost (Add inserts an order and may create a price level; Execute / Cancel
// mutate an existing level; Delete erases an order and may drop a level;
// Replace is a delete + add). To isolate that cost the bench groups the
// generated datagrams by type, then times a dedicated replay of each group
// against a handler pre-warmed with every Add so the dependent message types
// reference live orders.
struct TypeRate {
    const char* name;
    std::uint64_t applied;
    std::uint64_t elapsed_ns;
    double rate;
};

// Classify a transport-framed datagram by ITCH type code. The type code is
// the first byte of the ITCH frame, which sits two bytes past the transport
// header (after the 2-byte ITCH length prefix).
char datagram_type(const std::vector<std::uint8_t>& p) {
    const std::size_t off = sim::kTransportHeaderSize + 2;
    return off < p.size() ? static_cast<char>(p[off]) : '?';
}

void emit(FILE* out, std::uint64_t total, std::uint64_t applied, std::uint64_t elapsed_ns,
          const LatencyStats& lat, const core::PerTypeCounters& counts,
          const std::vector<TypeRate>& rates) {
    const double sec = static_cast<double>(elapsed_ns) / 1e9;
    const double tput = sec > 0 ? static_cast<double>(applied) / sec : 0.0;
    std::fprintf(out,
                 "{\n"
                 "  \"messages_total\": %llu,\n"
                 "  \"messages_applied\": %llu,\n"
                 "  \"elapsed_ns\": %llu,\n"
                 "  \"throughput_msgs_per_sec\": %.0f,\n"
                 "  \"latency_ns\": {\n"
                 "    \"min\": %llu,\n"
                 "    \"mean\": %.1f,\n"
                 "    \"p50\": %llu,\n"
                 "    \"p95\": %llu,\n"
                 "    \"p99\": %llu,\n"
                 "    \"p999\": %llu,\n"
                 "    \"max\": %llu\n"
                 "  },\n"
                 "  \"applied_by_type\": {\n"
                 "    \"add\": %llu,\n"
                 "    \"execute\": %llu,\n"
                 "    \"cancel\": %llu,\n"
                 "    \"delete\": %llu,\n"
                 "    \"replace\": %llu,\n"
                 "    \"other\": %llu\n"
                 "  },\n"
                 "  \"throughput_by_type\": {\n",
                 (unsigned long long)total, (unsigned long long)applied,
                 (unsigned long long)elapsed_ns, tput, (unsigned long long)lat.min, lat.mean,
                 (unsigned long long)lat.p50, (unsigned long long)lat.p95,
                 (unsigned long long)lat.p99, (unsigned long long)lat.p999,
                 (unsigned long long)lat.max, (unsigned long long)counts.add,
                 (unsigned long long)counts.execute, (unsigned long long)counts.cancel,
                 (unsigned long long)counts.delete_, (unsigned long long)counts.replace,
                 (unsigned long long)counts.other);
    for (std::size_t i = 0; i < rates.size(); ++i) {
        std::fprintf(out, "    \"%s\": %.0f%s\n", rates[i].name, rates[i].rate,
                     i + 1 < rates.size() ? "," : "");
    }
    std::fprintf(out, "  }\n}\n");
}

// Minimal scan of a JSON file for "throughput_msgs_per_sec": <number>.
bool read_baseline_throughput(const char* path, double& out) {
    FILE* f = std::fopen(path, "r");
    if (!f) return false;
    std::string text;
    char chunk[4096];
    std::size_t n;
    while ((n = std::fread(chunk, 1, sizeof(chunk), f)) > 0) text.append(chunk, n);
    std::fclose(f);
    const char* key = "\"throughput_msgs_per_sec\"";
    const auto pos = text.find(key);
    if (pos == std::string::npos) return false;
    const auto colon = text.find(':', pos);
    if (colon == std::string::npos) return false;
    out = std::strtod(text.c_str() + colon + 1, nullptr);
    return out > 0.0;
}

}  // namespace

int main(int argc, char** argv) {
    std::uint64_t count = 1'000'000;
    const char* out_path = nullptr;
    const char* regress_path = nullptr;
    double drift = 0.30;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--count") && i + 1 < argc) {
            count = static_cast<std::uint64_t>(std::atoll(argv[++i]));
        } else if (!std::strcmp(argv[i], "--out") && i + 1 < argc) {
            out_path = argv[++i];
        } else if (!std::strcmp(argv[i], "--regress") && i + 1 < argc) {
            regress_path = argv[++i];
        } else if (!std::strcmp(argv[i], "--drift") && i + 1 < argc) {
            drift = std::strtod(argv[++i], nullptr);
        }
    }

    sim::MessageGenerator gen(0xBEEF, 8);

    std::vector<std::vector<std::uint8_t>> payloads;
    payloads.reserve(count + 16);
    std::vector<std::uint8_t> buf;
    sim::TransportHeader hdr{};

    for (auto& d : gen.directory_messages()) payloads.push_back(std::move(d));
    for (std::uint64_t i = 0; i < count; ++i) {
        if (gen.next(buf, hdr)) payloads.push_back(buf);
    }

    double measured_tput = 0.0;
    {
        // Pass 1: throughput. No per-message timing in the hot loop.
        core::FeedHandler handler(10);
        const auto t0 = obs::now_ns();
        for (const auto& p : payloads) {
            handler.on_datagram(p.data(), p.size());
        }
        const auto t1 = obs::now_ns();
        const std::uint64_t elapsed = t1 - t0;
        const double sec = static_cast<double>(elapsed) / 1e9;
        measured_tput = sec > 0 ? static_cast<double>(handler.stats().messages_applied) / sec : 0.0;

        // Pass 2: latency on a smaller sample to keep timer cost bounded.
        obs::Histogram hist;
        core::FeedHandler handler2(10);
        const std::size_t sample = payloads.size() > 200'000 ? 200'000 : payloads.size();
        for (std::size_t i = 0; i < sample; ++i) {
            const auto a = obs::now_ns();
            handler2.on_datagram(payloads[i].data(), payloads[i].size());
            const auto b = obs::now_ns();
            hist.record(b - a);
        }
        const auto lat = latency_from(hist);

        // Pass 3: per-type throughput. Decode every frame once, group the
        // decoded messages by ITCH type, and time a dedicated apply pass per
        // group against a DepthBook pre-warmed with every Add so the
        // dependent types (Execute / Cancel / Delete / Replace) reference live
        // orders. This isolates each type's book-mutation cost.
        std::vector<itch::AnyMessage> adds, executes, cancels, deletes, replaces;
        for (const auto& p : payloads) {
            const char tc = datagram_type(p);
            const std::uint8_t* body = p.data() + sim::kTransportHeaderSize;
            const std::size_t body_len = p.size() - sim::kTransportHeaderSize;
            auto pr = itch::decode_frame(std::span<const std::uint8_t>(body, body_len));
            if (pr.status != itch::ParseStatus::Ok || !pr.message) continue;
            switch (tc) {
                case 'A':
                case 'F':
                    adds.push_back(*pr.message);
                    break;
                case 'E':
                case 'C':
                    executes.push_back(*pr.message);
                    break;
                case 'X':
                    cancels.push_back(*pr.message);
                    break;
                case 'D':
                    deletes.push_back(*pr.message);
                    break;
                case 'U':
                    replaces.push_back(*pr.message);
                    break;
                default:
                    break;
            }
        }

        auto time_group = [&](const std::vector<itch::AnyMessage>& group) -> TypeRate {
            // Fresh book per group; pre-warm with every Add so dependent
            // messages hit live orders. The pre-warm time is excluded.
            itch::DepthBook book(10);
            for (const auto& m : adds) book.apply(m);
            const auto g0 = obs::now_ns();
            for (const auto& m : group) {
                book.apply(m);
            }
            const auto g1 = obs::now_ns();
            const std::uint64_t el = g1 - g0;
            const double sc = static_cast<double>(el) / 1e9;
            const double r = sc > 0 ? static_cast<double>(group.size()) / sc : 0.0;
            return {"", group.size(), el, r};
        };

        // Adds are timed on their own fresh book (no pre-warm needed).
        TypeRate add_rate{};
        {
            itch::DepthBook book(10);
            const auto g0 = obs::now_ns();
            for (const auto& m : adds) book.apply(m);
            const auto g1 = obs::now_ns();
            const std::uint64_t el = g1 - g0;
            const double sc = static_cast<double>(el) / 1e9;
            add_rate = {"add", adds.size(), el,
                        sc > 0 ? static_cast<double>(adds.size()) / sc : 0.0};
        }
        TypeRate exec_rate = time_group(executes);
        exec_rate.name = "execute";
        TypeRate cancel_rate = time_group(cancels);
        cancel_rate.name = "cancel";
        TypeRate delete_rate = time_group(deletes);
        delete_rate.name = "delete";
        TypeRate replace_rate = time_group(replaces);
        replace_rate.name = "replace";

        std::vector<TypeRate> rates = {add_rate, exec_rate, cancel_rate, delete_rate, replace_rate};

        emit(stdout, static_cast<std::uint64_t>(payloads.size()), handler.stats().messages_applied,
             elapsed, lat, handler.stats().by_type, rates);

        if (out_path) {
            FILE* f = std::fopen(out_path, "w");
            if (f) {
                emit(f, static_cast<std::uint64_t>(payloads.size()),
                     handler.stats().messages_applied, elapsed, lat, handler.stats().by_type,
                     rates);
                std::fclose(f);
            }
        }
    }

    if (regress_path) {
        double baseline = 0.0;
        if (!read_baseline_throughput(regress_path, baseline)) {
            std::fprintf(stderr, "bench-regress: cannot read baseline throughput from %s\n",
                         regress_path);
            return 2;
        }
        const double floor = baseline * (1.0 - drift);
        std::fprintf(stderr,
                     "bench-regress: baseline=%.0f measured=%.0f floor=%.0f (drift=%.0f%%)\n",
                     baseline, measured_tput, floor, drift * 100.0);
        if (measured_tput < floor) {
            std::fprintf(stderr, "bench-regress: FAIL throughput regressed beyond threshold\n");
            return 1;
        }
        std::fprintf(stderr, "bench-regress: OK\n");
    }
    return 0;
}
