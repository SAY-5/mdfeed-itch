#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mdfeed::pcap {

// libpcap-format I/O for offline ITCH multicast captures.
//
// Captured packets are Ethernet (DLT_EN10MB) frames: a 14-byte Ethernet
// header, a 20-byte IPv4 header, an 8-byte UDP header, then the ITCH
// transport datagram (the payload the FeedHandler consumes). Both the reader
// and the writer assume this fixed, option-free layout, which is what a
// kernel-bypass-free libpcap capture of an ITCH multicast group looks like.

constexpr std::size_t kEthHeaderLen = 14;
constexpr std::size_t kIpv4HeaderLen = 20;
constexpr std::size_t kUdpHeaderLen = 8;
constexpr std::size_t kHeadersLen = kEthHeaderLen + kIpv4HeaderLen + kUdpHeaderLen;  // 42

// One replayed record: the capture timestamp and the extracted UDP payload.
struct PcapRecord {
    std::uint64_t ts_us{0};  // capture time, microseconds since epoch
    std::vector<std::uint8_t> payload;
};

// Reader: wraps pcap_open_offline / pcap_next_ex. Each next() call returns the
// next packet's UDP payload with the Ethernet/IPv4/UDP headers stripped.
class PcapReader {
  public:
    PcapReader() = default;
    ~PcapReader();
    PcapReader(const PcapReader&) = delete;
    PcapReader& operator=(const PcapReader&) = delete;

    bool open(const std::string& path, std::string* err = nullptr);
    void close();

    // Read the next packet. Returns true and fills rec on success; returns
    // false at end-of-file or on a non-recoverable error (check eof()).
    bool next(PcapRecord& rec);
    bool eof() const { return eof_; }

  private:
    void* handle_{nullptr};  // pcap_t*, kept opaque to avoid leaking <pcap.h>
    bool eof_{false};
};

// Writer: synthesizes a libpcap-format file. Used by the test-pcap generator
// so the committed inputs are reproducible from source. Each datagram is
// wrapped in a synthetic Ethernet/IPv4/UDP header addressed to the multicast
// group, matching what PcapReader expects to strip.
class PcapWriter {
  public:
    PcapWriter() = default;
    ~PcapWriter();
    PcapWriter(const PcapWriter&) = delete;
    PcapWriter& operator=(const PcapWriter&) = delete;

    bool open(const std::string& path, std::string* err = nullptr);
    void close();

    // Append one UDP datagram payload as a captured packet. ts_us is the
    // synthetic capture timestamp.
    bool write_datagram(const std::uint8_t* payload, std::size_t len, std::uint64_t ts_us,
                        std::uint16_t dst_port);

  private:
    void* file_{nullptr};  // FILE*
};

}  // namespace mdfeed::pcap
