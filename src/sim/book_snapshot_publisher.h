#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#include "core/depth_book.h"
#include "net/tcp_recovery.h"
#include "sim/book_snapshot.h"

namespace mdfeed::sim {

// Publisher trigger policy. A snapshot is taken when either the interval
// elapses or the message counter reaches the configured stride; set a field
// to 0 to disable that trigger.
struct BookSnapshotPolicy {
    std::chrono::milliseconds interval{0};  // time-based trigger
    std::uint64_t every_messages{0};        // message-count trigger
};

// TCP service that pushes binary book snapshots to connected subscribers.
//
// Hot-path discipline: the multicast receive thread only calls note_message()
// and the publisher only reads the book under a short-lived lock to produce a
// BookSnapshot value copy. The TCP write happens entirely outside that lock,
// so a slow or stalled subscriber can never back-pressure the feed thread.
// The caller passes the same mutex it uses to guard book mutations; if the
// feed is single-threaded relative to the publisher it can still pass a mutex
// the publisher will simply acquire uncontended.
class BookSnapshotPublisher {
  public:
    BookSnapshotPublisher() = default;
    ~BookSnapshotPublisher();

    BookSnapshotPublisher(const BookSnapshotPublisher&) = delete;
    BookSnapshotPublisher& operator=(const BookSnapshotPublisher&) = delete;

    // Start listening on 127.0.0.1:<port> (port 0 picks an ephemeral port).
    // book and book_mtx must outlive the publisher. book_mtx guards the read
    // used to capture a consistent snapshot; it is released before any TCP
    // write.
    bool start(std::uint16_t port, const itch::DepthBook& book, std::mutex& book_mtx,
               BookSnapshotPolicy policy, std::string* err = nullptr);
    void stop();

    // Called by the feed hot path once per applied message. Lock-free.
    void note_message() { msg_counter_.fetch_add(1, std::memory_order_relaxed); }

    std::uint16_t bound_port() const { return server_.bound_port(); }
    std::uint64_t snapshots_published() const { return published_.load(std::memory_order_relaxed); }
    // True for the duration of the in-lock book read; tests assert it is
    // false while a TCP write is in flight.
    bool holding_book_lock() const { return holding_lock_.load(std::memory_order_acquire); }

  private:
    void run();
    bool should_publish(std::chrono::steady_clock::time_point now, std::uint64_t msgs) const;
    // Capture under book_mtx_, release, then return the encoded frame. The
    // lock is held only for the duration of the value copy.
    std::vector<std::uint8_t> capture_frame();

    net::TcpServer server_;
    const itch::DepthBook* book_{nullptr};
    std::mutex* book_mtx_{nullptr};
    BookSnapshotPolicy policy_{};
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> holding_lock_{false};
    std::atomic<std::uint64_t> msg_counter_{0};
    std::atomic<std::uint64_t> published_{0};
    std::uint64_t seq_{0};
    std::uint64_t last_msg_mark_{0};
    std::chrono::steady_clock::time_point last_publish_{};
    std::vector<int> subscribers_;
};

}  // namespace mdfeed::sim
