#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

#include "core/feed_handler.h"
#include "net/multicast.h"
#include "sim/book_snapshot.h"
#include "sim/book_snapshot_publisher.h"
#include "sim/book_snapshot_subscriber.h"
#include "sim/publisher.h"

using namespace mdfeed;

namespace {

std::uint16_t mc_port() {
    static const std::uint16_t p =
        static_cast<std::uint16_t>(44000 + (static_cast<unsigned>(::getpid()) % 800));
    return p;
}

bool levels_equal(const std::vector<itch::PriceLevel>& a, const std::vector<itch::PriceLevel>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].price != b[i].price) return false;
        if (a[i].total_qty != b[i].total_qty) return false;
        if (a[i].order_count != b[i].order_count) return false;
    }
    return true;
}

// Capped depth-10 view of a SymbolBook side, mirroring capture_book_snapshot.
std::vector<itch::PriceLevel> top10(const std::vector<itch::PriceLevel>& src) {
    const std::size_t n =
        src.size() < sim::kSnapshotMaxLevels ? src.size() : sim::kSnapshotMaxLevels;
    return std::vector<itch::PriceLevel>(src.begin(), src.begin() + static_cast<std::ptrdiff_t>(n));
}

}  // namespace

// 1 publisher + 1 subscriber over loopback TCP. A multicast feed run drives a
// FeedHandler's depth book; the publisher pushes binary snapshots concurrently
// and the subscriber rebuilds its own depth-10 book. After the run, the
// subscriber's reconstruction is byte-equal to the publisher's reference book
// for every symbol.
TEST(BookSnapshotPubSub, CrossProcessBookEqualAllSymbols) {
    net::MulticastEndpoint ep{"239.0.0.7", mc_port(), "127.0.0.1"};

    net::MulticastReceiver rx;
    std::string err;
    ASSERT_TRUE(rx.open(ep, &err)) << err;
    rx.set_recv_timeout_ms(250);

    net::MulticastSender tx;
    ASSERT_TRUE(tx.open(ep, &err)) << err;

    sim::MessageGenerator gen(1234, 6);
    for (const auto& d : gen.directory_messages()) {
        ASSERT_GT(tx.send(d.data(), d.size()), 0);
    }

    core::FeedHandler handler(10);
    std::mutex book_mtx;

    // Publisher: snapshot every 64 applied messages, started before the feed
    // so it runs concurrently with the receive hot path.
    sim::BookSnapshotPublisher publisher;
    sim::BookSnapshotPolicy policy;
    policy.every_messages = 64;
    policy.interval = std::chrono::milliseconds(0);
    ASSERT_TRUE(publisher.start(0, handler.book(), book_mtx, policy, &err)) << err;
    const std::uint16_t tcp_port = publisher.bound_port();
    ASSERT_NE(tcp_port, 0);

    sim::BookSnapshotSubscriber subscriber;
    ASSERT_TRUE(subscriber.connect(tcp_port, 1000, &err)) << err;

    // Subscriber thread drains snapshot frames until told to stop.
    std::atomic<bool> sub_run{true};
    std::thread sub_thread([&] {
        while (sub_run.load()) {
            if (!subscriber.receive_one()) break;
        }
    });

    constexpr int kCount = 4000;
    std::thread feed([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        std::vector<std::uint8_t> buf;
        sim::TransportHeader hdr{};
        for (int i = 0; i < kCount; ++i) {
            if (gen.next(buf, hdr)) {
                tx.send(buf.data(), buf.size());
            }
            if ((i & 0x7f) == 0) std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    });

    std::vector<std::uint8_t> rbuf;
    int received = 0;
    int idle = 0;
    while (received < kCount && idle < 60) {
        const int n = rx.recv(rbuf);
        if (n > 0) {
            {
                std::lock_guard<std::mutex> lk(book_mtx);
                handler.on_datagram(rbuf.data(), static_cast<std::size_t>(n));
            }
            publisher.note_message();
            ++received;
            idle = 0;
        } else {
            ++idle;
        }
    }
    feed.join();

    EXPECT_GT(handler.stats().messages_applied, 0u);

    // Drive a final snapshot that reflects the terminal book state, then read
    // frames until the subscriber has applied one taken after the feed ended.
    const std::uint64_t target = publisher.snapshots_published() + 2;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (publisher.snapshots_published() < target &&
           std::chrono::steady_clock::now() < deadline) {
        publisher.note_message();  // keeps the message-count trigger firing
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_GE(publisher.snapshots_published(), target);

    // Give the subscriber a moment to apply the terminal frames.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    sub_run.store(false);
    subscriber.close();
    if (sub_thread.joinable()) sub_thread.join();
    publisher.stop();

    ASSERT_GT(subscriber.frames_applied(), 0u);

    // Reference: capture the publisher's book directly, depth-10 capped.
    sim::BookSnapshot reference;
    {
        std::lock_guard<std::mutex> lk(book_mtx);
        reference = sim::capture_book_snapshot(handler.book(), 0);
    }
    ASSERT_GT(reference.symbols.size(), 0u);

    // Every reference symbol must match the subscriber's reconstruction
    // byte-equal across all depth-10 levels.
    int compared = 0;
    for (const auto& ref : reference.symbols) {
        const auto* sub_bids = subscriber.bids(ref.symbol);
        const auto* sub_asks = subscriber.asks(ref.symbol);
        ASSERT_NE(sub_bids, nullptr)
            << "subscriber missing symbol " << itch::symbol_to_string(ref.symbol);
        ASSERT_NE(sub_asks, nullptr);
        EXPECT_TRUE(levels_equal(top10(ref.bids), *sub_bids))
            << "bid mismatch for " << itch::symbol_to_string(ref.symbol);
        EXPECT_TRUE(levels_equal(top10(ref.asks), *sub_asks))
            << "ask mismatch for " << itch::symbol_to_string(ref.symbol);
        ++compared;
    }
    EXPECT_EQ(compared, static_cast<int>(reference.symbols.size()));
    EXPECT_GT(compared, 0);
}

// The publish path must take its consistent copy under the book lock but
// release it before any TCP write. A subscriber that never reads must not
// stall the publisher, and the lock-held flag must read false between
// captures.
TEST(BookSnapshotPubSub, PublishDoesNotHoldLockDuringTcpWrite) {
    sim::MessageGenerator gen(99, 4);
    core::FeedHandler handler(10);
    std::mutex book_mtx;

    // Seed the book so snapshots carry real records.
    {
        net::MulticastEndpoint ep{"239.0.0.8", mc_port(), "127.0.0.1"};
        net::MulticastReceiver rx;
        net::MulticastSender tx;
        std::string err;
        ASSERT_TRUE(rx.open(ep, &err)) << err;
        rx.set_recv_timeout_ms(150);
        ASSERT_TRUE(tx.open(ep, &err)) << err;
        for (const auto& d : gen.directory_messages()) tx.send(d.data(), d.size());
        std::vector<std::uint8_t> buf;
        sim::TransportHeader hdr{};
        for (int i = 0; i < 600; ++i) {
            if (gen.next(buf, hdr)) tx.send(buf.data(), buf.size());
        }
        std::vector<std::uint8_t> rbuf;
        int idle = 0;
        while (idle < 20) {
            const int n = rx.recv(rbuf);
            if (n > 0) {
                handler.on_datagram(rbuf.data(), static_cast<std::size_t>(n));
                idle = 0;
            } else {
                ++idle;
            }
        }
    }

    sim::BookSnapshotPublisher publisher;
    sim::BookSnapshotPolicy policy;
    policy.interval = std::chrono::milliseconds(10);
    std::string err;
    ASSERT_TRUE(publisher.start(0, handler.book(), book_mtx, policy, &err)) << err;

    // Connect a subscriber but never read: its socket buffer fills, yet the
    // publisher must keep producing snapshots because the TCP write is
    // outside the lock.
    sim::BookSnapshotSubscriber stalled;
    ASSERT_TRUE(stalled.connect(publisher.bound_port(), 1000, &err)) << err;

    // The feed thread keeps acquiring the book lock; if the publisher held it
    // during a write, contention would block this loop.
    std::atomic<bool> run{true};
    std::atomic<std::uint64_t> feed_acquires{0};
    std::thread feed([&] {
        while (run.load()) {
            {
                std::lock_guard<std::mutex> lk(book_mtx);
                // Trivial in-lock work, mirroring a hot-path mutation.
            }
            feed_acquires.fetch_add(1, std::memory_order_relaxed);
            publisher.note_message();
        }
    });

    const std::uint64_t before = publisher.snapshots_published();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    run.store(false);
    feed.join();
    const std::uint64_t after = publisher.snapshots_published();
    publisher.stop();

    // The publisher kept publishing despite the stalled subscriber.
    EXPECT_GT(after, before);
    // The feed thread acquired the book lock many times without stalling.
    EXPECT_GT(feed_acquires.load(), 1000u);
    // Between captures the lock-held flag reads false.
    EXPECT_FALSE(publisher.holding_book_lock());
}
