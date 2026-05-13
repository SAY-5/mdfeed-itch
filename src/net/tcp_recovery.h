#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mdfeed::net {

// A thin synchronous TCP client used by the snapshot-client. Writes the
// length-prefixed snapshot-request frame, reads a single length-prefixed
// response frame, and returns the bytes to the caller for decoding.
class TcpClient {
public:
    TcpClient() = default;
    ~TcpClient();
    TcpClient(const TcpClient&) = delete;
    TcpClient& operator=(const TcpClient&) = delete;

    bool connect(const std::string& host, std::uint16_t port, int timeout_ms,
                 std::string* err = nullptr);
    bool send_all(const std::uint8_t* data, std::size_t len);
    // Read exactly one length-prefixed frame (4-byte u32 BE length + body).
    bool recv_frame(std::vector<std::uint8_t>& out);
    void close();
    int fd() const { return fd_; }

private:
    int fd_{-1};
};

// Server-side single-connection helper used by tests (and the sim package).
class TcpServer {
public:
    TcpServer() = default;
    ~TcpServer();
    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    bool listen(const std::string& bind_host, std::uint16_t port, std::string* err = nullptr);
    std::uint16_t bound_port() const { return port_; }
    // Accept one connection. Returns -1 on error.
    int accept_one(int timeout_ms);
    void close();

private:
    int fd_{-1};
    std::uint16_t port_{0};
};

bool tcp_send_all(int fd, const std::uint8_t* data, std::size_t len);
bool tcp_recv_frame(int fd, std::vector<std::uint8_t>& out);

}  // namespace mdfeed::net
