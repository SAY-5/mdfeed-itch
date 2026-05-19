#include "sim/book_snapshot_publisher.h"

#include <unistd.h>

#include <algorithm>

namespace mdfeed::sim {

BookSnapshotPublisher::~BookSnapshotPublisher() {
    stop();
}

bool BookSnapshotPublisher::start(std::uint16_t port, const itch::DepthBook& book,
                                  std::mutex& book_mtx, BookSnapshotPolicy policy,
                                  std::string* err) {
    stop();
    book_ = &book;
    book_mtx_ = &book_mtx;
    policy_ = policy;
    seq_ = 0;
    last_msg_mark_ = 0;
    msg_counter_.store(0, std::memory_order_relaxed);
    published_.store(0, std::memory_order_relaxed);
    if (!server_.listen("127.0.0.1", port, err)) return false;
    last_publish_ = std::chrono::steady_clock::now();
    running_.store(true);
    thread_ = std::thread([this] { run(); });
    return true;
}

void BookSnapshotPublisher::stop() {
    running_.store(false);
    server_.close();
    if (thread_.joinable()) thread_.join();
    for (int fd : subscribers_) {
        if (fd >= 0) ::close(fd);
    }
    subscribers_.clear();
}

bool BookSnapshotPublisher::should_publish(std::chrono::steady_clock::time_point now,
                                           std::uint64_t msgs) const {
    if (policy_.every_messages != 0 && msgs - last_msg_mark_ >= policy_.every_messages) {
        return true;
    }
    if (policy_.interval.count() != 0 && (now - last_publish_) >= policy_.interval) {
        return true;
    }
    return false;
}

std::vector<std::uint8_t> BookSnapshotPublisher::capture_frame() {
    // Phase 1: hold the book lock only long enough to copy out a consistent
    // BookSnapshot value. No socket calls happen here.
    BookSnapshot snap;
    {
        std::lock_guard<std::mutex> lk(*book_mtx_);
        holding_lock_.store(true, std::memory_order_release);
        snap = capture_book_snapshot(*book_, ++seq_);
        holding_lock_.store(false, std::memory_order_release);
    }
    // Phase 2: serialise outside the lock. The TCP write in run() is also
    // outside the lock, so a slow subscriber never stalls the feed thread.
    return encode_book_snapshot(snap);
}

void BookSnapshotPublisher::run() {
    while (running_.load()) {
        // Accept any pending subscriber without blocking the publish loop.
        const int cfd = server_.accept_one(20);
        if (cfd >= 0) subscribers_.push_back(cfd);

        const auto now = std::chrono::steady_clock::now();
        const std::uint64_t msgs = msg_counter_.load(std::memory_order_relaxed);
        if (!should_publish(now, msgs)) continue;

        last_publish_ = now;
        last_msg_mark_ = msgs;

        const std::vector<std::uint8_t> frame = capture_frame();

        // Push to every subscriber; drop any that error out. The book lock is
        // NOT held here.
        std::vector<int> live;
        live.reserve(subscribers_.size());
        for (int fd : subscribers_) {
            if (net::tcp_send_all(fd, frame.data(), frame.size())) {
                live.push_back(fd);
            } else {
                ::close(fd);
            }
        }
        subscribers_.swap(live);
        published_.fetch_add(1, std::memory_order_relaxed);
    }
}

}  // namespace mdfeed::sim
