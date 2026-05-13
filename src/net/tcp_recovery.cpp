#include "net/tcp_recovery.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace mdfeed::net {

namespace {

void set_err(std::string* err, const std::string& s) {
    if (err) *err = s;
}

bool set_nonblock(int fd, bool nb) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    flags = nb ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    return ::fcntl(fd, F_SETFL, flags) == 0;
}

}  // namespace

TcpClient::~TcpClient() { close(); }

void TcpClient::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

bool TcpClient::connect(const std::string& host, std::uint16_t port, int timeout_ms,
                        std::string* err) {
    close();
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) {
        set_err(err, std::string("socket: ") + strerror(errno));
        return false;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        set_err(err, "inet_pton failed");
        close();
        return false;
    }
    set_nonblock(fd_, true);
    int rc = ::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (rc < 0 && errno != EINPROGRESS) {
        set_err(err, std::string("connect: ") + strerror(errno));
        close();
        return false;
    }
    if (rc < 0) {
        fd_set wf;
        FD_ZERO(&wf);
        FD_SET(fd_, &wf);
        timeval tv{};
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        rc = ::select(fd_ + 1, nullptr, &wf, nullptr, &tv);
        if (rc <= 0) {
            set_err(err, "connect timeout");
            close();
            return false;
        }
        int sockerr = 0;
        socklen_t slen = sizeof(sockerr);
        ::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &sockerr, &slen);
        if (sockerr != 0) {
            set_err(err, std::string("connect: ") + strerror(sockerr));
            close();
            return false;
        }
    }
    set_nonblock(fd_, false);
    return true;
}

bool tcp_send_all(int fd, const std::uint8_t* data, std::size_t len) {
    std::size_t off = 0;
    while (off < len) {
        ssize_t n = ::send(fd, data + off, len - off, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        off += static_cast<std::size_t>(n);
    }
    return true;
}

bool tcp_recv_frame(int fd, std::vector<std::uint8_t>& out) {
    std::uint8_t hdr[4]{};
    std::size_t got = 0;
    while (got < 4) {
        ssize_t n = ::recv(fd, hdr + got, 4 - got, 0);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return false;
        }
        got += static_cast<std::size_t>(n);
    }
    const std::uint32_t len = (static_cast<std::uint32_t>(hdr[0]) << 24) |
                              (static_cast<std::uint32_t>(hdr[1]) << 16) |
                              (static_cast<std::uint32_t>(hdr[2]) << 8) |
                              static_cast<std::uint32_t>(hdr[3]);
    if (len == 0 || len > 16u * 1024u * 1024u) return false;
    out.assign(4, 0);
    out[0] = hdr[0];
    out[1] = hdr[1];
    out[2] = hdr[2];
    out[3] = hdr[3];
    out.resize(4 + len);
    std::size_t off = 4;
    while (off < out.size()) {
        ssize_t n = ::recv(fd, out.data() + off, out.size() - off, 0);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return false;
        }
        off += static_cast<std::size_t>(n);
    }
    return true;
}

bool TcpClient::send_all(const std::uint8_t* data, std::size_t len) {
    return tcp_send_all(fd_, data, len);
}

bool TcpClient::recv_frame(std::vector<std::uint8_t>& out) {
    return tcp_recv_frame(fd_, out);
}

TcpServer::~TcpServer() { close(); }

void TcpServer::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

bool TcpServer::listen(const std::string& bind_host, std::uint16_t port, std::string* err) {
    close();
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) {
        set_err(err, std::string("socket: ") + strerror(errno));
        return false;
    }
    int one = 1;
    ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, bind_host.c_str(), &addr.sin_addr) != 1) {
        set_err(err, "inet_pton failed");
        close();
        return false;
    }
    if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        set_err(err, std::string("bind: ") + strerror(errno));
        close();
        return false;
    }
    if (::listen(fd_, 4) < 0) {
        set_err(err, std::string("listen: ") + strerror(errno));
        close();
        return false;
    }
    socklen_t alen = sizeof(addr);
    ::getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &alen);
    port_ = ntohs(addr.sin_port);
    return true;
}

int TcpServer::accept_one(int timeout_ms) {
    if (fd_ < 0) return -1;
    fd_set rf;
    FD_ZERO(&rf);
    FD_SET(fd_, &rf);
    timeval tv{};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    int rc = ::select(fd_ + 1, &rf, nullptr, nullptr, &tv);
    if (rc <= 0) return -1;
    sockaddr_in peer{};
    socklen_t plen = sizeof(peer);
    return ::accept(fd_, reinterpret_cast<sockaddr*>(&peer), &plen);
}

}  // namespace mdfeed::net
