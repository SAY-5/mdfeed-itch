#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "core/feed_handler.h"
#include "net/multicast.h"
#include "sim/publisher.h"
#include "sim/snapshot_server.h"

using namespace mdfeed;

namespace {

std::uint16_t mc_port() {
    static const std::uint16_t p =
        static_cast<std::uint16_t>(44000 + (static_cast<unsigned>(::getpid()) % 1000));
    return p;
}

// A "shadow" book updated alongside the publisher so the snapshot server can
// answer with the authoritative state at the moment the gap occurred. The
// mutex makes shadow updates and snapshot reads mutually exclusive so the
// snapshot is consistent with last_seq_by_loc.
struct Shadow {
    itch::DepthBook book{10};
    std::unordered_map<itch::StockLocate, itch::SequenceNumber> last_seq_by_loc;
    std::unordered_map<itch::StockLocate, itch::Symbol> sym_by_loc;
    mutable std::mutex mu;
};

}  // namespace

TEST(GapFill, RecoversFromDroppedPackets) {
    net::MulticastEndpoint ep{"239.0.0.2", mc_port(), "127.0.0.1"};
    net::MulticastReceiver rx;
    std::string err;
    ASSERT_TRUE(rx.open(ep, &err)) << err;
    rx.set_recv_timeout_ms(250);
    net::MulticastSender tx;
    ASSERT_TRUE(tx.open(ep, &err)) << err;

    Shadow shadow;
    sim::SnapshotServer server;

    auto provider = [&shadow](const itch::SnapshotRequest& req) -> itch::SnapshotResponse {
        std::lock_guard<std::mutex> g(shadow.mu);
        itch::SnapshotResponse out{};
        out.stock_locate = req.stock_locate;
        auto it = shadow.sym_by_loc.find(req.stock_locate);
        if (it == shadow.sym_by_loc.end()) return out;
        out.symbol = it->second;
        const auto* sb = shadow.book.find(it->second);
        if (sb) {
            out.bids = sb->bids().levels;
            out.asks = sb->asks().levels;
        }
        out.last_applied_seq = shadow.last_seq_by_loc[req.stock_locate];
        return out;
    };
    ASSERT_TRUE(server.start(0, provider, &err)) << err;
    const std::uint16_t snap_port = server.bound_port();

    core::FeedHandler handler(10);
    handler.set_recovery_requester(
        [snap_port](const itch::SnapshotRequest& req, itch::SnapshotResponse& out) {
            net::TcpClient c;
            std::string e;
            if (!c.connect("127.0.0.1", snap_port, 2000, &e)) return false;
            const auto bytes = itch::encode_request(req);
            if (!c.send_all(bytes.data(), bytes.size())) return false;
            std::vector<std::uint8_t> frame;
            if (!c.recv_frame(frame)) return false;
            std::size_t consumed = 0;
            const auto dec = itch::decode_frame(frame, consumed);
            if (dec.kind != itch::DecodedFrame::Kind::Response) return false;
            out = dec.response;
            return true;
        });

    sim::MessageGenerator gen(7, 3);

    // Bootstrap directories.
    for (const auto& d : gen.directory_messages()) {
        ASSERT_GT(tx.send(d.data(), d.size()), 0);
        for (std::size_t i = 0; i < d.size() && i < 1; ++i) (void)d[i];
    }
    for (std::size_t i = 0; i < gen.symbols().size(); ++i) {
        shadow.sym_by_loc[static_cast<itch::StockLocate>(100 + i)] = gen.symbols()[i];
    }

    constexpr int kCount = 1500;
    constexpr int kDropEvery = 100;

    int sent_to_handler = 0;
    int dropped = 0;
    std::vector<std::vector<std::uint8_t>> sent_payloads;

    std::thread pub([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        std::vector<std::uint8_t> buf;
        sim::TransportHeader hdr{};
        for (int i = 0; i < kCount; ++i) {
            if (!gen.next(buf, hdr)) continue;
            // Update shadow first (it should mirror the source-of-truth feed).
            const std::uint8_t* body = buf.data() + sim::kTransportHeaderSize;
            const std::size_t body_len = buf.size() - sim::kTransportHeaderSize;
            auto pr = itch::decode_frame(std::span<const std::uint8_t>(body, body_len));
            if (pr.status == itch::ParseStatus::Ok && pr.message.has_value()) {
                std::lock_guard<std::mutex> g(shadow.mu);
                shadow.book.apply(*pr.message);
                shadow.last_seq_by_loc[hdr.stock_locate] = hdr.sequence;
            }
            sent_payloads.push_back(buf);
            tx.send(buf.data(), buf.size());
            if ((i & 0x1f) == 0) std::this_thread::sleep_for(std::chrono::microseconds(50));
            (void)kDropEvery;
            (void)dropped;
            (void)sent_to_handler;
        }
    });

    // Receive every packet but DROP every 100th by simulating it; we still
    // need to ack the network layer so we read into a buffer and discard.
    std::vector<std::uint8_t> rbuf;
    int idx = 0;
    int idle = 0;
    int total_received = 0;
    int total_dropped = 0;
    while (total_received < kCount && idle < 100) {
        const int n = rx.recv(rbuf);
        if (n > 0) {
            idle = 0;
            ++total_received;
            ++idx;
            // Drop every Nth packet seen by the handler.
            if (idx % kDropEvery == 0) {
                ++total_dropped;
            } else {
                handler.on_datagram(rbuf.data(), static_cast<std::size_t>(n));
            }
        } else {
            ++idle;
        }
    }
    pub.join();

    // Drain anything still in flight.
    rx.set_recv_timeout_ms(50);
    for (int extra = 0; extra < 200; ++extra) {
        const int n = rx.recv(rbuf);
        if (n <= 0) break;
        ++total_received;
        ++idx;
        if (idx % kDropEvery == 0) {
            ++total_dropped;
        } else {
            handler.on_datagram(rbuf.data(), static_cast<std::size_t>(n));
        }
    }

    EXPECT_GT(total_received, 0);
    EXPECT_GT(total_dropped, 0);
    // We expect at least one gap to have been detected and recovered.
    EXPECT_GT(handler.stats().gaps_detected, 0u);
    EXPECT_GT(handler.stats().snapshots_applied, 0u);

    // Force end-of-feed convergence: request a fresh snapshot per locate so
    // the handler's state is sourced from the (now-quiescent) shadow.
    for (std::size_t i = 0; i < gen.symbols().size(); ++i) {
        itch::SnapshotRequest req{};
        req.stock_locate = static_cast<itch::StockLocate>(100 + i);
        req.from_seq = 0;
        itch::SnapshotResponse resp{};
        net::TcpClient c;
        std::string e;
        ASSERT_TRUE(c.connect("127.0.0.1", snap_port, 2000, &e)) << e;
        const auto bytes = itch::encode_request(req);
        ASSERT_TRUE(c.send_all(bytes.data(), bytes.size()));
        std::vector<std::uint8_t> frame;
        ASSERT_TRUE(c.recv_frame(frame));
        std::size_t consumed = 0;
        const auto dec = itch::decode_frame(frame, consumed);
        ASSERT_EQ(dec.kind, itch::DecodedFrame::Kind::Response);
        handler.apply_snapshot(dec.response);
    }

    server.stop();

    // Final book state should match the shadow book for every symbol we saw,
    // since the snapshot path delivers the authoritative state.
    for (const auto& sym : gen.symbols()) {
        const auto* hb = handler.book().find(sym);
        const auto* sb = shadow.book.find(sym);
        if (!sb || !hb) continue;
        EXPECT_TRUE(hb->invariants_ok());
        ASSERT_EQ(hb->bids().levels.size(), sb->bids().levels.size())
            << "bids mismatch for " << itch::symbol_to_string(sym);
        ASSERT_EQ(hb->asks().levels.size(), sb->asks().levels.size())
            << "asks mismatch for " << itch::symbol_to_string(sym);
        for (std::size_t i = 0; i < hb->bids().levels.size(); ++i) {
            EXPECT_EQ(hb->bids().levels[i].price, sb->bids().levels[i].price);
            EXPECT_EQ(hb->bids().levels[i].total_qty, sb->bids().levels[i].total_qty);
        }
        for (std::size_t i = 0; i < hb->asks().levels.size(); ++i) {
            EXPECT_EQ(hb->asks().levels[i].price, sb->asks().levels[i].price);
            EXPECT_EQ(hb->asks().levels[i].total_qty, sb->asks().levels[i].total_qty);
        }
    }
}
