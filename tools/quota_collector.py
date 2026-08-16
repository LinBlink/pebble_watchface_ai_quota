#!/usr/bin/env python3
"""Collect Claude and MiniMax quota state and publish it for the watchface.

The watch has no network of its own and the phone must not hold the Claude
OAuth token (refreshing it there would rotate the token out from under Claude
Code on this machine). So this runs here, where the credentials already live,
and publishes only percentages and reset timestamps.

Two publish modes:

    serve   Serve the JSON on 127.0.0.1:<port>. Expose it with
            `cloudflared tunnel --url http://127.0.0.1:<port>` and point the
            watchface settings at the URL cloudflared prints.

    gist    PATCH the JSON into a secret GitHub gist. The phone reads the gist's
            raw URL, so it keeps working while this machine is asleep (the
            watch greys out values older than 20 minutes).

Usage:
    python quota_collector.py serve --port 8787
    python quota_collector.py gist --gist-id <id>      # needs GITHUB_TOKEN
    python quota_collector.py once --dump              # one fetch, print raw

MiniMax needs MINIMAX_API_KEY in the environment; without it the MiniMax rows
stay blank and the Claude rows still work.
"""

import argparse
import json
import os
import re
import sys
import time
import urllib.error
import urllib.request
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path
from threading import Lock, Thread

CLAUDE_CREDENTIALS = Path.home() / ".claude" / ".credentials.json"
CLAUDE_USAGE_URL = "https://api.anthropic.com/api/oauth/usage"
# api.minimaxi.com serves mainland accounts; international keys use
# api.minimax.io. Override with "minimax_url" in config.json.
MINIMAX_USAGE_URL = "https://api.minimaxi.com/v1/token_plan/remains"

CONFIG_PATH = Path(__file__).resolve().parent.parent / "config.json"

REFRESH_SECONDS = 300  # matches the phone's poll interval
HTTP_TIMEOUT = 20


def log(msg):
    print(f"[{datetime.now().strftime('%H:%M:%S')}] {msg}", flush=True)


def load_config():
    """config.json holds minimax_key and (optionally) claude_cookie. Pasted
    secrets sometimes carry a stray newline, which is illegal inside a JSON
    string — strip control characters rather than failing to start."""
    if not CONFIG_PATH.exists():
        return {}
    text = CONFIG_PATH.read_text(encoding="utf-8")
    try:
        return json.loads(text)
    except json.JSONDecodeError:
        cleaned = re.sub(r"[\x00-\x1f]+", "", text)
        try:
            config = json.loads(cleaned)
        except json.JSONDecodeError as e:
            raise RuntimeError(f"{CONFIG_PATH} is not valid JSON: {e}")
        log(f"note: {CONFIG_PATH.name} contained raw control characters; ignoring them")
        return config


def get_json(url, headers):
    req = urllib.request.Request(url, headers=headers)
    with urllib.request.urlopen(req, timeout=HTTP_TIMEOUT) as resp:
        return json.loads(resp.read().decode("utf-8"))


# ------------------------------------------------------------------- Claude


def claude_access_token():
    """Read the token Claude Code maintains. We never refresh it ourselves:
    the refresh token rotates on use, so refreshing here would invalidate the
    copy Claude Code holds. It gets renewed whenever Claude Code next runs."""
    try:
        data = json.loads(CLAUDE_CREDENTIALS.read_text(encoding="utf-8"))
    except FileNotFoundError:
        raise RuntimeError(f"no credentials at {CLAUDE_CREDENTIALS} — run Claude Code once")
    oauth = data.get("claudeAiOauth") or {}
    token = oauth.get("accessToken")
    if not token:
        raise RuntimeError("credentials file has no claudeAiOauth.accessToken")

    expires_at = oauth.get("expiresAt")
    if expires_at and expires_at / 1000 < time.time():
        raise RuntimeError("access token expired — run Claude Code to refresh it")
    return token


def window(block):
    """Map one usage window onto the watchface contract."""
    if not isinstance(block, dict):
        return None
    util = block.get("utilization")
    if util is None:
        return None
    out = {"used_pct": round(float(util))}
    if block.get("resets_at"):
        out["reset_at"] = block["resets_at"]
    return out


def fetch_claude(config, dump=False):
    raw = get_json(
        CLAUDE_USAGE_URL,
        {
            "Authorization": f"Bearer {claude_access_token()}",
            "anthropic-beta": "oauth-2025-04-20",
        },
    )
    if dump:
        log("claude raw: " + json.dumps(raw, indent=2))

    out = {}
    five = window(raw.get("five_hour"))
    week = window(raw.get("seven_day"))
    if five:
        out["five_hour"] = five
    if week:
        out["weekly"] = week
    return out


# ------------------------------------------------------------------ MiniMax

# MiniMax reports quota the other way round from Claude: a *remaining* percent
# per model, with window boundaries as epoch milliseconds.
def minimax_window(entry, pct_key, end_key):
    remaining = entry.get(pct_key)
    if not isinstance(remaining, (int, float)):
        return None
    out = {"used_pct": round(100 - float(remaining))}
    end = entry.get(end_key)
    if isinstance(end, (int, float)) and end > 0:
        out["reset_at"] = int(end)  # milliseconds; the phone normalises these
    return out


def fetch_minimax(config, dump=False):
    key = config.get("minimax_key") or os.environ.get("MINIMAX_API_KEY")
    if not key:
        return {}

    raw = get_json(
        config.get("minimax_url", MINIMAX_USAGE_URL),
        {"Authorization": f"Bearer {key}", "Content-Type": "application/json"},
    )
    if dump:
        log("minimax raw: " + json.dumps(raw, indent=2, ensure_ascii=False))

    status = (raw.get("base_resp") or {}).get("status_code")
    if status not in (0, None):
        raise RuntimeError(f"minimax: {(raw.get('base_resp') or {}).get('status_msg')} ({status})")

    wanted = config.get("minimax_model", "general")
    entries = raw.get("model_remains") or []
    entry = next((e for e in entries if e.get("model_name") == wanted), None)
    if entry is None:
        names = [e.get("model_name") for e in entries]
        log(f"minimax: no '{wanted}' entry (have {names}) — set minimax_model in config.json")
        return {}

    out = {}
    five = minimax_window(entry, "current_interval_remaining_percent", "end_time")
    week = minimax_window(entry, "current_weekly_remaining_percent", "weekly_end_time")
    if five:
        out["five_hour"] = five
    if week:
        out["weekly"] = week
    return out


# ---------------------------------------------------------------- collection


class Collector:
    def __init__(self, config, dump=False):
        self.config = config
        self.dump = dump
        self.lock = Lock()
        self.payload = {"generated_at": None, "claude": {}, "minimax": {}}

    def refresh(self):
        result = {"generated_at": datetime.now(timezone.utc).isoformat()}
        for name, fetch in (("claude", fetch_claude), ("minimax", fetch_minimax)):
            try:
                result[name] = fetch(self.config, self.dump)
            except urllib.error.HTTPError as e:
                # 429 on the Claude usage endpoint is common; keeping the previous
                # value beats publishing an empty object the watch would blank out.
                log(f"{name}: HTTP {e.code} — keeping previous value")
                with self.lock:
                    result[name] = self.payload.get(name, {})
            except Exception as e:
                log(f"{name}: {e}")
                with self.lock:
                    result[name] = self.payload.get(name, {})

        with self.lock:
            self.payload = result
        log("updated: " + json.dumps({k: v for k, v in result.items() if k != "generated_at"}))
        return result

    def snapshot(self):
        with self.lock:
            return json.dumps(self.payload).encode("utf-8")


# ------------------------------------------------------------------ publish


CONFIG_PAGE = """<!DOCTYPE html><html><head>
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>AI Quota Watchface</title><style>
body{font-family:-apple-system,system-ui,sans-serif;margin:16px;background:#fff;color:#111}
label{display:block;margin:14px 0 4px;font-weight:600}
input,select{width:100%;box-sizing:border-box;padding:10px;font-size:16px}
button{margin-top:22px;width:100%;padding:14px;font-size:17px;background:#ff6a00;
color:#fff;border:0;border-radius:6px}
p{color:#666;font-size:13px}
</style></head><body>
<h2>AI Quota Watchface</h2>
<p>Endpoint serving the quota JSON. Weather comes from Open-Meteo; leave lat/lon
blank to use phone GPS.</p>
<label>Endpoint URL</label><input id="url" type="url">
<label>Bearer token (optional)</label><input id="token" type="text">
<label>Quota refresh (minutes)</label><input id="refresh" type="number" min="1">
<label>Units</label><select id="units">
<option value="celsius">Celsius</option><option value="fahrenheit">Fahrenheit</option>
</select>
<label>Theme</label><select id="theme">
<option value="light">Light</option><option value="dark">Dark</option>
</select>
<label>Clock font</label><select id="timeFont">
<option value="bitham">Bitham</option><option value="consolas">Consolas</option>
</select>
<label>Latitude (optional)</label><input id="lat" type="text">
<label>Longitude (optional)</label><input id="lon" type="text">
<button id="save">Save</button>
<script>
var cur = {};
try {
  var q = /[?&]current=([^&]*)/.exec(location.search);
  if (q) cur = JSON.parse(decodeURIComponent(q[1]));
} catch (e) {}
var g = function (id) { return document.getElementById(id); };
g('url').value = cur.url || '';
g('token').value = cur.token || '';
g('refresh').value = cur.refreshMin || 5;
g('units').value = cur.units || 'celsius';
g('theme').value = cur.theme || 'light';
g('timeFont').value = cur.timeFont || 'bitham';
g('lat').value = cur.lat || '';
g('lon').value = cur.lon || '';
g('save').onclick = function () {
  var out = {
    url: g('url').value.trim(), token: g('token').value.trim(),
    refreshMin: parseInt(g('refresh').value, 10) || 5,
    units: g('units').value, lat: g('lat').value.trim(), lon: g('lon').value.trim(),
    theme: g('theme').value, timeFont: g('timeFont').value
  };
  location.href = 'pebblejs://close#' + encodeURIComponent(JSON.stringify(out));
};
</script></body></html>"""


def run_serve(collector, port):
    class Handler(BaseHTTPRequestHandler):
        def _send(self, body, content_type):
            self.send_response(200)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            # /config is the watchface's settings page: phone apps refuse to open
            # `data:` URLs, so it has to be served from a real origin.
            if self.path.split("?")[0].rstrip("/") == "/config":
                self._send(CONFIG_PAGE.encode("utf-8"), "text/html; charset=utf-8")
            else:
                self._send(collector.snapshot(), "application/json")

        def log_message(self, *args):
            pass  # the collector's own log is the interesting one

    def loop():
        while True:
            collector.refresh()
            time.sleep(REFRESH_SECONDS)

    Thread(target=loop, daemon=True).start()
    log(f"serving quota JSON on http://127.0.0.1:{port}/  (settings page at /config)")
    log(f"expose it with:  cloudflared tunnel --url http://127.0.0.1:{port}")
    HTTPServer(("127.0.0.1", port), Handler).serve_forever()


def run_gist(collector, gist_id, filename):
    token = os.environ.get("GITHUB_TOKEN")
    if not token:
        sys.exit("gist mode needs GITHUB_TOKEN in the environment")

    while True:
        collector.refresh()
        body = json.dumps(
            {"files": {filename: {"content": collector.snapshot().decode("utf-8")}}}
        ).encode("utf-8")
        req = urllib.request.Request(
            f"https://api.github.com/gists/{gist_id}",
            data=body,
            method="PATCH",
            headers={
                "Authorization": f"Bearer {token}",
                "Accept": "application/vnd.github+json",
                "Content-Type": "application/json",
            },
        )
        try:
            with urllib.request.urlopen(req, timeout=HTTP_TIMEOUT) as resp:
                info = json.loads(resp.read().decode("utf-8"))
                raw_url = info["files"][filename]["raw_url"]
            log(f"pushed to gist — point the watchface at: {raw_url}?t=<timestamp>")
        except Exception as e:
            log(f"gist push failed: {e}")
        time.sleep(REFRESH_SECONDS)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("mode", choices=("serve", "gist", "once"))
    parser.add_argument("--port", type=int, default=8787)
    parser.add_argument("--gist-id")
    parser.add_argument("--filename", default="quota.json")
    parser.add_argument("--dump", action="store_true", help="print raw upstream responses")
    args = parser.parse_args()

    collector = Collector(load_config(), dump=args.dump)

    if args.mode == "once":
        print(json.dumps(collector.refresh(), indent=2))
    elif args.mode == "serve":
        run_serve(collector, args.port)
    else:
        if not args.gist_id:
            sys.exit("gist mode needs --gist-id")
        run_gist(collector, args.gist_id, args.filename)


if __name__ == "__main__":
    main()
