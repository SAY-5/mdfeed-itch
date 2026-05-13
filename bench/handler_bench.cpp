// Single-threaded end-to-end bench. The publisher and handler share a process
// to keep the comparison fair (no network jitter); the bench reports parse +
// book-apply throughput on an unmeasured pass and per-message latency on a
// sampled pass so timer overhead does not dominate throughput.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "core/feed_handler.h"
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

void emit(FILE* out, std::uint64_t total, std::uint64_t applied, std::uint64_t elapsed_ns,
          const LatencyStats& lat) {
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
                 "  }\n"
                 "}\n",
                 (unsigned long long)total, (unsigned long long)applied,
                 (unsigned long long)elapsed_ns, tput, (unsigned long long)lat.min, lat.mean,
                 (unsigned long long)lat.p50, (unsigned long long)lat.p95,
                 (unsigned long long)lat.p99, (unsigned long long)lat.p999,
                 (unsigned long long)lat.max);
}

}  // namespace

int main(int argc, char** argv) {
    std::uint64_t count = 1'000'000;
    const char* out_path = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--count") && i + 1 < argc) {
            count = static_cast<std::uint64_t>(std::atoll(argv[++i]));
        } else if (!std::strcmp(argv[i], "--out") && i + 1 < argc) {
            out_path = argv[++i];
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

    // Pass 1: throughput. No per-message timing in the hot loop.
    {
        core::FeedHandler handler(10);
        const auto t0 = obs::now_ns();
        for (const auto& p : payloads) {
            handler.on_datagram(p.data(), p.size());
        }
        const auto t1 = obs::now_ns();
        const std::uint64_t elapsed = t1 - t0;

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

        emit(stdout, static_cast<std::uint64_t>(payloads.size()),
             handler.stats().messages_applied, elapsed, lat);

        if (out_path) {
            FILE* f = std::fopen(out_path, "w");
            if (f) {
                emit(f, static_cast<std::uint64_t>(payloads.size()),
                     handler.stats().messages_applied, elapsed, lat);
                std::fclose(f);
            }
        }
    }
    return 0;
}
