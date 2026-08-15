# AI Quota Watchface

Pebble Time (basalt) watchface showing time/date, a countdown to 22:00, the
weather at three horizons, and Claude / MiniMax usage quotas.

```
        21:01                 ← BITHAM_42_BOLD
Fri 08-14           59m       ← date | time left until 22:00 (yellow)
 NOW     +6H     +24H
 21°     19°      -8°
Clear   Rain    Cloud
CL 5H     8%    4h48         ← Claude 5-hour window
CL 7D     3%    6d23         ← Claude weekly window
MM 5H     1%    2h38         ← MiniMax 5-hour window
MM 7D    57%    2d2h         ← MiniMax weekly window
```

- Quota rows show used percentage, time until the window resets, and a progress
  bar. Claude rows are orange, MiniMax rows blue; ≥90% turns the number red.
- Percentages go **grey** when the last sync is over 20 minutes old — the number
  is still shown, but flagged as possibly moved.
- A **red dot** top-right means the phone is out of range; **amber** means a
  refresh is in flight.
- Everything received is persisted, so values show immediately after a restart;
  the countdowns are recomputed locally each minute.

## Keeping it live

The watch talks only to the phone, so freshness is driven from both ends:

| Trigger | Effect |
|---|---|
| Wrist flick (tap) | Watch asks the phone to fetch immediately |
| Bluetooth reconnect | Same, automatically |
| Data older than 20 min | Watch asks on the next minute tick |
| Phone timer | Quota every 5 min, weather every 30 min |

Watch-initiated fetches are throttled to one per 20 s on the phone side.

## Data flow

```
phone (pkjs)  →  watch
   ├── api.anthropic.com/api/oauth/usage   (Claude, OAuth token refreshed on the phone)
   ├── api.minimaxi.com/v1/token_plan/remains
   └── api.open-meteo.com                  (weather)
```

The phone fetches everything itself, so the watchface keeps working away from
this machine. `tools/make_secrets.py` bakes the credentials into the build; see
[Credentials on the phone](#credentials-on-the-phone) for what that costs you.

The PC collector (`tools/quota_collector.py`) is still supported and still
takes precedence — set `DEFAULTS.url` and the phone reads that instead, keeping
the tokens off the phone entirely. Then the flow is the older one:

```
PC (tools/quota_collector.py)  →  relay (tunnel or gist)  →  phone (pkjs)  →  watch
```

### Credentials on the phone

Direct fetching means the build carries a live Claude **refresh token** and the
MiniMax API key, and the installed `.pbw` on the phone contains both. Two
consequences worth being deliberate about:

- **Claude's refresh token rotates on use.** The first time the phone refreshes,
  the copy in `~/.claude/.credentials.json` is dead and Claude Code on this
  machine will ask you to log in again. After that re-login, run
  `python tools/make_secrets.py` and reinstall to re-seed the phone — otherwise
  the phone holds the only working token and a wipe of the app's storage loses
  it for good.
- A refresh token is a long-lived account credential, unlike the 8-hour access
  token the collector reads. Losing the phone is a bigger deal than it was.

The phone also cannot set `User-Agent` or `Cookie` on requests — both are
forbidden XHR headers — so the refresh call goes out without the headers Claude
Code sends. If Cloudflare starts requiring them the call returns 403 and the
Claude rows go stale; the pkjs console log says which endpoint failed.

### Sources

**Claude** — `GET https://api.anthropic.com/api/oauth/usage` with the token from
`~/.claude/.credentials.json` plus `anthropic-beta: oauth-2025-04-20`. Returns
`five_hour.utilization` / `seven_day.utilization` (0–100) and `resets_at`.
This is an unofficial internal endpoint and can change without notice.

The collector deliberately does **not** refresh the token — it reads whatever
Claude Code currently holds. If you don't run Claude Code for ~8 hours the token
expires, the collector logs it, and the watch greys out until you next use
Claude Code.

**MiniMax** — `GET https://api.minimaxi.com/v1/token_plan/remains` with
`Authorization: Bearer <coding plan key>`. Note the host: `api.minimaxi.com`
serves mainland accounts, `api.minimax.io` serves international ones, and each
rejects the other's keys with `invalid api key`. Override via `minimax_url`.

The response is a `model_remains[]` array, one entry per model family. The
collector reads the `general` entry (set `minimax_model` to change it) and
inverts the percentages — MiniMax reports how much is **left**, the watchface
shows how much is **used**:

| MiniMax field | Watchface row |
|---|---|
| `current_interval_remaining_percent`, `end_time` | MM 5H |
| `current_weekly_remaining_percent`, `weekly_end_time` | MM 7D |

### Configuration

`config.json` in the project root holds the secrets:

```json
{ "minimax_key": "sk-cp-...", "claude_cookie": "..." }
```

`minimax_key` is required for the MiniMax rows; without it they stay blank.
`claude_cookie` is unused — the OAuth token from `~/.claude/.credentials.json`
is preferred, since Claude Code keeps it fresh automatically. Optional keys:
`minimax_url`, `minimax_model`. **This file contains live credentials — keep it
out of version control.**

### Running the collector

```sh
# Option A — serve locally, expose with a tunnel (cloudflared is already installed)
python tools/quota_collector.py serve --port 8787
cloudflared tunnel --url http://127.0.0.1:8787

# Option B — push to a secret gist; survives this machine sleeping
export GITHUB_TOKEN=...
python tools/quota_collector.py gist --gist-id <id>

# One-shot check
python tools/quota_collector.py once --dump
```

Option A needs this machine awake and reachable; option B keeps serving the last
known values while it sleeps (the watch greys them out once stale). Option B has
no `/config` route, so with it the settings live entirely in `DEFAULTS`.

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

## Settings

Everything is baked in at build time, so the watchface works with no settings
page at all. The gear icon opens one anyway for later tweaks — refresh interval,
°C/°F, fixed lat/lon, and the optional collector URL/token. Anything saved there
overrides `DEFAULTS`.

That page is served as a `data:` URL, since with direct fetching there is no
collector hosting it. Some phone apps refuse to navigate to `data:` URLs and
silently do nothing; if the gear icon appears dead, edit `DEFAULTS` at the top of
`src/pkjs/index.js` and rebuild instead. When a collector URL *is* configured,
its `/config` page wins — that one is a real origin and always opens.

Weather comes from [Open-Meteo](https://open-meteo.com) — free, no key. It uses
phone GPS, falling back to the configured lat/lon when the phone denies location
(common) or times out. Set lat/lon if the weather row stays at `--`.

GPS needs `"location"` in `capabilities` in `package.json`. Without it
`navigator.geolocation` exists but every call fails, which is silent unless you
read the pkjs log — the weather row simply never fills in.

## Build

Requires the Pebble SDK (this project builds from WSL):

```sh
python tools/make_secrets.py   # renders src/pkjs/index.js from the template
pebble build
pebble install --emulator basalt
```

`make_secrets.py` reads `~/.claude/.credentials.json` and `config.json` and
inlines them into `src/pkjs/pebble-js-app.js`, rendered from
`src/pkjs-template/index.js`. **Edit the template** — the generated file is
overwritten on every run and is gitignored because it holds live tokens. Re-run
the script whenever you log in to Claude Code again, then reinstall.

### The JS filename is load-bearing

This project does not set `"enableMultiJS": true`, so the SDK's `process_js` step
only treats a file as the JS entry point if its path contains the literal string
`pebble-js-app.js`. Name it anything else — `index.js`, the modern convention —
and waf copies it into the `.pbw` without complaint, but nothing ever runs it.
The phone then reports `No JS found, can't show configuration`, and the symptom
set is total: no quotas, no weather, and a gear icon that does nothing.

The only warning you get is one line in the build output, easily lost in noise:

```
WARNING: enableMultiJS is not enabled for this project and pebble-js-app.js does not exist
```

The other tell is that `basalt/manifest.json` inside the `.pbw` has no `js`
section at all. The alternative fix is enabling multiJS, which pulls in a webpack
dependency; for a single-file pkjs that isn't worth it.

### Checking it end to end

The emulator runs pkjs on this machine, so it verifies everything except real
phone GPS. Start the log stream *before* installing, or the boot line and first
fetch scroll past unseen:

```sh
pebble logs --emulator basalt &
pebble install --emulator basalt
pebble emu-app-config --emulator basalt   # what the gear icon does
```

A healthy run logs `pkjs boot: claude=yes minimax=yes`, then one `sent:` line per
provider and one for weather.

To check the layout without a phone, add `#define DEMO_DATA 1` at the top of
`src/c/main.c` to preload sample quotas and weather.
