// pcap_gen: produce the committed pcap test inputs.
//
// Emits two libpcap-format captures of ITCH multicast datagrams so the
// pcap-replay test inputs are reproducible from source rather than opaque
// committed binaries:
//
//   itch_synthetic.pcap  1000 ITCH messages from the simulator, single symbol,
//                        transport sequence numbers 1..1000 contiguous.
//   itch_gap.pcap        the same 1000 messages with the packets carrying
//                        transport sequence 500..510 omitted, so a replay
//                        through the gap detector flags exactly that range.
//
// Single-symbol generation keeps the per-stock-locate sequence stream a clean
// monotonic 1..N, which makes "sequence 500-510 missing" unambiguous.
//
// Usage: pcap_gen <out_dir>
//   writes <out_dir>/itch_synthetic.pcap and <out_dir>/itch_gap.pcap

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "pcap/pcap_io.h"
#include "sim/publisher.h"

using namespace mdfeed;

namespace {

constexpr int kMessages = 1000;
constexpr std::uint64_t kGapLo = 500;
constexpr std::uint64_t kGapHi = 510;
constexpr std::uint16_t kDstPort = 46007;

struct Datagram {
    std::vector<std::uint8_t> bytes;
    std::uint64_t sequence;
};

// Generate kMessages transport-framed datagrams from a single-symbol
// simulator. The directory message (seq 0) is emitted first so the receiver
// can bootstrap; it is never dropped.
std::vector<Datagram> generate() {
    sim::MessageGenerator gen(0xCAFE, 1);  // one symbol -> one stock_locate
    std::vector<Datagram> out;
    for (auto& d : gen.directory_messages()) {
        out.push_back({std::move(d), 0});
    }
    std::vector<std::uint8_t> buf;
    sim::TransportHeader hdr{};
    for (int i = 0; i < kMessages; ++i) {
        if (gen.next(buf, hdr)) {
            out.push_back({buf, hdr.sequence});
        }
    }
    return out;
}

bool write_pcap(const std::string& path, const std::vector<Datagram>& datagrams, bool drop_gap) {
    pcap::PcapWriter w;
    std::string err;
    if (!w.open(path, &err)) {
        std::fprintf(stderr, "pcap_gen: %s\n", err.c_str());
        return false;
    }
    std::uint64_t ts_us = 1'700'000'000ull * 1'000'000ull;  // fixed epoch base
    std::uint64_t written = 0;
    for (const auto& d : datagrams) {
        if (drop_gap && d.sequence >= kGapLo && d.sequence <= kGapHi) {
            ts_us += 100;  // advance capture clock for the dropped slot too
            continue;
        }
        if (!w.write_datagram(d.bytes.data(), d.bytes.size(), ts_us, kDstPort)) {
            std::fprintf(stderr, "pcap_gen: write failed for %s\n", path.c_str());
            return false;
        }
        ts_us += 100;  // 100 us between captured packets
        ++written;
    }
    w.close();
    std::printf("wrote %s (%llu packets)\n", path.c_str(), (unsigned long long)written);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: pcap_gen <out_dir>\n");
        return 2;
    }
    const std::string dir = argv[1];
    const auto datagrams = generate();

    if (!write_pcap(dir + "/itch_synthetic.pcap", datagrams, false)) return 1;
    if (!write_pcap(dir + "/itch_gap.pcap", datagrams, true)) return 1;
    return 0;
}
