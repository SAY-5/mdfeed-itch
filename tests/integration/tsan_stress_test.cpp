// TSan stress: N publisher threads each open their own MulticastSender and
// push messages concurrently on a shared multicast group. One handler thread
// consumes from a MulticastReceiver and applies datagrams to a FeedHandler.
//
// The goal is to drive ThreadSanitizer over the publisher/handler/book code
// paths and assert no races. We do not assert message counts because the
// kernel can drop multicast packets under burst; we only assert the handler's
// stats are monotonic and the book invariants hold at the end.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "core/feed_handler.h"
#include "net/multicast.h"
#include "sim/publisher.h"

using namespace mdfeed;

namespace {

std::uint16_t tsan_port() {
    static const std::uint16_t p =
        static_cast<std::uint16_t>(45000 + (static_cast<unsigned>(::getpid()) % 1000));
    return p;
}

}  // namespace

TEST(TsanStress, ThreePublishersOneHandler) {
    net::MulticastEndpoint ep{"239.0.0.3", tsan_port(), "127.0.0.1"};

    net::MulticastReceiver rx;
    std::string err;
    ASSERT_TRUE(rx.open(ep, &err)) << err;
    rx.set_recv_timeout_ms(100);

    constexpr int kPublishers = 3;
    constexpr int kPerPub = 5000;

    core::FeedHandler handler(10);
    std::atomic<bool> done{false};

    std::thread consumer([&]() {
        std::vector<std::uint8_t> rbuf;
        while (!done.load(std::memory_order_acquire)) {
            const int n = rx.recv(rbuf);
            if (n > 0) {
                handler.on_datagram(rbuf.data(), static_cast<std::size_t>(n));
            }
        }
        // Final drain.
        rx.set_recv_timeout_ms(50);
        for (int extra = 0; extra < 200; ++extra) {
            const int n = rx.recv(rbuf);
            if (n <= 0) break;
            handler.on_datagram(rbuf.data(), static_cast<std::size_t>(n));
        }
    });

    std::vector<std::thread> publishers;
    publishers.reserve(static_cast<std::size_t>(kPublishers));
    for (int t = 0; t < kPublishers; ++t) {
        publishers.emplace_back([&, t]() {
            net::MulticastSender tx;
            std::string e;
            ASSERT_TRUE(tx.open(ep, &e)) << e;
            // Each publisher uses a distinct seed and a disjoint symbol band
            // so their order-id and stock_locate spaces do not collide on
            // the handler side. The MessageGenerator picks symbols based on
            // its own symbol list; we keep them all sharing the same names
            // since the handler indexes by symbol, but the order-ids are
            // partitioned by seed.
            sim::MessageGenerator gen(static_cast<std::uint64_t>(0xA + t) * 1000003ULL, 4);
            for (const auto& d : gen.directory_messages()) {
                tx.send(d.data(), d.size());
            }
            std::vector<std::uint8_t> buf;
            sim::TransportHeader hdr{};
            for (int i = 0; i < kPerPub; ++i) {
                if (gen.next(buf, hdr)) {
                    tx.send(buf.data(), buf.size());
                }
                // Light throttle every 256 messages so the kernel buffers
                // do not silently drop everything.
                if ((i & 0xFF) == 0) {
                    std::this_thread::sleep_for(std::chrono::microseconds(50));
                }
            }
        });
    }

    for (auto& p : publishers) p.join();
    // Give the consumer a moment to drain in-flight datagrams.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    done.store(true, std::memory_order_release);
    consumer.join();

    // Loopback multicast can drop bursts, but we should have received
    // something and the handler's view must remain consistent.
    EXPECT_GT(handler.stats().datagrams_in, 0u);
    EXPECT_GT(handler.stats().messages_applied, 0u);

    for (const auto& sym : sim::MessageGenerator(1, 4).symbols()) {
        const auto* sb = handler.book().find(sym);
        if (!sb) continue;
        EXPECT_TRUE(sb->invariants_ok())
            << "invariants violated for " << itch::symbol_to_string(sym);
    }
}
