#include "screens.h"
#include "display.h"
#include "widgets.h"
#include "../hal/battery.h"
#include "../hal/board.h"
#include "../util/timefmt.h"
#include "splash_bitmap.h"

namespace {

// Battery pill at the top right. Returns the x of its left edge so the next
// item can be placed to its left.
int drawBattery(TFT_eSPI& g, int rightX, int y, int batteryMv) {
    const int iconW = 22, iconH = 10, nubW = 2, nubH = 4;
    int iconX = rightX - iconW - nubW;

    int pct       = Battery::percent(batteryMv);
    bool charging = Battery::isCharging(batteryMv);
    uint16_t fill = (pct > 50) ? Ui::COLOR_OK
                  : (pct > 20) ? Ui::COLOR_WARN
                               : Ui::COLOR_DANGER;
    uint16_t border = charging ? Ui::COLOR_ACCENT : Ui::COLOR_MUTED;

    g.drawRect(iconX, y + 3, iconW, iconH, border);
    g.fillRect(iconX + iconW, y + 3 + (iconH - nubH) / 2, nubW, nubH, border);
    int fillW = ((iconW - 2) * pct) / 100;
    if (fillW > 0) g.fillRect(iconX + 1, y + 4, fillW, iconH - 2, fill);

    char buf[8];
    if (charging) snprintf(buf, sizeof(buf), "USBC");
    else          snprintf(buf, sizeof(buf), "%d%%", pct);
    g.setTextDatum(TR_DATUM);
    g.setTextColor(charging ? Ui::COLOR_ACCENT : Ui::COLOR_MUTED, Ui::COLOR_BG);
    g.drawString(buf, iconX - 4, y, 2);

    return iconX - 4 - g.textWidth(buf, 2) - 8;
}

// Identity block: coloured dot, vendor name, account label, kind badge.
// Returns the x just past whatever it drew.
int drawIdentity(TFT_eSPI& g, Provider& p, int x, int y, int maxRight, bool withLabel) {
    int cx = x + 4;
    g.fillCircle(cx, y + 8, 3, p.accent());
    int tx = cx + 7;

    g.setTextDatum(TL_DATUM);
    g.setTextColor(Ui::COLOR_TEXT, Ui::COLOR_BG);
    String name = p.typeName();
    g.drawString(name, tx, y, 2);
    tx += g.textWidth(name, 2) + 6;

    if (withLabel && !p.label().isEmpty()) {
        g.setTextColor(Ui::COLOR_MUTED, Ui::COLOR_BG);
        g.drawString("/", tx, y, 2);
        tx += g.textWidth("/", 2) + 5;

        String sub  = p.subLabel();
        int reserve = sub.isEmpty() ? 0 : Ui::badgeWidth(g, sub) + 6;
        String lbl  = Ui::fit(g, p.label(), maxRight - tx - reserve, 2);
        g.setTextColor(Ui::COLOR_TEXT, Ui::COLOR_BG);
        g.drawString(lbl, tx, y, 2);
        tx += g.textWidth(lbl, 2) + 6;
    }

    String sub = p.subLabel();
    if (!sub.isEmpty() && tx + Ui::badgeWidth(g, sub) <= maxRight) {
        tx += Ui::badge(g, tx, y + 2, sub, p.accent()) + 6;
    }
    return tx;
}

// Returns the y where the content area starts.
int drawHeader(TFT_eSPI& g, Provider& p, const Screens::Chrome& c) {
    const int W = Display::width();
    const bool landscape = Display::landscape();
    const int y = 4;

    int rightEdge = drawBattery(g, W - 8, y, c.batteryMv);

    // Position in the list, then the countdown to the next refresh. Portrait
    // only has room for the position.
    char pos[12];
    snprintf(pos, sizeof(pos), "%d/%d", c.index + 1, c.total);
    g.setTextDatum(TR_DATUM);

    if (landscape) {
        String refresh = c.loading ? String("...") : TimeFmt::mmss(c.refreshSec);
        g.setTextColor(Ui::COLOR_MUTED, Ui::COLOR_BG);
        g.drawString(refresh, rightEdge, y, 2);
        rightEdge -= g.textWidth(refresh, 2) + 10;
    }

    g.setTextColor(Ui::COLOR_TEXT, Ui::COLOR_BG);
    g.drawString(pos, rightEdge, y, 2);
    rightEdge -= g.textWidth(pos, 2) + 10;

    if (c.stale) {
        g.setTextColor(Ui::COLOR_DANGER, Ui::COLOR_BG);
        g.drawString("CACHED", rightEdge, y + 2, 1);
        rightEdge -= g.textWidth("CACHED", 1) + 8;
    }
    if (!c.wifiOk) {
        g.setTextColor(Ui::COLOR_DANGER, Ui::COLOR_BG);
        g.drawString("OFFLINE", rightEdge, y + 2, 1);
        rightEdge -= g.textWidth("OFFLINE", 1) + 8;
    }

    if (landscape) {
        drawIdentity(g, p, 4, y, rightEdge, true);
        g.drawFastHLine(0, 22, W, Ui::COLOR_DIVIDER);
        return 24;
    }

    // Portrait: vendor and badge on the first line, the account label on its
    // own line underneath.
    drawIdentity(g, p, 4, y, rightEdge, false);
    g.setTextDatum(TL_DATUM);
    g.setTextColor(Ui::COLOR_TEXT, Ui::COLOR_BG);
    g.drawString(Ui::fit(g, p.label(), W - 16, 2), 8, y + 18, 2);
    g.drawFastHLine(0, 40, W, Ui::COLOR_DIVIDER);
    return 42;
}

}  // namespace

void Screens::stats(Provider& provider, const Chrome& chrome, time_t now) {
    Display::beginFrame();
    TFT_eSPI& g = Display::canvas();

    int top = drawHeader(g, provider, chrome);

    // The module owns everything below the header.
    RenderCtx ctx{ g, 0, top, Display::width(), Display::height() - top,
                   now, Display::landscape() };
    provider.show(ctx);

    Display::endFrame();
}

void Screens::switchOverlay(Provider& provider, int index, int total) {
    Display::beginFrame();
    TFT_eSPI& g = Display::canvas();
    const int W = Display::width(), H = Display::height();

    g.setTextDatum(MC_DATUM);
    int nameW = g.textWidth(provider.typeName(), 4);
    g.fillCircle(W / 2 - nameW / 2 - 12, H / 2 - 22, 5, provider.accent());
    g.setTextColor(Ui::COLOR_MUTED, Ui::COLOR_BG);
    g.drawString(provider.typeName(), W / 2, H / 2 - 22, 4);

    g.setTextColor(Ui::COLOR_TEXT, Ui::COLOR_BG);
    g.drawString(Ui::fit(g, provider.label(), W - 20, 4), W / 2, H / 2 + 6, 4);

    char pos[12];
    snprintf(pos, sizeof(pos), "%d/%d", index + 1, total);
    g.setTextColor(Ui::COLOR_MUTED, Ui::COLOR_BG);
    g.drawString(pos, W / 2, H / 2 + 34, 2);

    Display::endFrame();
}

void Screens::info(const DeviceInfo& info) {
    Display::beginFrame();
    TFT_eSPI& g = Display::canvas();
    const int W = Display::width();

    g.setTextDatum(TL_DATUM);
    g.setTextColor(Ui::COLOR_ACCENT, Ui::COLOR_BG);
    g.drawString("Plan Usage", 8, 4, 2);
    g.setTextDatum(TR_DATUM);
    g.setTextColor(Ui::COLOR_MUTED, Ui::COLOR_BG);
    g.drawString(String("v") + Board::VERSION, W - 8, 4, 2);
    g.drawFastHLine(0, 22, W, Ui::COLOR_DIVIDER);

    char batBuf[32];
    snprintf(batBuf, sizeof(batBuf), "%d%%  %d.%02d V",
             Battery::percent(info.batteryMv),
             info.batteryMv / 1000, (info.batteryMv / 10) % 100);
    char wifiBuf[64];
    snprintf(wifiBuf, sizeof(wifiBuf), "%s  %d dBm", info.ssid.c_str(), info.rssi);

    struct Row { const char* key; String value; };
    Row rows[] = {
        { "WiFi", info.ssid.isEmpty() ? String("not connected") : String(wifiBuf) },
        { "IP",   info.ip.isEmpty() ? String("-") : info.ip },
        { "Host", info.host },
        { "Batt", String(batBuf) },
        { "Up",   TimeFmt::uptime(info.uptimeMs) + "   Refresh " + String(info.refreshMin) + " min" },
    };

    int y = 28;
    const int lineH = Display::landscape() ? 26 : 30;
    for (const Row& r : rows) {
        g.setTextDatum(TL_DATUM);
        g.setTextColor(Ui::COLOR_MUTED, Ui::COLOR_BG);
        g.drawString(r.key, 8, y, 2);
        g.setTextColor(Ui::COLOR_TEXT, Ui::COLOR_BG);
        g.drawString(Ui::fit(g, r.value, W - 64, 2), 56, y, 2);
        y += lineH;
    }

    Display::endFrame();
}

void Screens::loading(const String& msg) {
    Display::beginFrame();
    TFT_eSPI& g = Display::canvas();
    Ui::centeredMessage(g, 0, 0, Display::width(), Display::height(), msg, Ui::COLOR_MUTED);
    Display::endFrame();
}

void Screens::connecting(const String& ssid, int index, int total) {
    Display::beginFrame();
    TFT_eSPI& g = Display::canvas();
    const int W = Display::width(), H = Display::height();

    g.setTextDatum(MC_DATUM);
    g.setTextColor(Ui::COLOR_MUTED, Ui::COLOR_BG);
    g.drawString("Connecting", W / 2, H / 2 - 24, 2);
    g.setTextColor(Ui::COLOR_TEXT, Ui::COLOR_BG);
    g.drawString(Ui::fit(g, ssid, W - 20, 4), W / 2, H / 2, 4);

    if (total > 1) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d/%d", index + 1, total);
        g.setTextColor(Ui::COLOR_MUTED, Ui::COLOR_BG);
        g.drawString(buf, W / 2, H / 2 + 26, 2);
    }
    Display::endFrame();
}

void Screens::provisioning(const String& apSsid, const String& ip) {
    Display::beginFrame();
    TFT_eSPI& g = Display::canvas();
    const int W = Display::width(), H = Display::height();

    g.setTextDatum(MC_DATUM);
    g.setTextColor(Ui::COLOR_ACCENT, Ui::COLOR_BG);
    g.drawString("Setup", W / 2, 14, 2);
    g.setTextColor(Ui::COLOR_MUTED, Ui::COLOR_BG);
    g.drawString("Join this WiFi", W / 2, H / 2 - 28, 2);
    g.setTextColor(Ui::COLOR_TEXT, Ui::COLOR_BG);
    g.drawString(apSsid, W / 2, H / 2 - 4, 4);
    g.setTextColor(Ui::COLOR_MUTED, Ui::COLOR_BG);
    g.drawString("then open " + ip, W / 2, H / 2 + 26, 2);

    Display::endFrame();
}

void Screens::message(const String& title, const String& body, uint16_t color) {
    Display::beginFrame();
    TFT_eSPI& g = Display::canvas();
    const int W = Display::width(), H = Display::height();

    g.setTextDatum(MC_DATUM);
    g.setTextColor(color, Ui::COLOR_BG);
    g.drawString(Ui::fit(g, title, W - 16, 4), W / 2, H / 2 - 16, 4);
    g.setTextColor(Ui::COLOR_MUTED, Ui::COLOR_BG);
    g.drawString(Ui::fit(g, body, W - 16, 2), W / 2, H / 2 + 14, 2);

    Display::endFrame();
}

void Screens::noAccounts(const String& url) {
    Display::beginFrame();
    TFT_eSPI& g = Display::canvas();
    const int W = Display::width(), H = Display::height();

    g.setTextDatum(MC_DATUM);
    g.setTextColor(Ui::COLOR_TEXT, Ui::COLOR_BG);
    g.drawString("No accounts yet", W / 2, H / 2 - 20, 4);
    g.setTextColor(Ui::COLOR_MUTED, Ui::COLOR_BG);
    g.drawString("Add one at", W / 2, H / 2 + 8, 2);
    g.setTextColor(Ui::COLOR_ACCENT, Ui::COLOR_BG);
    g.drawString(Ui::fit(g, url, W - 16, 2), W / 2, H / 2 + 28, 2);

    Display::endFrame();
}

void Screens::sleepArmed(const char* title, int secondsLeft, const char* subtitle) {
    TFT_eSPI& g = Display::beginDirect();
    int W = g.width(), H = g.height();
    bool portrait = W < H;

    const char* cancel = subtitle ? subtitle
                       : (portrait ? "any button cancels"
                                   : "press any button to cancel");

    g.setTextDatum(MC_DATUM);
    g.setTextColor(Ui::COLOR_MUTED, Ui::COLOR_BG);
    g.drawString(title, W / 2, H / 2 - 54, 2);

    char buf[4];
    snprintf(buf, sizeof(buf), "%d", secondsLeft);
    g.setTextColor(Ui::COLOR_ORANGE, Ui::COLOR_BG);
    g.drawString(buf, W / 2, H / 2, 7);

    g.setTextColor(Ui::COLOR_MUTED, Ui::COLOR_BG);
    g.drawString(cancel, W / 2, H / 2 + 54, 2);

    // Recreate the sprite so a cancelled countdown returns to normal drawing.
    Display::endDirect();
}

void Screens::deepSleep() {
    TFT_eSPI& g = Display::beginDirect();
    g.setTextDatum(MC_DATUM);
    g.setTextColor(Ui::COLOR_MUTED, Ui::COLOR_BG);
    g.drawString("Sleeping...", g.width() / 2, g.height() / 2, 4);
    Display::endDirect();
}

void Screens::resetting() {
    TFT_eSPI& g = Display::beginDirect();
    g.setTextDatum(MC_DATUM);
    g.setTextColor(Ui::COLOR_DANGER, Ui::COLOR_BG);
    g.drawString("Factory reset", g.width() / 2, g.height() / 2, 4);
    Display::endDirect();
}

void Screens::splash() {
    TFT_eSPI& g = Display::beginDirect();
    // The asset is authored full-bleed at the exact 320x170 landscape canvas
    // size - draw it 1:1 rather than centering a smaller icon, so nothing
    // gets scaled a second time on top of the resize already baked into
    // scripts/gen_logo.ps1's output. beginDirect() already cleared the
    // screen to Ui::COLOR_BG, so a portrait boot (no matching asset for that
    // shape yet) just shows a plain background for this one frame instead of
    // a stretched or cropped landscape image.
    if (Display::landscape()) {
        // On ESP32, flash is memory-mapped, so pushImage reads straight out
        // of the PROGMEM array with no separate copy step.
        g.pushImage(0, 0, SPLASH_BITMAP_W, SPLASH_BITMAP_H, SPLASH_BITMAP);
    }
    Display::endDirect();
}
