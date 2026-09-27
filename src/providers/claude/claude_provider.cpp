#include "claude_provider.h"
#include "../../net/http.h"
#include "../../ui/widgets.h"
#include "../../util/timefmt.h"
#include <ArduinoJson.h>

namespace {

const char* API_ORGS  = "https://claude.ai/api/organizations";
const long  FIVE_HOUR = 5L * 3600;
const long  SEVEN_DAY = 7L * 24 * 3600;

// claude.ai wants a browser-shaped request; a bare fetch gets bounced.
void buildHeaders(const String& sessionKey, Http::Header* out) {
    out[0] = { "Cookie",     "sessionKey=" + sessionKey };
    out[1] = { "Accept",     "application/json" };
    out[2] = { "User-Agent", "PlanUsage/0.1 (ESP32)" };
}

// Best effort: the capabilities array is not documented, so an unfamiliar
// account simply gets no badge rather than a wrong one.
String kindFromCapabilities(JsonVariantConst caps) {
    if (caps.isNull()) return String();
    bool team = false, pro = false, max = false, enterprise = false;
    for (JsonVariantConst v : caps.as<JsonArrayConst>()) {
        String s = v.as<const char*>() ? String(v.as<const char*>()) : String();
        s.toLowerCase();
        if (s.indexOf("raven") >= 0 || s.indexOf("team") >= 0)  team = true;
        if (s.indexOf("enterprise") >= 0)                       enterprise = true;
        if (s.indexOf("claude_max") >= 0)                       max = true;
        if (s.indexOf("claude_pro") >= 0)                       pro = true;
    }
    if (enterprise) return "ENTERPRISE";
    if (team)       return "TEAMS";
    if (max)        return "MAX";
    if (pro)        return "PRO";
    return String();
}

}  // namespace

uint16_t ClaudeProvider::accent() const { return Ui::COLOR_CLAUDE; }

String ClaudeProvider::credentialId() const {
    // Rows created from the same login share the key, which is what lets the
    // admin page re-paste an expired credential once for all of them.
    if (sessionKey_.length() < 8) return String();
    return sessionKey_.substring(sessionKey_.length() - 12);
}

String ClaudeProvider::credentialHint() const { return maskCredential(sessionKey_); }

void ClaudeProvider::setCredential(const String& cred) {
    if (cred.isEmpty() || cred == sessionKey_) return;
    sessionKey_ = cred;
    markDirty();
}

void ClaudeProvider::selectWorkspace(const Workspace& w) {
    orgId_   = w.id;
    orgName_ = w.name;
    kind_    = w.kind;
}

void ClaudeProvider::loadSettings(const String& json) {
    JsonDocument doc;
    if (deserializeJson(doc, json)) return;
    sessionKey_ = doc["sessionKey"] | "";
    orgId_      = doc["orgId"]      | "";
    orgName_    = doc["orgName"]    | "";
    kind_       = doc["kind"]       | "";
}

String ClaudeProvider::saveSettings() const {
    JsonDocument doc;
    doc["sessionKey"] = sessionKey_;
    doc["orgId"]      = orgId_;
    doc["orgName"]    = orgName_;
    doc["kind"]       = kind_;
    String out;
    serializeJson(doc, out);
    return out;
}

FetchResult ClaudeProvider::fetchOrgId() {
    Http::Header headers[3];
    buildHeaders(sessionKey_, headers);

    Http::Response res = Http::get(API_ORGS, headers, 3);
    if (res.status == 401 || res.status == 403) return FetchResult::Unauthorized;
    if (!res.ok())                              return FetchResult::Network;

    JsonDocument doc;
    if (deserializeJson(doc, res.body)) return FetchResult::Parse;

    JsonArrayConst arr = doc.as<JsonArrayConst>();
    if (arr.isNull() || arr.size() == 0) return FetchResult::Parse;

    const char* uuid = arr[0]["uuid"] | (const char*)nullptr;
    if (!uuid || !*uuid) return FetchResult::Parse;

    orgId_ = uuid;
    const char* name = arr[0]["name"] | (const char*)nullptr;
    if (name && *name && orgName_.isEmpty()) orgName_ = name;
    if (kind_.isEmpty()) kind_ = kindFromCapabilities(arr[0]["capabilities"]);

    markDirty();  // so the shell writes the discovered org back to NVS
    return FetchResult::Ok;
}

bool ClaudeProvider::discover(std::vector<Workspace>& out, String& err) {
    out.clear();
    if (sessionKey_.isEmpty()) {
        err = "Paste a sessionKey first.";
        return false;
    }

    Http::Header headers[3];
    buildHeaders(sessionKey_, headers);
    Http::Response res = Http::get(API_ORGS, headers, 3);

    if (res.status == 401 || res.status == 403) {
        err = "That sessionKey was rejected.";
        return false;
    }
    if (!res.ok()) {
        err = "claude.ai did not answer (HTTP " + String(res.status) + ").";
        return false;
    }

    JsonDocument doc;
    if (deserializeJson(doc, res.body)) {
        err = "Could not read the response.";
        return false;
    }

    for (JsonVariantConst org : doc.as<JsonArrayConst>()) {
        const char* uuid = org["uuid"] | (const char*)nullptr;
        if (!uuid || !*uuid) continue;
        Workspace w;
        w.id   = uuid;
        w.name = org["name"] | "Organization";
        w.kind = kindFromCapabilities(org["capabilities"]);
        out.push_back(w);
    }

    if (out.empty()) {
        err = "That login has no organizations.";
        return false;
    }
    return true;
}

FetchResult ClaudeProvider::refresh(time_t now) {
    // Count this as an attempt whether it succeeds or not. The carousel's
    // interval gate keys off hasFetched()/lastFetchMs(); if only a success
    // set them, an expired sessionKey or a network hiccup would leave
    // hasFetched() false forever and dueForFetch() would fire on every loop
    // tick instead of backing off - a retry storm, not the configured
    // refresh cadence.
    markFetched(now);

    if (sessionKey_.isEmpty()) {
        setError("No sessionKey configured.");
        return FetchResult::NotConfigured;
    }

    // An account added without picking an org resolves to the first one the
    // login owns, which is the common single-org case.
    if (orgId_.isEmpty()) {
        FetchResult r = fetchOrgId();
        if (r != FetchResult::Ok) {
            setNeedsAuth(r == FetchResult::Unauthorized);
            setError(r == FetchResult::Unauthorized ? "Session expired."
                                                    : "Could not find the organization.");
            return r;
        }
    }

    Http::Header headers[3];
    buildHeaders(sessionKey_, headers);
    String url = String(API_ORGS) + "/" + orgId_ + "/usage";

    Http::Response res = Http::get(url, headers, 3);
    if (res.status == 401 || res.status == 403) {
        setNeedsAuth(true);
        setError("Session expired.");
        return FetchResult::Unauthorized;
    }
    if (!res.ok()) {
        setError("Network error (HTTP " + String(res.status) + ").");
        return FetchResult::Network;
    }

    JsonDocument doc;
    if (deserializeJson(doc, res.body)) {
        setError("Bad response from claude.ai.");
        return FetchResult::Parse;
    }

    auto readWindow = [](JsonVariantConst v, Window& w) {
        w = Window{};
        if (v.isNull()) return;
        w.valid = true;
        if (!v["utilization"].isNull()) w.utilization = v["utilization"].as<double>();
        w.resetsAt = TimeFmt::parseIso8601Utc(v["resets_at"] | "");
    };

    Usage fresh;
    readWindow(doc["five_hour"], fresh.fiveHour);
    readWindow(doc["seven_day"], fresh.sevenDay);
    fresh.valid = fresh.fiveHour.valid || fresh.sevenDay.valid;

    if (!fresh.valid) {
        // A 200 with neither window present has only ever been verified
        // against a personal Pro/Max sessionKey - a Teams org may shape this
        // response differently. Dump the raw body so the parser can be fixed
        // against something real instead of guessed at.
        Serial.println("[claude] no five_hour/seven_day in response for org "
                       + orgId_ + ", raw body:");
        Serial.println(res.body);
    }

    usage_ = fresh;
    setNeedsAuth(false);
    setError("");
    return FetchResult::Ok;
}

void ClaudeProvider::drawWindows(const RenderCtx& ctx) {
    TFT_eSPI& g = ctx.g;

    Ui::Pace p5 = Ui::pace(usage_.fiveHour.utilization, usage_.fiveHour.resetsAt,
                           FIVE_HOUR, ctx.now);
    Ui::Pace p7 = Ui::pace(usage_.sevenDay.utilization, usage_.sevenDay.resetsAt,
                           SEVEN_DAY, ctx.now);

    const bool has5 = usage_.fiveHour.valid;
    const bool has7 = usage_.sevenDay.valid;

    // Room for an organization line only when the layout is tall enough,
    // which in practice means portrait.
    int footerH = (!orgName_.isEmpty() && ctx.h >= 150) ? 18 : 0;
    int areaY = ctx.y;
    int areaH = ctx.h - footerH;

    const String t5 = ctx.landscape ? "5-hour window" : "5-hour";
    const String t7 = "Weekly";

    if (has5 && has7) {
        int panelH = (areaH - 2) / 2;
        Ui::windowRow(g, ctx.x, areaY + 4, ctx.w, panelH - 4,
                      t5, usage_.fiveHour.utilization,
                      usage_.fiveHour.resetsAt, ctx.now, p5, true);
        g.drawFastHLine(8, areaY + panelH, ctx.w - 16, Ui::COLOR_DIVIDER);
        Ui::windowRow(g, ctx.x, areaY + panelH + 6, ctx.w, panelH - 4,
                      t7, usage_.sevenDay.utilization,
                      usage_.sevenDay.resetsAt, ctx.now, p7, true);
    } else if (has7 || has5) {
        // Only one window has data (a 5-hour window that has not been opened
        // yet is simply absent): give it the whole area.
        const Window& w = has7 ? usage_.sevenDay : usage_.fiveHour;
        const Ui::Pace& p = has7 ? p7 : p5;
        Ui::windowRow(g, ctx.x, areaY + (areaH / 4), ctx.w, areaH / 2,
                      has7 ? t7 : t5, w.utilization, w.resetsAt, ctx.now, p, true);
    }

    if (footerH > 0) {
        g.setTextDatum(TL_DATUM);
        g.setTextColor(Ui::COLOR_MUTED, Ui::COLOR_BG);
        g.drawString(Ui::fit(g, orgName_, ctx.w - 16, 1),
                     ctx.x + 8, ctx.y + ctx.h - footerH + 4, 1);
    }
}

void ClaudeProvider::show(const RenderCtx& ctx) {
    TFT_eSPI& g = ctx.g;

    if (needsAuth()) {
        g.setTextDatum(MC_DATUM);
        g.setTextColor(Ui::COLOR_DANGER, Ui::COLOR_BG);
        g.drawString("Re-authenticate", ctx.x + ctx.w / 2, ctx.y + ctx.h / 2 - 12, 4);
        g.setTextColor(Ui::COLOR_MUTED, Ui::COLOR_BG);
        g.drawString("paste a fresh sessionKey", ctx.x + ctx.w / 2,
                     ctx.y + ctx.h / 2 + 14, 2);
        return;
    }

    if (!usage_.valid) {
        String msg = lastError().isEmpty() ? String("No usage data") : lastError();
        Ui::centeredMessage(g, ctx.x, ctx.y, ctx.w, ctx.h, msg, Ui::COLOR_MUTED);
        return;
    }

    drawWindows(ctx);
}

String ClaudeProvider::formHtml() const {
    String h;
    h.reserve(1024);

    h += "<label>sessionKey";
    h += "<input type=password name=p_sessionKey autocomplete=off placeholder=\"";
    h += sessionKey_.isEmpty() ? String("sk-ant-sid01-...") : maskCredential(sessionKey_);
    h += "\"></label>";
    h += "<p class=hint>Copy the <code>sessionKey</code> cookie from claude.ai in your "
         "browser dev tools. Leave blank to keep the stored one.</p>";

    h += "<label>Organization id";
    h += "<input name=p_orgId value=\"" + htmlEscape(orgId_) + "\" placeholder=\"auto-discover\"></label>";
    h += "<p class=hint>Leave empty to use the first organization this login owns, or "
         "press Discover to list them all and add one row per organization.</p>";

    h += "<label>Organization name";
    h += "<input name=p_orgName value=\"" + htmlEscape(orgName_) + "\"></label>";

    h += "<label>Badge";
    h += "<input name=p_kind value=\"" + htmlEscape(kind_) + "\" placeholder=\"PRO / MAX / TEAMS\"></label>";
    return h;
}

bool ClaudeProvider::applyForm(const FormSource& form, String& err) {
    String key = form.get("p_sessionKey");
    key.trim();
    // An empty field means "keep what is stored", so a masked placeholder
    // never overwrites a good credential.
    if (!key.isEmpty()) sessionKey_ = key;

    String org = form.get("p_orgId");
    org.trim();
    if (org != orgId_) {
        orgId_ = org;
        usage_ = Usage{};   // numbers from the previous org are meaningless now
    }

    orgName_ = form.get("p_orgName");
    orgName_.trim();
    kind_ = form.get("p_kind");
    kind_.trim();
    kind_.toUpperCase();

    if (sessionKey_.isEmpty()) {
        err = "A sessionKey is required.";
        return false;
    }
    setNeedsAuth(false);
    setError("");
    return true;
}
