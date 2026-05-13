#include "sim/snapshot_server.h"

#include <unistd.h>

namespace mdfeed::sim {

SnapshotServer::~SnapshotServer() {
    stop();
}

bool SnapshotServer::start(std::uint16_t port, SnapshotProvider provider, std::string* err) {
    stop();
    provider_ = std::move(provider);
    if (!server_.listen("127.0.0.1", port, err)) return false;
    running_.store(true);
    thread_ = std::thread([this] { run(); });
    return true;
}

void SnapshotServer::stop() {
    running_.store(false);
    server_.close();
    if (thread_.joinable()) thread_.join();
}

void SnapshotServer::run() {
    while (running_.load()) {
        const int cfd = server_.accept_one(100);
        if (cfd < 0) continue;
        std::vector<std::uint8_t> frame;
        if (!net::tcp_recv_frame(cfd, frame)) {
            ::close(cfd);
            continue;
        }
        std::size_t consumed = 0;
        const auto dec = itch::decode_frame(frame, consumed);
        if (dec.kind != itch::DecodedFrame::Kind::Request) {
            ::close(cfd);
            continue;
        }
        itch::SnapshotResponse resp;
        if (provider_) {
            resp = provider_(dec.request);
        }
        const auto out = itch::encode_response(resp);
        net::tcp_send_all(cfd, out.data(), out.size());
        served_.fetch_add(1, std::memory_order_relaxed);
        ::close(cfd);
    }
}

}  // namespace mdfeed::sim
