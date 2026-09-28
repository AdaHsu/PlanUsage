# CLAUDE.md

Notes for future sessions working on this repo.

## What this is

ESP32-S3 firmware for a LilyGo T-Display S3 that shows subscription usage for
several AI accounts at once. It talks to the vendors directly over HTTPS; there
is no companion app. Structure and board handling are modelled on
`github.com/alberthorta/ClaudeStatsPortable` (MIT), but the core is a rewrite
around a pluggable provider layer.

## Board

- LilyGo T-Display S3, ESP32-S3, 16 MB flash, 8 MB PSRAM.
- ST7789 170x320 parallel 8-bit; the TFT_eSPI pin map lives in `platformio.ini`.
- KEY on GPIO14, BOOT on GPIO0 (both active low).
- **GPIO15 = LCD_POWER_ON must be driven HIGH** or the panel is dark on
  battery while the buttons keep working. Handled in `Display::begin()`.
- Battery on GPIO4 through a 2:1 divider: `analogReadMilliVolts(4) * 2`.
- Uses the stock `default_16MB.csv` partition table, unmodified. A custom
  table that enlarged NVS was tried and tested reliably in a boot loop on
  real hardware (structurally valid partition binary, checksum and all - the
  fault was never actually pinned down) - do not reintroduce one without
  flashing real hardware to prove it boots. Account credentials go on
  LittleFS instead, which sidesteps the whole problem: see Storage below.

## Build

```bash
pio run                       # compile
pio run -t upload             # flash
pio device monitor -b 115200  # logs
```

## The one design rule

**A provider module owns its data, its screen and its settings form.** The
shell hands it a rectangle and a clock and knows nothing else about it.
`src/core/provider.h` is the whole contract. Adding a vendor is a directory
under `src/providers` plus one line in `registry.cpp` - if a change wants to
touch `main.cpp`, `screens.cpp` or `webconfig.cpp` to support a specific
vendor, the abstraction is being bypassed.

Corollaries worth keeping:

- Vendor-specific warnings (a token expiring, a re-auth prompt) are drawn by
  the module inside `show()`. They do not go on the Info screen, which is
  device-level only and deliberately short.
- The shell draws the identity header (dot, vendor, label, badge, `3/6`) so
  every vendor looks consistent. Modules should not repeat it.
- `webconfig.cpp` renders `formHtml()` and feeds `applyForm()`. It never
  learns what a sessionKey or an access token is.

## Fetch gates (`core/carousel.cpp`)

Only `providers[activeIndex]` is ever refreshed. A switch does not fetch:

1. `SWITCH_SETTLE_MS` (5 s) - restarted by every press, so a burst through the
   list sends nothing.
2. `SWITCH_MIN_AGE_MS` (3 min) - after settling, only refetch if the cache is
   older than this.

Outside a switch the cadence is `refreshMin` from device settings (default 5,
range 1-60). A module may ask for something slower via `refreshIntervalMs()`;
it can never ask for faster. `dueForFetch()` mutates switch state, so call it
exactly once per tick.

**A provider's `refresh()` must call `markFetched(now)` unconditionally, as
the first thing it does** - not only on the success path. `hasFetched()`
(`lastFetchMs_ != 0`) is what gates the fetch above; if only success sets it,
a bad credential or a network error leaves it permanently false, and
`dueForFetch()` returns true on every single loop tick forever - a retry
storm, not the configured cadence. This was a real bug (both Claude and
OpenAI shipped with it) fixed by moving the call to the top of `refresh()`
instead of the end. Any new provider copying an old one as a template should
copy the fixed shape, not reintroduce the bug.

## Buttons (`main.cpp`)

KEY presses accumulate into a burst that closes `DOUBLE_CLICK_MS` (280 ms)
after the last press: two presses toggle Stats/Info, anything else advances
that many accounts. Do not "fix" the 280 ms latency by acting on the first
press - that is what makes one button carry both actions, and it is why fast
repeated presses skip accounts instead of flipping views.

Sleep tiers follow the upstream design: BOOT 2 s backlight off, idle light
sleep (timer wake refreshes the visible account silently, button wake
restarts because peripherals come back half-initialised), BOOT+KEY 5 s deep
sleep. `keyWakeOnly` / `bootWakeOnly` swallow the press that wakes the screen.

## Data sources

```
GET https://claude.ai/api/organizations               Cookie: sessionKey=...
GET https://claude.ai/api/organizations/{org}/usage   -> five_hour, seven_day
GET https://chatgpt.com/backend-api/wham/usage        Authorization: Bearer ...
                                                      ChatGPT-Account-Id, originator
POST https://auth.openai.com/oauth/token              refresh_token grant
```

Both usage endpoints are private APIs. Parse defensively: a missing field
degrades the screen, it does not fail the fetch. Before changing a parser,
confirm the shape with curl from a computer rather than guessing - the field
names below were wrong on the first attempt precisely because they came from
secondhand research instead of a live response.

`refresh_token` rotates. Automatic renewal is opt-in per account and off by
default because the device winning a rotation signs the desktop Codex CLI out.

**Confirmed against a live response** (2026-09, Plus plan) - the wham/usage
windows are nested one level deeper than the secondhand research this was
originally written from suggested:

```
rate_limit.primary_window / .secondary_window     (not "primary"/"secondary")
  { used_percent, limit_window_seconds, reset_after_seconds, reset_at }
additional_rate_limits[]                           (not flat - nested)
  { limit_name, metered_feature, normal_model_slug,
    rate_limit: { primary_window, secondary_window } }
rate_limit_reset_credits.available_count            manual early-reset credits
```

Claude: an org returning `five_hour: null, seven_day: null` with a 200 and no
error is not necessarily broken - it may genuinely be an unused org. Every
Anthropic login has an auto-created default org named
`{email}'s Organization`; if the person actually works through a named Team
org instead, that default one will legitimately show nothing forever. Confirm
in the claude.ai UI itself (switch to that org, check its own usage page)
before assuming the parser or the fetch is at fault.

## Storage (`core/account.cpp`, `core/settings.cpp`)

Two backends, split by size:

- **Accounts live on LittleFS** as a single JSON array at `/accounts.json`
  (`Accounts::begin()` mounts it, formatting on first boot - call before
  anything touches the account list). A Codex access token alone is 1-2 KB;
  a dozen of those plus Claude session keys would crowd the stock ~20 KB nvs
  partition and hit NVS's own per-value size ceiling besides. `settings` is
  a real JSON sub-object on disk, round-tripped to/from the opaque string the
  `Provider` interface uses - still never parsed outside the module that
  wrote it.
- **Everything small stays in NVS**, namespace `planusage`: WiFi (`w<N>s/p`
  plus `wN`), device settings, and the one-int `actIdx` (active account).

## Web UI

WiFi network suggestions (provisioning's setup step and the admin page's WiFi
section) are rendered as tappable "SSID (N dBm)" buttons that fill the text
field via a one-line `onclick`, not an HTML `<datalist>`. Datalist is the
"correct" zero-JS way to do this, but iOS's Captive Network Assistant (the
mini browser it uses for WiFi setup portals) mostly does not render it at
all - confirmed on real hardware, not a theoretical concern. The shared
scan/dedupe/sort logic lives in `net/wifi_scan.{h,cpp}`; the chip-rendering
and the JS-string escaping it needs live in `WebUi::wifiChips()` /
`WebUi::jsEscape()` in `web/webui.{h,cpp}`. Both surfaces call the same code;
don't reintroduce a second copy.

The admin page's WiFi section only scans on an explicit "Scan for networks"
click, never on page load - the device is already associated at that point,
and hopping channels to scan causes a brief connectivity hiccup that should
not happen just from opening the page. Provisioning's captive portal, by
contrast, scans immediately on open since there is no existing connection to
disturb.

The Accounts list has a "Show" button per row (hidden on whichever row is
already active, marked with an "on screen" pill instead) that jumps the
device straight to that account - `POST /account/switch` ->
`Hooks::onSwitchTo` -> `Carousel::setActiveIndex()`. That function already
existed on `Carousel` from early on but was never wired to anything until
this; it goes through the same settle/age fetch gates and switch-overlay
feedback (`showSwitchFeedback()` in `main.cpp`, shared with the physical
KEY path) a button-driven switch would, so jumping several accounts via the
web in a row still costs no extra fetches.

## Logo

Two source images in `images/`, both with "Plan Usage" baked into the
artwork itself (drawn as part of the image, not overlaid as separate text -
a deliberate choice, see below): `Banner.png` (760x403, admin page + README)
and `Splash.png` (320x160, TFT boot screen - note this is authored close to
but not exactly the screen's 320x170, `gen_logo.ps1` resizes it the rest of
the way). `scripts/gen_logo.ps1` regenerates every embedded copy from these
two files - there is no build-time step for this, the generated headers are
checked in like any other source file:

- `src/web/banner_png.h` - 480x255 PNG resized from `Banner.png`, served at
  `GET /banner.png`. `brand()` renders it full-width (css `max-width:440px`)
  as the admin page's header image, replacing what used to be a small icon
  next to an `<h1>` - the banner carries the wordmark now, so there is no
  separate text heading to keep in sync with it.
- `src/web/logo_png.h` - 64x64 PNG, **cropped** from `Banner.png`'s icon
  badge (crop box `92,68,100,80` against the 760x403 source - hand-found,
  re-check it if the icon moves within a future banner revision), served at
  `GET /logo.png` and referenced as the browser tab favicon
  (`<link rel=icon>` in `HEAD_META`).
- `src/ui/splash_bitmap.h` - 320x170 RGB565 resized from `Splash.png`, drawn
  full-bleed (`pushImage(0, 0, ...)`, no centering/margin) by
  `Screens::splash()` right after `Display::setRotation()` in `setup()`.
  Only drawn when `Display::landscape()` - there is no portrait-shaped
  asset, and stretching or cropping this one into 170x320 would look worse
  than the plain background a portrait boot shows instead. Pre-blended
  against `Ui::COLOR_BG` at generation time, since RGB565 carries no alpha
  channel - regenerate through the script (which does this blending) rather
  than hand-rolling a different conversion, or the rounded corners will pick
  up a hard color-key fringe.

Both `registerLogoRoute()` (registers `/logo.png` and `/banner.png` despite
the name) call sites and `WebUi::brand()` itself are code, not asset
regeneration - only the three files above need `gen_logo.ps1` re-run when
the source art changes.

An earlier square `Logo.png` (single source, centered-icon splash, small
inline web icon + `<h1>` text) was replaced entirely by this pair once real
hardware showed it illegible at both target sizes - resizing a busy
300x300 illustration down to a 26px web icon or a 72px TFT icon loses
real information no amount of sharpening recovers. `Splash.png` and
`Banner.png` are native-resolution, single-composition designs for their
specific canvas instead, which is why "Plan Usage" is baked into each as
art rather than drawn as separate overlaid text - keep that shape if either
is revised again: dedicated art per target size, not one source scaled to
fit everything.

The script uses .NET's `System.Drawing` via `pwsh`, not ImageMagick/PIL/
ffmpeg - none of those were available in the environment this was built in.
If that's no longer true, the script doesn't need to change on that account
alone.

**Finding a crop box by eyeballing a preview is slow and was wrong the first
time** - the original `logo_png.h` favicon crop shipped, looked fine in an
isolated 100x100 test crop, but the real `/logo.png` response (64x64, after
the resize the pipeline actually does) caught a slice of the outer banner
border instead of centering the icon badge. Two lessons, not one:

1. **Verify the actual final asset, not an intermediate step.** A crop that
   looks right before the resize can still be off after it - always inspect
   the exact size/pipeline the code ships, not a convenient stand-in.
2. **Read coordinates off a grid instead of guessing-and-checking.** Render
   the source region upscaled (2-3x) with a coordinate grid and axis labels
   burned in (`Graphics.DrawLine` + `DrawString` in a loop, cheap to throw
   together in the same `pwsh`/`System.Drawing` toolchain already in use),
   inspect that once, and read the true edges directly. This converged in
   one pass; blind guess-render-look-adjust had already burned several
   rounds before it found the actual bug.

## Researched, not built

Two things were investigated in depth and deliberately not implemented -
read this before redoing the research from scratch.

**Claude / OpenAI API remaining credit balance** (the pay-as-you-go developer
API's prepaid balance, not the subscription usage this firmware already
shows): confirmed dead end as of 2026-09. Neither vendor exposes this through
any API, documented or otherwise - both have open feature requests for it on
their own GitHub repos and the balance is Console/Dashboard-web-UI-only.
Don't spend time re-searching this; check whether either vendor has since
shipped the feature request before trying again.

**OpenAI API spend/usage by model** (different from the above - this is
*already-spent* tracking, which the user considers first-class, not a
consolation prize): `GET /v1/organization/usage/completions` and `/costs`,
supports `group_by=model`, time-bucketed. Needs an Admin API key
(`sk-admin-...`), a different and more privileged credential than the Codex
module's ChatGPT access token. Not yet verified against a live response -
get a real Admin key and curl it before writing a parser, same discipline as
everything else in this file. Would be its own provider type (different
auth, different data shape from the Codex/ChatGPT subscription module),
picked up under a name like `openai_api` rather than folded into `openai`.

**GitHub Copilot usage** - personal plan only, by the user's own choice (they
are not an org owner/billing manager anywhere Copilot is provided, so the
org-managed mode has no one to serve and should not be built as a stub
either - don't half-implement it "for later").
- The endpoint (`GET /users/{username}/settings/billing/premium_request/usage`)
  needs a **GitHub App** user access token with the `Plan` permission,
  obtained via OAuth **Device Flow** (same UX shape as `gh auth login`:
  show a URL and a code, user authorizes in a browser). It does not accept a
  plain PAT.
- This is a GitHub App, not a classic OAuth App - the `Plan` permission only
  exists on GitHub Apps. Registration needs Device Flow enabled, the `Plan`
  account permission set to read-only, and "Where can this be installed" set
  to Any account (so anyone building this firmware can device-flow into one
  shared app registration, not just the maintainer).
- The org/enterprise-managed-seat endpoints exist but require the caller to
  be an org owner or billing manager - an ordinary seat holder cannot see
  their own usage through the API at all, by GitHub's own design. Confirm
  this hasn't changed before ever building that path.
- Client ID for such an app is meant to be public (same as `gh`'s own,
  embedded in its open-source client) - safe to hardcode in source once
  registered.

## What not to do

- Do not add a companion app, bridge or local JSONL parsing.
- Do not put vendor details in the shell, or device details in a module.
- Do not fetch for accounts that are not on screen, including "just to warm
  the cache" on the Info screen.
- Do not show a stored credential in full anywhere in the web UI.
- Do not load a CDN stylesheet or font: the captive portal runs with no
  internet at all.
- Do not parse timestamps as local time. TZ is pinned to UTC0 at boot.

## Git / publish

Repo: `github.com/AdaHsu/PlanUsage` (private, renamed from TokenUsage - GitHub
redirects the old URL automatically, but update any local remote still
pointing at `.../TokenUsage.git`). Licensed **AGPL-3.0** -
GitHub generated this at repo creation; note it is *not* MIT like the
upstream project this is structured after, so don't describe the two as
having the same license. `README.md` / `README.zh-TW.md` are kept in sync
(English is the source of truth; the Chinese one is a full translation, not
a summary) and cross-link each other at the top. `images/` holds real
on-device photos used in both READMEs.
