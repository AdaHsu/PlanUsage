#pragma once
#include <Arduino.h>
#include <functional>
#include "../core/settings.h"

// Connects using the saved list in priority order: the list position is the
// preference, not the signal strength. A scan first tells us which of the
// saved networks are actually in the air, then each visible one is tried top
// down until one associates.
namespace WifiManager {
    // Reports which network is being tried, so the screen can show progress.
    typedef std::function<void(const String& ssid, int attempt, int total)> ProgressFn;

    bool connectAny(const DeviceSettings& settings, ProgressFn onProgress = nullptr);

    bool   isConnected();
    String ip();
    String ssid();
    int    rssi();

    // Advertises planusage.local so the admin page does not need the IP.
    void startMdns(const char* hostname);

    // Watchdog for the main loop. Once the link has been down for
    // graceMs it retries the whole priority list, without rebooting.
    // Returns true while the device is (or has just become) connected.
    bool keepAlive(const DeviceSettings& settings, unsigned long graceMs = 10000);

    // Consecutive failed keepAlive rounds; the caller falls back to setup
    // mode once this gets high enough.
    int failedRounds();
    void resetFailures();
}
