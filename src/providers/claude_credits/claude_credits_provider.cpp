#include "claude_credits_provider.h"
#include "../../net/http.h"
#include "../../ui/widgets.h"
#include "../../util/timefmt.h"
#include <ArduinoJson.h>

namespace {

const char* API_ORGS = "https://claude.ai/api/organizations";

// Ui::COLOR_DANGER (235,80,80) reads as coral/salmon on the actual panel,
// not red - fine for the shared "something's wrong" tint elsewhere, but too
// washed out for a "go top up" alarm that is meant to read as unmistakably
// red. Local to this file rather than changing the shared constant, which
// other screens' error states also rely on.
const uint16_t COLOR_LOW_BALANCE = Ui::rgb565(255, 40, 40);

// claude.ai wants a browser-shaped request; a bare fetch gets bounced.
void buildHeaders(const String& sessionKey, Http::Header* out) {
    out[0] = { "Cookie",     "sessionKey=" + sessionKey };
    out[1] = { "Accept",     "application/json" };
    out[2] = { "User-Agent", "PlanUsage/0.1 (ESP32)" };
}

// amount_minor / 10^exponent, split into the amount (with a "$" prefix for
// USD) and the ISO code separately, so a caller can draw them in different
// colors - "$" alone is ambiguous (TWD, HKD and AUD all use it too), which is
// why the code is always spelled out rather than assumed from the symbol.
void splitMoney(long amountMinor, int exponent, const String& currency,
                String& amountOut, String& currencyOut) {
    if (exponent < 0) exponent = 0;
    if (exponent > 4) exponent = 4;
    double divisor = 1;
    for (int i = 0; i < exponent; i++) divisor *= 10;

    char buf[32];
    snprintf(buf, sizeof(buf), "%.*f", exponent, amountMinor / divisor);
    String amt(buf);

    amountOut   = (currency.isEmpty() || currency == "USD") ? ("$" + amt) : amt;
    currencyOut = currency;
}

String formatMoney(long amountMinor, int exponent, const String& currency) {
    String amt, cur;
    splitMoney(amountMinor, exponent, currency, amt, cur);
    return cur.isEmpty() ? amt : (amt + " " + cur);
}

}  // namespace

uint16_t ClaudeCreditsProvider::accent() const { return Ui::COLOR_CLAUDE; }

String ClaudeCreditsProvider::credentialId() const {
    if (sessionKey_.length() < 8) return String();
    return sessionKey_.substring(sessionKey_.length() - 12);
}

String ClaudeCreditsProvider::credentialHint() const { return maskCredential(sessionKey_); }

void ClaudeCreditsProvider::setCredential(const String& cred) {
    if (cred.isEmpty() || cred == sessionKey_) return;
    sessionKey_ = cred;
    markDirty();
}

void ClaudeCreditsProvider::selectWorkspace(const Workspace& w) {
    orgId_   = w.id;
    orgName_ = w.name;
}

void ClaudeCreditsProvider::loadSettings(const String& json) {
    JsonDocument doc;
    if (deserializeJson(doc, json)) return;
    sessionKey_  = doc["sessionKey"]  | "";
    orgId_       = doc["orgId"]       | "";
    orgName_     = doc["orgName"]     | "";
    refreshMin_  = doc["refreshMin"]  | 0;
}

String ClaudeCreditsProvider::saveSettings() const {
    JsonDocument doc;
    doc["sessionKey"] = sessionKey_;
    doc["orgId"]      = orgId_;
    doc["orgName"]    = orgName_;
    doc["refreshMin"] = refreshMin_;
    String out;
    serializeJson(doc, out);
    return out;
}

FetchResult ClaudeCreditsProvider::fetchOrgId() {
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

    markDirty();  // so the shell writes the discovered org back to NVS
    return FetchResult::Ok;
}

bool ClaudeCreditsProvider::discover(std::vector<Workspace>& out, String& err) {
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
        out.push_back(w);
    }

    if (out.empty()) {
        err = "That login has no organizations.";
        return false;
    }
    return true;
}

FetchResult ClaudeCreditsProvider::refresh(time_t now) {
    // Count this as an attempt whether it succeeds or not - see
    // ClaudeProvider::refresh() / CLAUDE.md for why this has to be first.
    markFetched(now);

    if (sessionKey_.isEmpty()) {
        setError("No sessionKey configured.");
        return FetchResult::NotConfigured;
    }

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
    String url = String(API_ORGS) + "/" + orgId_ + "/prepaid/credits";

    Http::Response res = Http::get(url, headers, 3);
    if (res.status == 401 || res.status == 403) {
        setNeedsAuth(true);
        setError("Session expired.");
        return FetchResult::Unauthorized;
    }
    if (!res.ok()) {
        // Only ever confirmed against an account with Extra usage already
        // enabled. Whether an account that never turned it on gets a 404
        // here, or a 200 with an empty/zeroed body, is unverified - treat
        // any non-2xx as a plain network error until that is seen for real.
        setError("Network error (HTTP " + String(res.status) + ").");
        return FetchResult::Network;
    }

    JsonDocument doc;
    if (deserializeJson(doc, res.body)) {
        setError("Bad response from claude.ai.");
        return FetchResult::Parse;
    }

    Credits fresh;
    fresh.amountMinor = doc["amount"]   | 0L;
    fresh.currency    = doc["currency"] | "USD";
    fresh.exponent    = doc["balance"]["credits"]["exponent"] | 2;
    fresh.valid       = true;

    // Grants only - no per-draw transaction log has turned up yet (see
    // CLAUDE.md), so the most recent tranche is as much history as this
    // screen can show today. Take the one with the latest granted_at rather
    // than assuming array order.
    JsonArrayConst tranches = doc["tranches"].as<JsonArrayConst>();
    if (!tranches.isNull()) {
        for (JsonVariantConst t : tranches) {
            time_t grantedAt = TimeFmt::parseIso8601Utc(t["granted_at"] | "");
            if (grantedAt > fresh.lastGrantedAt) {
                fresh.lastGrantedAt        = grantedAt;
                fresh.lastGrantAmountMinor = t["granted_amount_minor_units"] | 0L;
                fresh.hasGrant              = true;
            }
        }
    }

    credits_ = fresh;
    setNeedsAuth(false);
    setError("");
    return FetchResult::Ok;
}

void ClaudeCreditsProvider::show(const RenderCtx& ctx) {
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

    if (!credits_.valid) {
        String msg = lastError().isEmpty() ? String("No credit data") : lastError();
        Ui::centeredMessage(g, ctx.x, ctx.y, ctx.w, ctx.h, msg, Ui::COLOR_MUTED);
        return;
    }

    String amountPart, currencyPart;
    splitMoney(credits_.amountMinor, credits_.exponent, credits_.currency, amountPart, currencyPart);
    int midY = ctx.y + ctx.h / 2;

    // Only USD has a confirmed $5 cutoff worth alarming on - a Extra usage
    // balance in any other currency keeps the normal color rather than
    // guess at an equivalent threshold nobody has verified.
    bool lowBalance = false;
    if (credits_.currency == "USD") {
        long thresholdMinor = 5;
        for (int i = 0; i < credits_.exponent; i++) thresholdMinor *= 10;
        lowBalance = credits_.amountMinor < thresholdMinor;
    }
    uint16_t amountColor = lowBalance ? COLOR_LOW_BALANCE : Ui::COLOR_TEXT;

    // FreeSansBold18pt7b - already pulled in globally by TFT_eSPI.h's own
    // gfxfont.h whenever LOAD_GFXFF is set (platformio.ini already sets it),
    // so no #include or asset pipeline is needed here, just the symbol.
    // Sits between font 4 and a doubled font 4, drawn at native resolution
    // instead of 2x-upscaled, so it does not pick up the blocky edges
    // setTextSize(2) did. The amount and the currency code are drawn as two
    // separate strings (rather than one combined one) so the code can stay
    // dim while the amount carries the low-balance color. Falls back to the
    // plain numbered font, combined into one string, whenever the pair
    // would not fit - truncating digits off a balance would be misleading.
    g.setFreeFont(&FreeSansBold18pt7b);
    int amountW = g.textWidth(amountPart);
    g.setFreeFont();
    int gapW      = currencyPart.isEmpty() ? 0 : 6;
    int currencyW = currencyPart.isEmpty() ? 0 : g.textWidth(currencyPart, 4);
    int totalW    = amountW + gapW + currencyW;
    bool bigFits  = totalW <= ctx.w - 16;

    if (bigFits) {
        int leftX = ctx.x + ctx.w / 2 - totalW / 2;

        g.setTextDatum(ML_DATUM);
        g.setTextColor(amountColor, Ui::COLOR_BG);
        g.setFreeFont(&FreeSansBold18pt7b);
        g.drawString(amountPart, leftX, midY - 20);
        g.setFreeFont();  // always leave the GFX font cleared for later numbered-font draws

        if (!currencyPart.isEmpty()) {
            g.setTextColor(Ui::COLOR_MUTED, Ui::COLOR_BG);
            g.drawString(currencyPart, leftX + amountW + gapW, midY - 16, 4);
        }
    } else {
        g.setTextDatum(MC_DATUM);
        g.setTextColor(amountColor, Ui::COLOR_BG);
        String balance = currencyPart.isEmpty() ? amountPart : (amountPart + " " + currencyPart);
        g.drawString(Ui::fit(g, balance, ctx.w - 16, 4), ctx.x + ctx.w / 2, midY - 14, 4);
    }

    int afterBalanceY = midY + (bigFits ? 26 : 14);

    g.setTextDatum(MC_DATUM);  // the bigFits branch above leaves ML_DATUM set
    g.setTextColor(Ui::COLOR_MUTED, Ui::COLOR_BG);
    g.drawString("extra usage remaining", ctx.x + ctx.w / 2, afterBalanceY, 2);

    if (credits_.hasGrant) {
        String grant = "Last top-up " +
                       formatMoney(credits_.lastGrantAmountMinor, credits_.exponent, credits_.currency) +
                       " on " + TimeFmt::date(credits_.lastGrantedAt);
        g.drawString(Ui::fit(g, grant, ctx.w - 16, 2), ctx.x + ctx.w / 2, afterBalanceY + 24, 2);
    }

    // Deliberately nothing drawn below for "recent draws": the response has
    // no per-use transaction log, only grant tranches (see CLAUDE.md). Add
    // that line once a real endpoint for it is confirmed, not before.
}

String ClaudeCreditsProvider::formHtml() const {
    String h;
    h.reserve(768);

    h += "<label>sessionKey";
    h += "<input type=password name=p_sessionKey autocomplete=off placeholder=\"";
    h += sessionKey_.isEmpty() ? String("sk-ant-sid01-...") : maskCredential(sessionKey_);
    h += "\"></label>";
    h += "<p class=hint>Copy the <code>sessionKey</code> cookie from claude.ai in your "
         "browser dev tools. Stored separately from the Claude usage account's key - "
         "rotating one does not update the other.</p>";

    h += "<label>Organization id";
    h += "<input name=p_orgId value=\"" + htmlEscape(orgId_) + "\" placeholder=\"auto-discover\"></label>";
    h += "<p class=hint>Leave empty to use the first organization this login owns, or "
         "press Discover to list them all.</p>";

    h += "<label>Organization name";
    h += "<input name=p_orgName value=\"" + htmlEscape(orgName_) + "\"></label>";

    h += "<label>Refresh every (minutes)";
    h += "<input type=number name=p_refreshMin min=0 max=60 value=\"";
    h += refreshMin_ > 0 ? String(refreshMin_) : String("");
    h += "\" placeholder=\"device default\"></label>";
    h += "<p class=hint>Blank or 0 follows the device-wide refresh setting. Extra usage can "
         "drain fast once draw-down starts, so this account can be checked more often than "
         "the rest without speeding up every other account too.</p>";
    return h;
}

bool ClaudeCreditsProvider::applyForm(const FormSource& form, String& err) {
    String key = form.get("p_sessionKey");
    key.trim();
    // An empty field means "keep what is stored", so a masked placeholder
    // never overwrites a good credential.
    if (!key.isEmpty()) sessionKey_ = key;

    String org = form.get("p_orgId");
    org.trim();
    if (org != orgId_) {
        orgId_   = org;
        credits_ = Credits{};   // numbers from the previous org are meaningless now
    }

    orgName_ = form.get("p_orgName");
    orgName_.trim();

    String refreshStr = form.get("p_refreshMin");
    refreshStr.trim();
    int refreshMin = refreshStr.isEmpty() ? 0 : refreshStr.toInt();
    if (refreshMin < 0)  refreshMin = 0;
    if (refreshMin > 60) refreshMin = 60;
    refreshMin_ = refreshMin;

    if (sessionKey_.isEmpty()) {
        err = "A sessionKey is required.";
        return false;
    }
    setNeedsAuth(false);
    setError("");
    return true;
}
