#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "core/feed_handler.h"
#include "net/multicast.h"
#include "obs/clock.h"

using namespace mdfeed;

namespace {
void usage(const char* prog) {
    std::fprintf(stderr,
                 "usage: %s --group <ip> --port <p> [--iface <ip>] [--duration-ms <ms>]\n",
                 prog);
}
}  // namespace

int main(int argc, char** argv) {
    std::string group = "239.0.0.1";
    std::uint16_t port = 42424;
    std::string iface = "127.0.0.1";
    std::uint64_t duration_ms = 0;  // 0 == run until signal
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        auto next = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", name);
                std::exit(2);
            }
            return argv[++i];
        };
        if (!std::strcmp(a, "--group"))
            group = next("--group");
        else if (!std::strcmp(a, "--port"))
            port = static_cast<std::uint16_t>(std::atoi(next("--port")));
        else if (!std::strcmp(a, "--iface"))
            iface = next("--iface");
        else if (!std::strcmp(a, "--duration-ms"))
            duration_ms = static_cast<std::uint64_t>(std::atoll(next("--duration-ms")));
        else if (!std::strcmp(a, "--help")) {
            usage(argv[0]);
            return 0;
        } else {
            std::fprintf(stderr, "unknown arg: %s\n", a);
            usage(argv[0]);
            return 2;
        }
    }

    net::MulticastReceiver rx;
    std::string err;
    if (!rx.open({group, port, iface}, &err)) {
        std::fprintf(stderr, "open failed: %s\n", err.c_str());
        return 1;
    }
    rx.set_recv_timeout_ms(250);

    core::FeedHandler handler;
    std::vector<std::uint8_t> buf;
    const auto t0 = obs::now_ns();
    while (true) {
        const int n = rx.recv(buf);
        if (n > 0) {
            handler.on_datagram(buf.data(), static_cast<std::size_t>(n));
        }
        if (duration_ms > 0) {
            const auto now = obs::now_ns();
            if ((now - t0) / 1'000'000 >= duration_ms) break;
        }
    }
    const auto& s = handler.stats();
    std::printf(
        "{\"datagrams\":%llu,\"messages\":%llu,\"gaps\":%llu,"
        "\"snapshots_requested\":%llu,\"snapshots_applied\":%llu,\"parse_errors\":%llu}\n",
        (unsigned long long)s.datagrams_in, (unsigned long long)s.messages_applied,
        (unsigned long long)s.gaps_detected, (unsigned long long)s.snapshots_requested,
        (unsigned long long)s.snapshots_applied, (unsigned long long)s.parse_errors);
    return 0;
}
