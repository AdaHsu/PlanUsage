#pragma once
#include <Arduino.h>
#include <time.h>

namespace TimeFmt {
    // TZ is pinned to UTC0 at boot, so mktime() yields a UTC epoch.
    time_t parseIso8601Utc(const char* s);

    // "2h 30m" / "3d 5h" / "12m" — how long until the window resets.
    String countdown(time_t resetsAt, time_t now);

    // "now" / "3m ago" / "2h ago" / "never" — age of a cached fetch.
    String age(time_t last, time_t now);

    // "2026-07-08" — UTC calendar date, for stamps worth pinning to a day
    // rather than an "ago" that keeps sliding. "--" when t is 0/unset.
    String date(time_t t);

    // "4:12" for the next-refresh countdown in the header.
    String mmss(int seconds);

    // "4h 12m" uptime.
    String uptime(unsigned long ms);
}
