<div align="center">

# ⌚ AI Quota Watchface

A Pebble Time (`basalt`) watchface that shows time, weather, today's **GitHub**
commits, and your **Claude**, **ChatGPT Codex**, and **MiniMax** usage.

[![Pebble SDK 3](https://img.shields.io/badge/Pebble%20SDK-3-brightgreen)](https://developer.rebble.io)
[![Platform](https://img.shields.io/badge/Platform-basalt-blue.svg)](#)
[![Language](https://img.shields.io/badge/C%20%2B%20JavaScript-1.0.0-red.svg)](#)
[![License](https://img.shields.io/badge/license-MIT-lightgrey.svg)](#license)

![Watchface preview](assets/screenshot.png)

</div>

## ✨ Features

- **Time & date** — big bold time, date, a live countdown to 22:00, and days
  until the next March 28 birthday.
- **Weather** — today / +6h / +24h forecast from [Open-Meteo](https://open-meteo.com) (free, no key).
- **GitHub activity** — the `GHD` row shows today's authored commit count and
  the most recent commit time (or its date when it was before today).
- **AI quota rows** — switch the 7-day row between Claude (orange) and ChatGPT
  Codex (green) with the watch select button; MiniMax stays visible in blue:
  - used percentage + a progress bar
  - time until the window resets
- **Stale detection** — percentages turn **grey** when the last sync is over
  20 minutes old.
- **Connectivity status** — **red dot** = phone out of range, **amber** = refresh in flight.
- **Persistent** — all values survive a reboot; countdowns recompute locally every minute.

```
        21:01                  ← time
Fri 08-14           59m        ← date | countdown to 22:00
 NOW     +6H     +24H
 21°     19°      -8°
Clear   Rain    Cloud
GHD       12    14:05           ← GitHub commits today | latest commit
CL 7D     3%    6d23           ← Claude weekly window
MM 5H     1%    2h38           ← MiniMax 5-hour window
MM 7D    57%    2d2h           ← MiniMax weekly window
```

## 📖 Table of Contents

- [How it stays fresh](#keeping-it-live)
- [Data flow](#data-flow)
- [Configuration](#configuration)
- [Build & install](#build--install)
- [Troubleshooting](#troubleshooting)
- [License](#license)

## 🔄 Keeping it live

The watch only talks to the phone, so freshness is driven from both ends:

| Trigger | Effect |
|---|---|
| Wrist flick (tap) | Watch asks the phone to fetch immediately |
| Bluetooth reconnect | Same, automatically |
| Data older than 20 min | Watch asks on the next minute tick |
| Phone timer | Quota every 5 min, weather every 30 min |

Watch-initiated fetches are throttled to one per 20 s on the phone side.

## 🔀 Data flow

```
phone (pkjs)  →  watch
   ├── api.github.com/search/commits         (today's commits)
   ├── chatgpt.com/backend-api/wham/usage  (Codex, phone device login)
   ├── api.minimaxi.com/v1/token_plan/remains
   └── api.open-meteo.com                  (weather)
```

The phone fetches GitHub, Codex, MiniMax, and weather itself, so the watchface
works away from this machine, including on cellular data. Claude is a locally
rolling countdown calibrated in settings.

**GitHub** — the companion searches every GitHub repository visible to the
request for commits authored by `github_username` on the phone's local date.
Public commits require no token. Set an optional read-only `github_token` to
include private repositories that token can access. Local commits are invisible
until they are pushed to GitHub.

### Data sources

**Claude** — enter the time remaining for each window in settings. The watch
derives elapsed percentage locally and advances expired windows without a
network request.

**ChatGPT Codex** — the phone companion owns the complete device-code flow.
Choose **Connect Codex**, then manually open the displayed address in the
phone's normal browser and enter the one-time code. The companion keeps polling
after its page is closed, stores and refreshes its own OAuth token, and reads
quota from `chatgpt.com/backend-api/wham/usage` over Wi-Fi or cellular.
Because this is an internal endpoint, manual calibration remains available.

**MiniMax** — `GET https://api.minimaxi.com/v1/token_plan/remains` with
`Authorization: Bearer <coding plan key>`. Note the host: `api.minimaxi.com`
serves mainland accounts, `api.minimax.io` serves international ones. The
`general` model entry is read (`minimax_model` to change it) and the percentage
is inverted — MiniMax reports what's **left**, the watch shows what's **used**.

| MiniMax field | Watchface row |
|---|---|
| `current_interval_remaining_percent`, `end_time` | MM 5H |
| `current_weekly_remaining_percent`, `weekly_end_time` | MM 7D |

### Credentials on the phone

The built `.pbw` contains the MiniMax key. Codex credentials are not baked into
the package: device login stores them in Pebble companion `localStorage` on the
phone. A Codex refresh token is still a long-lived account credential; use
**Disconnect Codex** in settings before transferring or disposing of the phone.

## ⚙️ Configuration

`config.json` in the project root holds the secrets:

```json
{ "minimax_key": "sk-cp-...", "github_username": "LinBlink" }
```

- `minimax_key` — required for the MiniMax rows; without it they stay blank.
- Optional: `minimax_url`, `minimax_model`.
- Optional: `github_username` (defaults to `LinBlink`) and `github_token`.

> 🔒 **This file contains live credentials. It is gitignored.**

### Running the collector

```sh
# Option A — serve locally, expose with a tunnel (cloudflared already installed)
python tools/quota_collector.py serve --port 8787
cloudflared tunnel --url http://127.0.0.1:8787

# Option B — push to a secret gist; survives this machine sleeping
export GITHUB_TOKEN=...
python tools/quota_collector.py gist --gist-id <id>

# One-shot check
python tools/quota_collector.py once --dump
```

Option A needs this machine awake and reachable; option B keeps serving the last
known values while it sleeps. Option B has no `/config` route, so settings live
entirely in `DEFAULTS`.

### Endpoint contract

Whatever serves the JSON must return:

```json
{
  "claude":  { "five_hour": { "used_pct": 5, "reset_at": "2026-08-14T18:09:59Z" },
               "weekly":    { "used_pct": 2, "reset_at": "2026-08-21T12:59:59Z" } },
  "minimax": { "five_hour": { ... }, "weekly": { ... } }
}
```

- `reset_at` — ISO-8601, or epoch seconds/milliseconds.
- `used_pct` — 0-100. `{"used": 1234, "limit": 5000}` also works.
- Missing providers or windows are skipped; the watch keeps its last value.
- A flat shape (`claude_5h_pct` / `claude_5h_reset`, …) is also accepted.

## 🛠️ Build & install

Requires the Pebble SDK (this project builds from WSL):

```sh
python tools/make_secrets.py   # renders src/pkjs/index.js from the template
pebble build
pebble install --emulator basalt
```

`make_secrets.py` reads `config.json` and inlines the MiniMax settings into
`src/pkjs/pebble-js-app.js`, rendered from
`src/pkjs-template/index.js`. **Edit the template** — the generated file is
overwritten on every run and is gitignored because it holds live tokens.

### Settings

Everything is baked in at build time, so the watchface works with **no settings
page at all**. The gear icon opens one for later tweaks — theme (light/dark),
clock font (Bitham / Consolas-like digits), refresh interval, °C/°F, fixed
lat/lon, Claude/Codex display selection, Codex device login, and manual quota
calibration. Blank calibration fields preserve values already stored on the
watch when automatic Codex sync is unavailable.

That page is served as a `data:` URL (with direct fetching there is no collector
hosting it). Some phone apps refuse to navigate to `data:` URLs; if the gear
icon appears dead, edit `DEFAULTS` at the top of `src/pkjs/index.js` instead.

Weather uses phone GPS, falling back to configured lat/lon when the phone denies
location (common) or times out. Set lat/lon if the weather row stays at `--`.

## 🔍 Troubleshooting

**JS filename is load-bearing** — this project does not set `"enableMultiJS"`,
so the SDK only runs a JS entry point that contains the literal string
`pebble-js-app.js`. Name it anything else and nothing ever runs. The only
warning you get is one line in the build output:

```
WARNING: enableMultiJS is not enabled for this project and pebble-js-app.js does not exist
```

**Checking it end to end** — the emulator runs pkjs on this machine. Start the
log stream *before* installing:

```sh
pebble logs --emulator basalt &
pebble install --emulator basalt
pebble emu-app-config --emulator basalt
```

A healthy run logs `pkjs boot: claude=yes minimax=yes`, then one `sent:` line
per provider and one for weather. To check the layout without a phone, add
`#define DEMO_DATA 1` at the top of `src/c/main.c`.

## 📂 Project layout

```
src/c/main.c            watchface (C, Pebble SDK)
src/pkjs-template/      phone-side JS template (edit this)
tools/make_secrets.py   inlines credentials → src/pkjs/pebble-js-app.js
tools/quota_collector.py  optional PC-side quota collector
resources/images/       images (menu icon)
```

## 📄 License

MIT — see [LICENSE](LICENSE).
