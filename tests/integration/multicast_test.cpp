#include "net/multicast.h"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "core/feed_handler.h"
#include "sim/publisher.h"

using namespace mdfeed;

namespace {

// Pick an unused-enough port for the test run by deriving from PID.
std::uint16_t test_port() {
    static const std::uint16_t p =
        static_cast<std::uint16_t>(43000 + (static_cast<unsigned>(::getpid()) % 1000));
    return p;
}

}  // namespace

TEST(Multicast, PublisherToHandlerOverLoopback) {
    net::MulticastEndpoint ep{"239.0.0.1", test_port(), "127.0.0.1"};

    net::MulticastReceiver rx;
    std::string err;
    ASSERT_TRUE(rx.open(ep, &err)) << err;
    rx.set_recv_timeout_ms(250);

    net::MulticastSender tx;
    ASSERT_TRUE(tx.open(ep, &err)) << err;

    sim::MessageGenerator gen(42, 4);

    // Push bootstrap stock-directory messages (seq=0).
    for (const auto& d : gen.directory_messages()) {
        ASSERT_GT(tx.send(d.data(), d.size()), 0);
    }

    core::FeedHandler handler(10);

    constexpr int kCount = 2000;
    std::thread pub([&]() {
        // Small delay so the receiver is in recv when packets arrive.
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        std::vector<std::uint8_t> buf;
        sim::TransportHeader hdr{};
        for (int i = 0; i < kCount; ++i) {
            if (gen.next(buf, hdr)) {
                tx.send(buf.data(), buf.size());
            }
            // Throttle slightly to give the receiver a chance to drain.
            if ((i & 0x7f) == 0) std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    });

    std::vector<std::uint8_t> rbuf;
    int received = 0;
    int idle = 0;
    while (received < kCount && idle < 50) {
        const int n = rx.recv(rbuf);
        if (n > 0) {
            handler.on_datagram(rbuf.data(), static_cast<std::size_t>(n));
            ++received;
            idle = 0;
        } else {
            ++idle;
        }
    }
    pub.join();

    // We expect the bulk of packets to be applied. The loopback can drop
    // bursts so we tolerate some loss but require the book to be non-empty
    // and consistent for at least one symbol.
    EXPECT_GT(handler.stats().datagrams_in, 0u);
    EXPECT_GT(handler.stats().messages_applied, 0u);

    bool any = false;
    for (const auto& sym : gen.symbols()) {
        const auto* sb = handler.book().find(sym);
        if (!sb) continue;
        EXPECT_TRUE(sb->invariants_ok());
        if (!sb->bids().levels.empty() || !sb->asks().levels.empty()) any = true;
    }
    EXPECT_TRUE(any);
}
