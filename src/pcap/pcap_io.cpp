#include "pcap/pcap_io.h"

#include <pcap/pcap.h>

#include <cstdio>
#include <cstring>

namespace mdfeed::pcap {

namespace {

void set_err(std::string* err, const std::string& s) {
    if (err) *err = s;
}

// Little-endian writers for the libpcap file headers.
void put_u16le(std::vector<std::uint8_t>& v, std::uint16_t x) {
    v.push_back(static_cast<std::uint8_t>(x));
    v.push_back(static_cast<std::uint8_t>(x >> 8));
}
void put_u32le(std::vector<std::uint8_t>& v, std::uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(static_cast<std::uint8_t>(x >> (i * 8)));
}

// Big-endian writers for the on-wire network headers.
void put_u16be(std::vector<std::uint8_t>& v, std::uint16_t x) {
    v.push_back(static_cast<std::uint8_t>(x >> 8));
    v.push_back(static_cast<std::uint8_t>(x));
}

// Ones-complement checksum over a byte range, used for the IPv4 header.
std::uint16_t ip_checksum(const std::uint8_t* data, std::size_t len) {
    std::uint32_t sum = 0;
    for (std::size_t i = 0; i + 1 < len; i += 2) {
        sum += (static_cast<std::uint32_t>(data[i]) << 8) | data[i + 1];
    }
    if (len & 1) sum += static_cast<std::uint32_t>(data[len - 1]) << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return static_cast<std::uint16_t>(~sum & 0xFFFF);
}

constexpr std::uint32_t kPcapMagic = 0xA1B2C3D4u;
constexpr std::uint16_t kPcapVersionMajor = 2;
constexpr std::uint16_t kPcapVersionMinor = 4;
constexpr std::uint32_t kSnapLen = 65535u;
constexpr std::uint32_t kDltEn10mb = 1u;

}  // namespace

// ---- PcapReader -----------------------------------------------------------

PcapReader::~PcapReader() {
    close();
}

void PcapReader::close() {
    if (handle_) {
        ::pcap_close(static_cast<pcap_t*>(handle_));
        handle_ = nullptr;
    }
    eof_ = false;
}

bool PcapReader::open(const std::string& path, std::string* err) {
    close();
    char errbuf[PCAP_ERRBUF_SIZE]{};
    pcap_t* p = ::pcap_open_offline(path.c_str(), errbuf);
    if (!p) {
        set_err(err, std::string("pcap_open_offline: ") + errbuf);
        return false;
    }
    if (::pcap_datalink(p) != DLT_EN10MB) {
        set_err(err, "unexpected link type; expected Ethernet (DLT_EN10MB)");
        ::pcap_close(p);
        return false;
    }
    handle_ = p;
    eof_ = false;
    return true;
}

bool PcapReader::next(PcapRecord& rec) {
    if (!handle_) return false;
    pcap_t* p = static_cast<pcap_t*>(handle_);
    struct pcap_pkthdr* hdr = nullptr;
    const u_char* data = nullptr;
    for (;;) {
        const int rc = ::pcap_next_ex(p, &hdr, &data);
        if (rc == PCAP_ERROR_BREAK) {
            eof_ = true;
            return false;
        }
        if (rc != 1 || !hdr || !data) {
            // Transient read error: skip this packet.
            if (rc < 0) {
                eof_ = true;
                return false;
            }
            continue;
        }
        if (hdr->caplen <= kHeadersLen) continue;  // too short to carry payload
        // Strip Ethernet + IPv4 + UDP. The generator emits option-free headers
        // so the payload begins at a fixed offset.
        const std::size_t payload_len = hdr->caplen - kHeadersLen;
        rec.payload.assign(data + kHeadersLen, data + kHeadersLen + payload_len);
        rec.ts_us = static_cast<std::uint64_t>(hdr->ts.tv_sec) * 1'000'000ull +
                    static_cast<std::uint64_t>(hdr->ts.tv_usec);
        return true;
    }
}

// ---- PcapWriter -----------------------------------------------------------

PcapWriter::~PcapWriter() {
    close();
}

void PcapWriter::close() {
    if (file_) {
        std::fclose(static_cast<FILE*>(file_));
        file_ = nullptr;
    }
}

bool PcapWriter::open(const std::string& path, std::string* err) {
    close();
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        set_err(err, "cannot open pcap file for writing: " + path);
        return false;
    }
    std::vector<std::uint8_t> ghdr;
    put_u32le(ghdr, kPcapMagic);
    put_u16le(ghdr, kPcapVersionMajor);
    put_u16le(ghdr, kPcapVersionMinor);
    put_u32le(ghdr, 0);  // thiszone
    put_u32le(ghdr, 0);  // sigfigs
    put_u32le(ghdr, kSnapLen);
    put_u32le(ghdr, kDltEn10mb);
    if (std::fwrite(ghdr.data(), 1, ghdr.size(), f) != ghdr.size()) {
        set_err(err, "short write of pcap global header");
        std::fclose(f);
        return false;
    }
    file_ = f;
    return true;
}

bool PcapWriter::write_datagram(const std::uint8_t* payload, std::size_t len, std::uint64_t ts_us,
                                std::uint16_t dst_port) {
    if (!file_) return false;
    FILE* f = static_cast<FILE*>(file_);

    // Build the network frame: Ethernet + IPv4 + UDP + payload.
    std::vector<std::uint8_t> frame;
    frame.reserve(kHeadersLen + len);

    // Ethernet: dst (multicast 01:00:5e:..), src, ethertype IPv4.
    const std::uint8_t eth_dst[6] = {0x01, 0x00, 0x5e, 0x00, 0x00, 0x07};
    const std::uint8_t eth_src[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
    frame.insert(frame.end(), eth_dst, eth_dst + 6);
    frame.insert(frame.end(), eth_src, eth_src + 6);
    put_u16be(frame, 0x0800);  // IPv4

    const std::uint16_t udp_len = static_cast<std::uint16_t>(kUdpHeaderLen + len);
    const std::uint16_t ip_total = static_cast<std::uint16_t>(kIpv4HeaderLen + udp_len);

    // IPv4 header (no options).
    const std::size_t ip_off = frame.size();
    frame.push_back(0x45);  // version 4, IHL 5
    frame.push_back(0x00);  // DSCP/ECN
    put_u16be(frame, ip_total);
    put_u16be(frame, 0x0000);  // identification
    put_u16be(frame, 0x0000);  // flags + fragment offset
    frame.push_back(0x10);     // TTL 16
    frame.push_back(0x11);     // protocol UDP (17)
    put_u16be(frame, 0x0000);  // checksum placeholder
    // src 127.0.0.1
    frame.push_back(127);
    frame.push_back(0);
    frame.push_back(0);
    frame.push_back(1);
    // dst 239.0.0.7
    frame.push_back(239);
    frame.push_back(0);
    frame.push_back(0);
    frame.push_back(7);
    const std::uint16_t ipck = ip_checksum(frame.data() + ip_off, kIpv4HeaderLen);
    frame[ip_off + 10] = static_cast<std::uint8_t>(ipck >> 8);
    frame[ip_off + 11] = static_cast<std::uint8_t>(ipck);

    // UDP header. Checksum 0 is legal for IPv4 UDP and lets the file stay
    // deterministic without a pseudo-header computation.
    put_u16be(frame, 50007);  // source port
    put_u16be(frame, dst_port);
    put_u16be(frame, udp_len);
    put_u16be(frame, 0x0000);  // checksum (disabled)

    frame.insert(frame.end(), payload, payload + len);

    // pcap per-packet record header.
    std::vector<std::uint8_t> rhdr;
    put_u32le(rhdr, static_cast<std::uint32_t>(ts_us / 1'000'000ull));
    put_u32le(rhdr, static_cast<std::uint32_t>(ts_us % 1'000'000ull));
    put_u32le(rhdr, static_cast<std::uint32_t>(frame.size()));  // caplen
    put_u32le(rhdr, static_cast<std::uint32_t>(frame.size()));  // origlen

    if (std::fwrite(rhdr.data(), 1, rhdr.size(), f) != rhdr.size()) return false;
    if (std::fwrite(frame.data(), 1, frame.size(), f) != frame.size()) return false;
    return true;
}

}  // namespace mdfeed::pcap
