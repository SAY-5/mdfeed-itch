#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>

#include "core/depth_book.h"
#include "core/types.h"
#include "net/tcp_recovery.h"
#include "recovery/snapshot_client.h"

namespace mdfeed::sim {

// In-test snapshot server. It binds to 127.0.0.1 on an ephemeral port (when
// configured port == 0) and serves one snapshot per accepted connection from
// a SnapshotProvider callback.
class SnapshotServer {
public:
    // The provider returns the canonical book snapshot for a (stock_locate,
    // from_seq) request. last_applied_seq on the response indicates the
    // sequence number up to which the snapshot is consistent.
    using SnapshotProvider = std::function<itch::SnapshotResponse(const itch::SnapshotRequest&)>;

    SnapshotServer() = default;
    ~SnapshotServer();

    bool start(std::uint16_t port, SnapshotProvider provider, std::string* err = nullptr);
    void stop();

    std::uint16_t bound_port() const { return server_.bound_port(); }
    std::uint64_t served_count() const { return served_.load(); }

private:
    void run();

    net::TcpServer server_;
    SnapshotProvider provider_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> served_{0};
};

}  // namespace mdfeed::sim
