#include "webui.h"
#include "../core/provider.h"  // htmlEscape()
#include "logo_png.h"
#include "banner_png.h"

const char WebUi::HEAD_META[] =
    "<meta charset=utf-8>"
    "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
    "<link rel=icon type=image/png href=/logo.png>";

const char WebUi::STYLES[] =
    ":root{color-scheme:dark}"
    "*{box-sizing:border-box}"
    "body{margin:0;padding:24px 16px;background:#080a10;color:#e8eaf0;"
    "font:15px/1.5 -apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif}"
    ".card{max-width:760px;margin:0 auto}"
    ".brand{margin-bottom:18px;text-align:center}"
    ".brand img{display:block;width:100%;max-width:440px;margin:0 auto;"
    "border-radius:14px}"
    ".brand .badge{display:inline-block;margin-top:10px}"
    ".badge{font-size:11px;letter-spacing:.06em;text-transform:uppercase;"
    "border:1px solid #3a4152;border-radius:999px;padding:2px 8px;color:#98a0b0}"
    "section{background:#11141c;border:1px solid #232838;border-radius:12px;"
    "padding:16px;margin-bottom:16px}"
    "h2{font-size:14px;letter-spacing:.06em;text-transform:uppercase;"
    "color:#98a0b0;margin:0 0 12px;font-weight:600}"
    "label{display:block;margin:10px 0 4px;font-size:13px;color:#b6bdcc}"
    "label.check{display:flex;align-items:center;gap:8px;margin-top:12px;color:#e8eaf0}"
    "input[type=text],input[type=password],input:not([type]),select{width:100%;"
    "padding:9px 10px;background:#0b0e15;border:1px solid #2b3245;border-radius:8px;"
    "color:#e8eaf0;font:14px inherit}"
    "input[type=number]{width:96px;padding:9px 10px;background:#0b0e15;"
    "border:1px solid #2b3245;border-radius:8px;color:#e8eaf0;font:14px inherit}"
    "button,.btn{display:inline-block;padding:9px 14px;border-radius:8px;border:0;"
    "background:#3d7dff;color:#fff;font:600 14px inherit;cursor:pointer;text-decoration:none}"
    ".btnGhost{background:#1a1f2b;color:#c9d0de;border:1px solid #2b3245}"
    ".btnDanger{background:#3a1c1f;color:#f0a0a0;border:1px solid #5a2a2e}"
    ".row{display:flex;align-items:center;gap:10px;padding:10px 0;"
    "border-bottom:1px solid #1d2230}"
    ".row:last-child{border-bottom:0}"
    ".row .grow{flex:1;min-width:0}"
    ".row .name{font-weight:600}"
    ".row .sub{font-size:12px;color:#8b93a4}"
    ".pill{font-size:11px;padding:1px 7px;border-radius:999px;border:1px solid #2b3245;"
    "color:#98a0b0}"
    ".pill.warn{border-color:#5a2a2e;color:#f0a0a0}"
    ".hint{font-size:12px;color:#8b93a4;margin:6px 0 0}"
    ".hint code{background:#0b0e15;padding:1px 5px;border-radius:4px}"
    ".err{background:#3a1c1f;border:1px solid #5a2a2e;color:#f0a0a0;"
    "padding:10px 12px;border-radius:8px;margin-bottom:14px}"
    ".ok{background:#12301f;border:1px solid #1f5a38;color:#8fe0b0;"
    "padding:10px 12px;border-radius:8px;margin-bottom:14px}"
    ".actions{display:flex;gap:8px;margin-top:14px;flex-wrap:wrap}"
    ".mini{padding:5px 9px;font-size:13px}"
    "form.inline{display:inline}"
    "a{color:#7fb0ff}";

String WebUi::page(const String& title, const String& body) {
    String html;
    html.reserve(body.length() + sizeof(STYLES) + 512);
    html += "<!doctype html><html><head>";
    html += HEAD_META;
    html += "<title>";
    html += title;
    html += "</title><style>";
    html += STYLES;
    html += "</style></head><body><div class=card>";
    html += body;
    html += "</div></body></html>";
    return html;
}

String WebUi::brand(const char* badge) {
    String s = "<div class=brand>";
    s += "<img src=/banner.png alt=\"Plan Usage\">";
    if (badge) {
        s += "<span class=badge>";
        s += badge;
        s += "</span>";
    }
    s += "</div>";
    return s;
}

String WebUi::jsEscape(const String& s) {
    String out;
    out.reserve(s.length() + 4);
    for (size_t i = 0; i < s.length(); i++) {
        char c = s[i];
        if (c == '\\' || c == '\'') out += '\\';
        out += c;
    }
    return out;
}

void WebUi::registerLogoRoute(WebServer& server) {
    server.on("/logo.png", HTTP_GET, [&server]() {
        server.send_P(200, "image/png", (const char*)LOGO_PNG, LOGO_PNG_LEN);
    });
    server.on("/banner.png", HTTP_GET, [&server]() {
        server.send_P(200, "image/png", (const char*)BANNER_PNG, BANNER_PNG_LEN);
    });
}

String WebUi::wifiChips(const std::vector<ScannedNetwork>& nets, const char* targetInputId) {
    if (nets.empty()) return String();

    String s = "<div class=actions>";
    for (const auto& n : nets) {
        String onclick = "document.getElementById('" + String(targetInputId) + "').value='" +
                         jsEscape(n.ssid) + "';return false;";
        s += "<button type=button class=\"btnGhost mini\" onclick=\"";
        s += htmlEscape(onclick);
        s += "\">" + htmlEscape(n.ssid) + " (" + String(n.rssi) + " dBm)</button>";
    }
    s += "</div>";
    return s;
}
