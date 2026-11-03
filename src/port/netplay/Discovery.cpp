#include "Discovery.h"
#include "NetSocket.h"

#include <algorithm>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace netplay {

namespace {

// "MK64LAN1" | game port (u16) | build id (u64) | name length (u8) | name. Big endian.
const char kMagic[8] = { 'M', 'K', '6', '4', 'L', 'A', 'N', '1' };
constexpr size_t kHeader = sizeof(kMagic) + 2 + 8 + 1;
constexpr size_t kMaxName = 32;
constexpr uint64_t kIntervalMs = 1000;
constexpr uint64_t kForgetMs = 4000;

#ifdef _WIN32
using SockFd = SOCKET;
bool Invalid(SockFd fd) {
    return fd == INVALID_SOCKET;
}
void CloseFd(intptr_t fd) {
    closesocket((SOCKET) fd);
}
#else
using SockFd = int;
bool Invalid(SockFd fd) {
    return fd < 0;
}
void CloseFd(intptr_t fd) {
    close((int) fd);
}
#endif

// IPv4 only: broadcast does not exist in IPv6, and the dual-stack game socket cannot send it everywhere.
intptr_t OpenBroadcastSocket(uint16_t bindPort) {
#ifdef _WIN32
    static bool sStarted = [] {
        WSADATA d;
        return WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }();
    (void) sStarted;
#endif
    SockFd fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (Invalid(fd)) {
        return -1;
    }
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, (const char*) &on, sizeof(on));
    if (bindPort != 0) {
        // Several copies of the game on one machine (a test, or a phone running two) all hear announcements.
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char*) &on, sizeof(on));
#ifdef SO_REUSEPORT
        setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, (const char*) &on, sizeof(on));
#endif
        sockaddr_in any{};
        any.sin_family = AF_INET;
        any.sin_addr.s_addr = htonl(INADDR_ANY);
        any.sin_port = htons(bindPort);
        if (bind(fd, (const sockaddr*) &any, sizeof(any)) != 0) {
            CloseFd((intptr_t) fd);
            return -1;
        }
    }
#ifdef _WIN32
    u_long nb = 1;
    ioctlsocket(fd, FIONBIO, &nb);
#else
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
#endif
    return (intptr_t) fd;
}

// 255.255.255.255 only leaves through the interface of the default route. A phone sharing its hotspot routes by
// mobile data, so each interface's own broadcast address is tried as well.
std::vector<uint32_t> BroadcastAddresses() {
    std::vector<uint32_t> out = { 0xFFFFFFFFu };
#ifndef _WIN32
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) == 0) {
        for (ifaddrs* it = list; it != nullptr; it = it->ifa_next) {
            if (it->ifa_addr == nullptr || it->ifa_addr->sa_family != AF_INET || !(it->ifa_flags & IFF_UP) ||
                (it->ifa_flags & IFF_LOOPBACK) || !(it->ifa_flags & IFF_BROADCAST) || it->ifa_broadaddr == nullptr) {
                continue;
            }
            uint32_t b = ntohl(((const sockaddr_in*) it->ifa_broadaddr)->sin_addr.s_addr);
            if (b != 0 && std::find(out.begin(), out.end(), b) == out.end()) {
                out.push_back(b);
            }
        }
        freeifaddrs(list);
    }
#endif
    return out;
}

void Put16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t) (v >> 8);
    p[1] = (uint8_t) v;
}

void Put64(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; i++) {
        p[i] = (uint8_t) (v >> (56 - 8 * i));
    }
}

uint16_t Get16(const uint8_t* p) {
    return (uint16_t) ((p[0] << 8) | p[1]);
}

uint64_t Get64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        v = (v << 8) | p[i];
    }
    return v;
}

} // namespace

LanBeacon::~LanBeacon() {
    Close();
}

void LanBeacon::Close() {
    if (mFd != -1) {
        CloseFd(mFd);
        mFd = -1;
    }
}

void LanBeacon::Tick(const std::string& name, uint16_t gamePort, uint64_t buildId) {
    uint64_t now = NowMs();
    if (mLastMs != 0 && now - mLastMs < kIntervalMs) {
        return;
    }
    mLastMs = now;
    if (mFd == -1 && !mTried) {
        mTried = true;
        mFd = OpenBroadcastSocket(0);
    }
    if (mFd == -1) {
        return;
    }
    uint8_t pkt[kHeader + kMaxName];
    size_t nameLen = std::min(name.size(), kMaxName);
    memcpy(pkt, kMagic, sizeof(kMagic));
    Put16(pkt + 8, gamePort);
    Put64(pkt + 10, buildId);
    pkt[18] = (uint8_t) nameLen;
    memcpy(pkt + kHeader, name.data(), nameLen);
    for (uint32_t b : BroadcastAddresses()) {
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_addr.s_addr = htonl(b);
        to.sin_port = htons(kDiscoveryPort);
        sendto((SockFd) mFd, (const char*) pkt, (int) (kHeader + nameLen), 0, (const sockaddr*) &to, sizeof(to));
    }
}

LanBrowser::~LanBrowser() {
    Close();
}

void LanBrowser::Close() {
    if (mFd != -1) {
        CloseFd(mFd);
        mFd = -1;
    }
    mTried = false;
    mGames.clear();
}

void LanBrowser::Poll(uint64_t buildId) {
    if (mFd == -1 && !mTried) {
        mTried = true;
        mFd = OpenBroadcastSocket(kDiscoveryPort);
    }
    if (mFd == -1) {
        return;
    }
    uint64_t now = NowMs();
    for (;;) {
        uint8_t buf[256];
        sockaddr_in from{};
        socklen_t fromLen = sizeof(from);
        int n = (int) recvfrom((SockFd) mFd, (char*) buf, sizeof(buf), 0, (sockaddr*) &from, &fromLen);
        if (n <= 0) {
            break;
        }
        if ((size_t) n < kHeader || memcmp(buf, kMagic, sizeof(kMagic)) != 0 || from.sin_family != AF_INET) {
            continue;
        }
        size_t nameLen = std::min<size_t>(buf[18], (size_t) n - kHeader);
        char ip[INET_ADDRSTRLEN] = { 0 };
        inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
        LanGame g;
        g.name.assign((const char*) buf + kHeader, nameLen);
        g.address = std::string(ip) + ":" + std::to_string(Get16(buf + 8));
        g.sameBuild = Get64(buf + 10) == buildId;
        g.lastSeenMs = now;
        auto it = std::find_if(mGames.begin(), mGames.end(),
                               [&](const LanGame& o) { return o.address == g.address; });
        if (it != mGames.end()) {
            *it = g;
        } else {
            mGames.push_back(g);
        }
    }
    mGames.erase(std::remove_if(mGames.begin(), mGames.end(),
                                [now](const LanGame& g) { return now - g.lastSeenMs > kForgetMs; }),
                 mGames.end());
}

} // namespace netplay
