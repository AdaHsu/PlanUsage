#pragma once
#include "../../core/provider.h"

// Reads the claude.ai "Extra usage" prepaid credit balance - a private
// endpoint distinct from the subscription usage ClaudeProvider shows (a
// different URL, a different data shape: a dollar balance plus grant
// history rather than rate-limit windows). It gets its own provider rather
// than a second screen bolted onto ClaudeProvider's.
//
// One instance is one (sessionKey, orgId) pair, same shape as ClaudeProvider,
// but the two providers do not share stored credentials: re-pasting a
// rotated sessionKey on one does not update the other (webconfig's "apply to
// all" only matches rows of the same provider type).
class ClaudeCreditsProvider : public Provider {
public:
    const char* typeId()   const override { return "claude_credits"; }
    // Same vendor name as ClaudeProvider so the header reads "Claude /
    // <label> [CREDITS]" - one consistent "vendor / label [badge]" shape,
    // with the badge (not the name) telling the two providers apart.
    const char* typeName() const override { return "Claude"; }
    uint16_t    accent()   const override;
    String      subLabel() const override { return "CREDITS"; }

    FetchResult refresh(time_t now) override;
    // 0 (unset) defers to the device-wide cadence, same as every other
    // provider. Set from this account's own settings form below - once
    // extra-usage draw-down starts it can empty the balance within minutes,
    // faster than most users would want the device-wide cadence set to, so
    // this is the one provider where asking for a tighter-than-default
    // interval is worth a dedicated field.
    uint32_t    refreshIntervalMs() const override { return refreshMin_ > 0 ? (uint32_t)refreshMin_ * 60UL * 1000UL : 0; }
    bool        hasData() const override { return credits_.valid; }
    void        show(const RenderCtx& ctx) override;

    void   loadSettings(const String& json) override;
    String saveSettings() const override;
    String formHtml() const override;
    bool   applyForm(const FormSource& form, String& err) override;

    bool supportsDiscovery() const override { return true; }
    bool discover(std::vector<Workspace>& out, String& err) override;
    void selectWorkspace(const Workspace& w) override;

    String credentialId()   const override;
    String credentialHint() const override;
    String credentialSecret() const override { return sessionKey_; }
    void   setCredential(const String& cred) override;

private:
    // The most recent grant only (dashboard-style "last top-up"). The
    // response has no per-draw transaction log yet - see CLAUDE.md - so
    // there is nowhere to source a "last 3 draws" list until one turns up.
    struct Credits {
        bool   valid                = false;
        long   amountMinor          = 0;
        int    exponent             = 2;
        String currency             = "USD";
        bool   hasGrant             = false;
        time_t lastGrantedAt        = 0;
        long   lastGrantAmountMinor = 0;
    };

    FetchResult fetchOrgId();

    String  sessionKey_;
    String  orgId_;
    String  orgName_;
    int     refreshMin_ = 0;   // 0 = unset, defer to the device-wide cadence
    Credits credits_;
};
