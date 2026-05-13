#include "net/multicast.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cstring>

namespace mdfeed::net {

namespace {
void set_err(std::string* err, const std::string& s) {
    if (err) *err = s;
}
}  // namespace

MulticastReceiver::~MulticastReceiver() { close(); }

void MulticastReceiver::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

bool MulticastReceiver::open(const MulticastEndpoint& ep, std::string* err) {
    close();
    ep_ = ep;
    fd_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd_ < 0) {
        set_err(err, std::string("socket: ") + strerror(errno));
        return false;
    }
    int one = 1;
    if (::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0) {
        set_err(err, std::string("SO_REUSEADDR: ") + strerror(errno));
        close();
        return false;
    }
#ifdef SO_REUSEPORT
    ::setsockopt(fd_, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
#endif
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    // Bind to 0.0.0.0 not the group; the kernel filters by membership.
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(ep.port);
    if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        set_err(err, std::string("bind: ") + strerror(errno));
        close();
        return false;
    }
    ip_mreq mreq{};
    if (::inet_pton(AF_INET, ep.group.c_str(), &mreq.imr_multiaddr) != 1) {
        set_err(err, "inet_pton(group) failed");
        close();
        return false;
    }
    if (::inet_pton(AF_INET, ep.iface.c_str(), &mreq.imr_interface) != 1) {
        set_err(err, "inet_pton(iface) failed");
        close();
        return false;
    }
    if (::setsockopt(fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        set_err(err, std::string("IP_ADD_MEMBERSHIP: ") + strerror(errno));
        close();
        return false;
    }
    return true;
}

bool MulticastReceiver::set_recv_timeout_ms(int ms) {
    if (fd_ < 0) return false;
    timeval tv{};
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    return ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0;
}

int MulticastReceiver::recv(std::vector<std::uint8_t>& buf) {
    if (fd_ < 0) return -1;
    if (buf.size() < 2048) buf.resize(2048);
    sockaddr_in from{};
    socklen_t fl = sizeof(from);
    const ssize_t n =
        ::recvfrom(fd_, buf.data(), buf.size(), 0, reinterpret_cast<sockaddr*>(&from), &fl);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        return -1;
    }
    return static_cast<int>(n);
}

MulticastSender::~MulticastSender() { close(); }

void MulticastSender::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

bool MulticastSender::open(const MulticastEndpoint& ep, std::string* err) {
    close();
    ep_ = ep;
    fd_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd_ < 0) {
        set_err(err, std::string("socket: ") + strerror(errno));
        return false;
    }
    // Enable loopback so the receiver on the same host sees our packets.
    unsigned char loop = 1;
    if (::setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop)) < 0) {
        set_err(err, std::string("IP_MULTICAST_LOOP: ") + strerror(errno));
        close();
        return false;
    }
    in_addr local{};
    if (::inet_pton(AF_INET, ep.iface.c_str(), &local) != 1) {
        set_err(err, "inet_pton(iface) failed");
        close();
        return false;
    }
    if (::setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_IF, &local, sizeof(local)) < 0) {
        set_err(err, std::string("IP_MULTICAST_IF: ") + strerror(errno));
        close();
        return false;
    }
    unsigned char ttl = 1;  // stay on the local link
    ::setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(ep.port);
    if (::inet_pton(AF_INET, ep.group.c_str(), &to.sin_addr) != 1) {
        set_err(err, "inet_pton(group) failed");
        close();
        return false;
    }
    remote_.assign(reinterpret_cast<std::uint8_t*>(&to),
                   reinterpret_cast<std::uint8_t*>(&to) + sizeof(to));
    return true;
}

int MulticastSender::send(const std::uint8_t* data, std::size_t len) {
    if (fd_ < 0 || remote_.size() != sizeof(sockaddr_in)) return -1;
    const sockaddr_in* to = reinterpret_cast<const sockaddr_in*>(remote_.data());
    const ssize_t n = ::sendto(fd_, data, len, 0, reinterpret_cast<const sockaddr*>(to),
                               sizeof(*to));
    if (n < 0) return -1;
    return static_cast<int>(n);
}

}  // namespace mdfeed::net
