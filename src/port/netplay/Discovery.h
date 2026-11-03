#pragma once

// LAN discovery: while a host waits in its lobby it announces the game on the local network once a second, and the
// Online menu lists every announcement it hears. Players on the same Wi-Fi (or one phone's hotspot) then join with
// one tap instead of typing an address, which matters on a phone.
// Plain IPv4 UDP broadcast on its own port, separate from the game socket; the announcement carries the game port,
// so joining is an ordinary direct join to that address.

#include <cstdint>
#include <string>
#include <vector>

namespace netplay {

constexpr uint16_t kDiscoveryPort = 46465;

struct LanGame {
    std::string name;
    std::string address; // "192.168.1.20:46464", ready for --join
    bool sameBuild = false;
    uint64_t lastSeenMs = 0;
};

class LanBeacon {
  public:
    LanBeacon() = default;
    ~LanBeacon();
    LanBeacon(const LanBeacon&) = delete;
    LanBeacon& operator=(const LanBeacon&) = delete;

    // Sends at most one announcement a second, so it is cheap to call from a polling loop.
    void Tick(const std::string& name, uint16_t gamePort, uint64_t buildId);
    void Close();

  private:
    intptr_t mFd = -1;
    bool mTried = false;
    uint64_t mLastMs = 0;
};

class LanBrowser {
  public:
    LanBrowser() = default;
    ~LanBrowser();
    LanBrowser(const LanBrowser&) = delete;
    LanBrowser& operator=(const LanBrowser&) = delete;

    // Reads pending announcements and forgets games not heard from for a few seconds.
    void Poll(uint64_t buildId);
    const std::vector<LanGame>& Games() const {
        return mGames;
    }
    // False when the discovery port could not be opened (another program holds it without sharing).
    bool IsListening() const {
        return mFd != -1;
    }
    void Close();

  private:
    intptr_t mFd = -1;
    bool mTried = false;
    std::vector<LanGame> mGames;
};

} // namespace netplay
