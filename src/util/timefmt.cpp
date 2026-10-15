#include "timefmt.h"

time_t TimeFmt::parseIso8601Utc(const char* s) {
    if (!s || !*s) return 0;
    int y, mo, d, h, mi, se;
    if (sscanf(s, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se) != 6) return 0;
    struct tm tm = {};
    tm.tm_year = y - 1900;
    tm.tm_mon  = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min  = mi;
    tm.tm_sec  = se;
    return mktime(&tm);
}

String TimeFmt::countdown(time_t resetsAt, time_t now) {
    if (resetsAt <= 0 || now <= 0) return String("--");
    long secs = (long)(resetsAt - now);
    if (secs <= 0) return String("now");

    long days  = secs / 86400;
    long hours = (secs % 86400) / 3600;
    long mins  = (secs % 3600) / 60;

    char buf[24];
    if (days > 0)       snprintf(buf, sizeof(buf), "%ldd %ldh", days, hours);
    else if (hours > 0) snprintf(buf, sizeof(buf), "%ldh %ldm", hours, mins);
    else                snprintf(buf, sizeof(buf), "%ldm", mins);
    return String(buf);
}

String TimeFmt::age(time_t last, time_t now) {
    if (last <= 0) return String("never");
    long secs = (long)(now - last);
    if (secs < 45) return String("now");
    char buf[24];
    if (secs < 3600)      snprintf(buf, sizeof(buf), "%ldm ago", secs / 60);
    else if (secs < 86400) snprintf(buf, sizeof(buf), "%ldh ago", secs / 3600);
    else                   snprintf(buf, sizeof(buf), "%ldd ago", secs / 86400);
    return String(buf);
}

String TimeFmt::date(time_t t) {
    if (t <= 0) return String("--");
    struct tm tmv;
    gmtime_r(&t, &tmv);
    char buf[12];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
    return String(buf);
}

String TimeFmt::mmss(int seconds) {
    if (seconds < 0) seconds = 0;
    char buf[12];
    snprintf(buf, sizeof(buf), "%d:%02d", seconds / 60, seconds % 60);
    return String(buf);
}

String TimeFmt::uptime(unsigned long ms) {
    unsigned long secs = ms / 1000;
    char buf[24];
    if (secs >= 86400)   snprintf(buf, sizeof(buf), "%lud %luh", secs / 86400, (secs % 86400) / 3600);
    else if (secs >= 3600) snprintf(buf, sizeof(buf), "%luh %lum", secs / 3600, (secs % 3600) / 60);
    else                   snprintf(buf, sizeof(buf), "%lum", secs / 60);
    return String(buf);
}
