// Multicast loopback scaling bench.
//
// Pushes N transport-framed ITCH datagrams through a real UDP multicast group
// on 127.0.0.1, from a sender thread to a receiver thread, and reports how
// many datagrams the receiver actually delivered to the FeedHandler. This
// measures the loopback datagram-delivery ceiling, which is distinct from the
// in-process parse+apply throughput reported by handler_bench.
//
// UDP is lossy: an un-throttled 1M-datagram burst will overrun the receiver's
// socket buffer and drop. The bench therefore reports the delivered fraction
// honestly rather than asserting zero loss. A --pace-us flag inserts a small
// per-batch sleep so the operator can trade wall time for delivery rate.
//
// Flags:
//   --count N     datagrams to publish (default 1,000,000)
//   --pace-us U   microseconds to sleep every 256 datagrams (default 0)
//   --rcvbuf B    SO_RCVBUF request in bytes (default 16 MiB)

#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include "core/feed_handler.h"
#include "net/multicast.h"
#include "obs/clock.h"
#include "sim/publisher.h"

using namespace mdfeed;

int main(int argc, char** argv) {
    std::uint64_t count = 1'000'000;
    int pace_us = 0;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--count") && i + 1 < argc) {
            count = static_cast<std::uint64_t>(std::atoll(argv[++i]));
        } else if (!std::strcmp(argv[i], "--pace-us") && i + 1 < argc) {
            pace_us = std::atoi(argv[++i]);
        }
    }

    const std::uint16_t port =
        static_cast<std::uint16_t>(45000 + (static_cast<unsigned>(::getpid()) % 1000));
    net::MulticastEndpoint ep{"239.0.0.7", port, "127.0.0.1"};

    net::MulticastReceiver rx;
    std::string err;
    if (!rx.open(ep, &err)) {
        std::fprintf(stderr, "receiver open failed: %s\n", err.c_str());
        return 2;
    }
    rx.set_recv_timeout_ms(200);

    net::MulticastSender tx;
    if (!tx.open(ep, &err)) {
        std::fprintf(stderr, "sender open failed: %s\n", err.c_str());
        return 2;
    }

    // Pre-generate every payload so the publish loop does no allocation.
    sim::MessageGenerator gen(0xBEEF, 8);
    std::vector<std::vector<std::uint8_t>> payloads;
    payloads.reserve(count + 16);
    for (auto& d : gen.directory_messages()) payloads.push_back(std::move(d));
    std::vector<std::uint8_t> buf;
    sim::TransportHeader hdr{};
    for (std::uint64_t i = 0; i < count; ++i) {
        if (gen.next(buf, hdr)) payloads.push_back(buf);
    }

    core::FeedHandler handler(10);

    std::thread pub([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        for (std::size_t i = 0; i < payloads.size(); ++i) {
            tx.send(payloads[i].data(), payloads[i].size());
            if (pace_us > 0 && (i & 0xff) == 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(pace_us));
            }
        }
    });

    std::vector<std::uint8_t> rbuf;
    std::uint64_t received = 0;
    int idle = 0;
    const auto t0 = obs::now_ns();
    while (received < payloads.size() && idle < 25) {
        const int n = rx.recv(rbuf);
        if (n > 0) {
            handler.on_datagram(rbuf.data(), static_cast<std::size_t>(n));
            ++received;
            idle = 0;
        } else {
            ++idle;
        }
    }
    const auto t1 = obs::now_ns();
    pub.join();

    const double sec = static_cast<double>(t1 - t0) / 1e9;
    const double delivered_frac =
        payloads.empty() ? 0.0
                         : static_cast<double>(received) / static_cast<double>(payloads.size());
    const double rate = sec > 0 ? static_cast<double>(received) / sec : 0.0;

    std::printf(
        "{\n"
        "  \"published\": %llu,\n"
        "  \"delivered\": %llu,\n"
        "  \"delivered_fraction\": %.4f,\n"
        "  \"pace_us\": %d,\n"
        "  \"elapsed_ns\": %llu,\n"
        "  \"delivered_datagrams_per_sec\": %.0f,\n"
        "  \"messages_applied\": %llu,\n"
        "  \"gaps_detected\": %llu\n"
        "}\n",
        (unsigned long long)payloads.size(), (unsigned long long)received, delivered_frac, pace_us,
        (unsigned long long)(t1 - t0), rate, (unsigned long long)handler.stats().messages_applied,
        (unsigned long long)handler.stats().gaps_detected);
    return 0;
}
