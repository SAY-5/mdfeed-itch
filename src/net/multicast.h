#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mdfeed::net {

struct MulticastEndpoint {
    std::string group;  // dotted-quad, e.g. "239.0.0.1"
    std::uint16_t port{0};
    std::string iface{"127.0.0.1"};  // local interface IP to bind/join on
};

class MulticastReceiver {
public:
    MulticastReceiver() = default;
    ~MulticastReceiver();
    MulticastReceiver(const MulticastReceiver&) = delete;
    MulticastReceiver& operator=(const MulticastReceiver&) = delete;

    bool open(const MulticastEndpoint& ep, std::string* err = nullptr);
    bool set_recv_timeout_ms(int ms);
    void close();
    int fd() const { return fd_; }
    bool is_open() const { return fd_ >= 0; }

    // Blocking receive of one datagram into buf. Returns bytes read; 0 on
    // timeout; -1 on error.
    int recv(std::vector<std::uint8_t>& buf);

private:
    int fd_{-1};
    MulticastEndpoint ep_{};
};

class MulticastSender {
public:
    MulticastSender() = default;
    ~MulticastSender();
    MulticastSender(const MulticastSender&) = delete;
    MulticastSender& operator=(const MulticastSender&) = delete;

    bool open(const MulticastEndpoint& ep, std::string* err = nullptr);
    void close();
    int fd() const { return fd_; }
    bool is_open() const { return fd_ >= 0; }

    int send(const std::uint8_t* data, std::size_t len);

private:
    int fd_{-1};
    MulticastEndpoint ep_{};
    // Cached sockaddr_in storage; we keep it as raw bytes to keep the header
    // POSIX-free.
    std::vector<std::uint8_t> remote_;
};

}  // namespace mdfeed::net
