/*
 * Phone-side companion. The watch has no network of its own, so this fetches
 * everything and pushes it over AppMessage:
 *
 *   - Today's public GitHub commit count, fetched directly from GitHub.
 *   - Claude's 5h/7d usage windows are NOT fetched live any more — every live
 *     approach (direct OAuth refresh, the PC collector) kept going stale
 *     because Claude's refresh token rotates on use and collides with
 *     whichever other holder (Claude Code CLI, this phone) refreshed it last.
 *     Instead the watch just counts down two windows locally: you tell it how
 *     much time is left in each (settings page), it stores the resulting
 *     reset time, and the firmware itself rolls the countdown to the next
 *     window once one expires (see advance_claude_reset in main.c). No token,
 *     no network call, nothing to go stale.
 *   - ChatGPT Codex quota from the phone's own device-code login. Access tokens
 *     are refreshed on the phone; manual calibration remains as a fallback.
 *   - MiniMax quota, straight from api.minimaxi.com with the coding-plan key.
 *   - Weather for now / +6h / +24h, from Open-Meteo (free, no API key).
 */

// Substituted by tools/make_secrets.py, which renders this template into
// src/pkjs/index.js. It is inlined rather than `require`d from a second file
// because require() needs enableMultiJS, which needs webpack — one build
// dependency more than this is worth.
var SECRETS = /*__SECRETS__*/{};

// First line of output in `pebble logs`. If you don't see it, the JS never
// loaded at all — which looks identical to "the gear icon does nothing", since
// an unloaded pkjs registers no showConfiguration listener either.
console.log('pkjs boot: codex=' + (localStorage.getItem('codex_oauth') ? 'yes' : 'NO') +
            ' minimax=' + (SECRETS.minimaxKey ? 'yes' : 'NO') +
            ' github=' + (SECRETS.githubUsername || 'LinBlink'));

// ---------------------------------------------------------------- defaults
//
// Everything the watchface needs is baked in here, so it works with no settings
// page at all. Anything saved through the settings page overrides these.
var DEFAULTS = {
  refreshMin: 5,
  units: 'celsius',   // or 'fahrenheit'
  lat: '',            // blank = use phone GPS
  lon: '',
  targetDate: '',     // 'YYYY-MM-DD' for the day counter; blank hides it
  theme: 'light',     // or 'dark'
  timeFont: 'bitham', // or 'consolas' — Consolas-like digits for the clock
  aiProvider: 'claude', // select button toggles the Claude / Codex 7D row
  githubUsername: SECRETS.githubUsername || 'LinBlink',
  githubToken: SECRETS.githubToken || ''
};

var DEFAULT_QUOTA_REFRESH_MIN = 5;
var WEATHER_REFRESH_MIN = 30;
// Ignore watch-initiated refreshes that arrive faster than this, so a shaky
// wrist can't hammer the endpoints.
var MIN_REFRESH_GAP_MS = 20 * 1000;

var MINIMAX_USAGE_URL = 'https://api.minimaxi.com/v1/token_plan/remains';
// Codex's device login and personal quota endpoints are used by the Codex
// client but are not part of the documented public OpenAI API. Keep every URL
// in one place so a server-side change has a small, obvious repair surface.
var CODEX_CLIENT_ID = 'app_EMoamEEZ73f0CkXaXp7hrann';
var CODEX_DEVICE_CODE_URL = 'https://auth.openai.com/api/accounts/deviceauth/usercode';
var CODEX_DEVICE_POLL_URL = 'https://auth.openai.com/api/accounts/deviceauth/token';
var CODEX_DEVICE_LOGIN_URL = 'https://auth.openai.com/codex/device';
var CODEX_TOKEN_URL = 'https://auth.openai.com/oauth/token';
var CODEX_USAGE_URL = 'https://chatgpt.com/backend-api/wham/usage';
var CODEX_TOKEN_EARLY_REFRESH_MS = 5 * 60 * 1000;
var CODEX_BACKOFF_MS = 15 * 60 * 1000;

var lastQuotaFetch = 0;
var codexBackoffUntil = 0;
var codexRefreshInFlight = false;
var codexRefreshWaiters = [];
var codexLoginTimer = null;

function getSettings() {
  var raw = localStorage.getItem('settings');
  var s = {};
  if (raw) {
    try { s = JSON.parse(raw); } catch (e) { s = {}; }
  }
  var units = s.units || DEFAULTS.units;
  var theme = s.theme || DEFAULTS.theme;
  var timeFont = s.timeFont || DEFAULTS.timeFont;
  return {
    refreshMin: s.refreshMin || DEFAULTS.refreshMin || DEFAULT_QUOTA_REFRESH_MIN,
    units: units === 'fahrenheit' ? 'fahrenheit' : 'celsius',
    lat: s.lat || DEFAULTS.lat,
    lon: s.lon || DEFAULTS.lon,
    targetDate: s.targetDate || DEFAULTS.targetDate,
    theme: theme === 'dark' ? 'dark' : 'light',
    timeFont: timeFont === 'consolas' ? 'consolas' : 'bitham',
    aiProvider: s.aiProvider === 'codex' ? 'codex' : 'claude',
    githubUsername: s.githubUsername || DEFAULTS.githubUsername,
    githubToken: s.githubToken || DEFAULTS.githubToken
  };
}

// The watch counts the days down itself, so it only needs the target once —
// as local midnight, which is the boundary it ticks over on. Sending 0 clears
// it, otherwise clearing the field in settings would leave the old date stuck.
function sendTargetDate() {
  var raw = getSettings().targetDate;
  var m = /^(\d{4})-(\d{2})-(\d{2})$/.exec(raw || '');
  if (!m) {
    send({ TARGET_DATE: 0 });
    return;
  }
  var midnight = new Date(Number(m[1]), Number(m[2]) - 1, Number(m[3]));
  send({ TARGET_DATE: Math.floor(midnight.getTime() / 1000) });
}

// Appearance lives on the watch, so the watch needs it even when no data has
// been pushed yet — send it on every boot and after every settings save.
function sendAppearance() {
  var cfg = getSettings();
  send({
    THEME: cfg.theme === 'dark' ? 1 : 0,
    TIME_FONT: cfg.timeFont === 'consolas' ? 1 : 0,
    AI_PROVIDER: cfg.aiProvider === 'codex' ? 1 : 0
  });
}

function saveSettings(s) {
  localStorage.setItem('settings', JSON.stringify(s));
}

function send(msg) {
  if (Object.keys(msg).length === 0) return;
  Pebble.sendAppMessage(msg,
    function () { console.log('sent: ' + JSON.stringify(msg)); },
    function (e) { console.log('send failed: ' + JSON.stringify(e)); });
}

function request(method, url, headers, body, onOk, onFail) {
  var req = new XMLHttpRequest();
  req.open(method, url, true);
  req.timeout = 20000;
  Object.keys(headers || {}).forEach(function (k) { req.setRequestHeader(k, headers[k]); });
  var fail = onFail || function () {};
  req.onload = function () {
    if (req.status < 200 || req.status >= 300) {
      console.log(url.split('?')[0] + ' -> http ' + req.status +
                  (req.responseText ? ' ' + req.responseText.substring(0, 160) : ''));
      var errorBody = {};
      try { errorBody = JSON.parse(req.responseText || '{}'); } catch (e) {}
      fail(req.status, errorBody);
      return;
    }
    var parsed;
    try {
      parsed = JSON.parse(req.responseText);
    } catch (e) {
      console.log('parse error from ' + url.split('?')[0] + ': ' + e);
      fail(0);
      return;
    }
    onOk(parsed);
  };
  req.onerror = function () { console.log('network error: ' + url.split('?')[0]); fail(0); };
  req.ontimeout = function () { console.log('timeout: ' + url.split('?')[0]); fail(0); };
  req.send(body);
}

function getJSON(url, headers, onData, onFail) {
  request('GET', url, headers, null, onData, onFail);
}

// ------------------------------------------------------------- codex tokens

function loadCodexTokens() {
  var raw = localStorage.getItem('codex_oauth');
  if (!raw) return {};
  try { return JSON.parse(raw); } catch (e) { return {}; }
}

function saveCodexTokens(tokens) {
  if (!tokens || !tokens.accessToken) return;
  localStorage.setItem('codex_oauth', JSON.stringify(tokens));
}

function loadCodexLogin() {
  var raw = localStorage.getItem('codex_device_login');
  if (!raw) return null;
  try { return JSON.parse(raw); } catch (e) { return null; }
}

function clearCodexLogin() {
  if (codexLoginTimer) clearTimeout(codexLoginTimer);
  codexLoginTimer = null;
  localStorage.removeItem('codex_device_login');
}

// The authorization page never handles or returns credentials. It only shows
// the one-time code; the companion keeps polling in the background and stores
// the resulting token directly, even if this page is closed.
function codexAuthorizationPage(login) {
  return '<!DOCTYPE html><html><head><meta charset="utf-8">' +
    '<meta name="viewport" content="width=device-width,initial-scale=1">' +
    '<title>Connect Codex</title><style>' +
    'body{font:16px -apple-system,Roboto,sans-serif;padding:24px;background:#111;color:#eee}' +
    '.code{font:bold 30px monospace;letter-spacing:4px;color:#7ddc7d;text-align:center;' +
    'padding:18px;border:1px solid #444;border-radius:8px;margin:18px 0}' +
    'p{color:#bbb;line-height:1.5}b{color:#fff}</style></head><body>' +
    '<h2>Connect ChatGPT Codex</h2><p>In your normal phone browser, open:</p>' +
    '<p><b>' + CODEX_DEVICE_LOGIN_URL + '</b></p>' +
    '<p>Enter this one-time code:</p><div class="code">' + login.user_code + '</div>' +
    '<p>You may close this page immediately. AI Quota continues checking in the ' +
    'phone companion and will refresh the watch automatically after authorization.</p>' +
    '</body></html>';
}

function showCodexAuthorization(login) {
  var url = 'data:text/html;charset=utf-8,' +
            encodeURIComponent(codexAuthorizationPage(login));
  Pebble.openURL(url);
}

function finishCodexLogin(raw) {
  request('POST', CODEX_TOKEN_URL, { 'Content-Type': 'application/json' },
    JSON.stringify({
      client_id: CODEX_CLIENT_ID,
      grant_type: 'authorization_code',
      code: raw.authorization_code,
      redirect_uri: 'https://auth.openai.com/deviceauth/callback',
      code_verifier: raw.code_verifier
    }),
    function (tokens) {
      if (!tokens.access_token || !tokens.refresh_token) {
        console.log('codex login: token exchange returned incomplete credentials');
        return;
      }
      saveCodexTokens({
        accessToken: tokens.access_token,
        refreshToken: tokens.refresh_token,
        accountId: tokens.account_id || '',
        expiresAt: Date.now() + Math.max(60, Number(tokens.expires_in) || 3600) * 1000
      });
      clearCodexLogin();
      console.log('codex login: connected on phone');
      refreshQuotas(true);
    },
    function (status) { console.log('codex login: token exchange failed with status ' + status); });
}

function pollCodexLogin(login) {
  if (!login || Date.now() > login.deadline) {
    console.log('codex login: device code expired');
    clearCodexLogin();
    return;
  }
  request('POST', CODEX_DEVICE_POLL_URL, { 'Content-Type': 'application/json' },
    JSON.stringify({ device_auth_id: login.device_auth_id, user_code: login.user_code }),
    function (raw) {
      if (raw.authorization_code) finishCodexLogin(raw);
    },
    function (status, raw) {
      var code = raw && raw.error && raw.error.code;
      if ((status === 403 && code === 'deviceauth_authorization_pending') || status === 0) {
        codexLoginTimer = setTimeout(function () { pollCodexLogin(login); },
          Math.max(1, Number(login.interval) || 5) * 1000);
        return;
      }
      console.log('codex login: polling failed with status ' + status);
      clearCodexLogin();
    });
}

function startCodexLogin() {
  clearCodexLogin();
  request('POST', CODEX_DEVICE_CODE_URL, { 'Content-Type': 'application/json' },
    JSON.stringify({ client_id: CODEX_CLIENT_ID }),
    function (raw) {
      if (!raw.device_auth_id || !raw.user_code) {
        console.log('codex login: device-code response incomplete');
        return;
      }
      var login = {
        device_auth_id: raw.device_auth_id,
        user_code: raw.user_code,
        interval: Math.max(1, Number(raw.interval) || 5),
        deadline: Date.now() + Math.max(60, Number(raw.expires_in) || 600) * 1000
      };
      localStorage.setItem('codex_device_login', JSON.stringify(login));
      showCodexAuthorization(login);
      pollCodexLogin(login);
    },
    function (status) { console.log('codex login: could not get device code (' + status + ')'); });
}

function finishCodexRefresh(tokens) {
  codexRefreshInFlight = false;
  var waiters = codexRefreshWaiters;
  codexRefreshWaiters = [];
  for (var i = 0; i < waiters.length; i++) waiters[i](tokens);
}

// Refresh tokens can rotate. Preserve the old one only when the response does
// not include a replacement, and persist the complete new set before allowing
// a waiting quota request to continue.
function refreshCodexToken(onDone) {
  codexRefreshWaiters.push(onDone);
  if (codexRefreshInFlight) return;
  codexRefreshInFlight = true;

  var current = loadCodexTokens();
  if (!current.refreshToken) {
    console.log('codex: not connected — use the settings page to sign in');
    finishCodexRefresh(null);
    return;
  }

  request('POST', CODEX_TOKEN_URL, { 'Content-Type': 'application/json' },
    JSON.stringify({
      client_id: CODEX_CLIENT_ID,
      grant_type: 'refresh_token',
      refresh_token: current.refreshToken
    }),
    function (raw) {
      var next = {
        accessToken: raw.access_token || '',
        refreshToken: raw.refresh_token || current.refreshToken,
        accountId: raw.account_id || current.accountId || '',
        expiresAt: Date.now() + Math.max(60, Number(raw.expires_in) || 3600) * 1000
      };
      if (!next.accessToken) {
        console.log('codex: refresh response has no access token');
        finishCodexRefresh(null);
        return;
      }
      saveCodexTokens(next);
      console.log('codex: token refreshed');
      finishCodexRefresh(next);
    },
    function (status) {
      console.log('codex: token refresh failed with status ' + status);
      finishCodexRefresh(null);
    });
}

function withCodexToken(onDone) {
  var tokens = loadCodexTokens();
  if (!tokens.accessToken) {
    onDone(null);
    return;
  }
  if (!tokens.expiresAt || Date.now() + CODEX_TOKEN_EARLY_REFRESH_MS < tokens.expiresAt) {
    onDone(tokens);
    return;
  }
  refreshCodexToken(onDone);
}

// ---------------------------------------------------------- claude calibration
//
// No live fetch, no token, nothing to rotate or go stale. The settings page
// takes "how much time is left in the window right now" and this turns that
// into an absolute reset time to push to the watch; the firmware ticks it
// down and rolls it over to the next window on its own (advance_claude_reset
// in main.c) without ever needing to hear from the phone again.

// Accepts "Ud" / "Vh" / "Wm" in any combination (e.g. "2h30m", "4d12h"),
// "H:MM" (colon form — hours:minutes), or a bare number using bareUnitSec for
// its unit (60 for the 5h field's plain-minutes shorthand, 3600 for the 7d
// field's plain-hours shorthand). Returns seconds, or null if unparseable.
function parseDuration(str, bareUnitSec) {
  str = (str || '').trim().toLowerCase();
  if (!str) return null;
  var colon = /^(\d+):(\d{1,2})$/.exec(str);
  if (colon) return Number(colon[1]) * 3600 + Number(colon[2]) * 60;
  var total = 0, matched = false;
  var re = /(\d+)\s*(d|h|m)/g;
  var m;
  while ((m = re.exec(str)) !== null) {
    matched = true;
    var n = Number(m[1]);
    if (m[2] === 'd') total += n * 86400;
    else if (m[2] === 'h') total += n * 3600;
    else total += n * 60;
  }
  if (matched) return total;
  var bare = Number(str);
  return isNaN(bare) ? null : bare * bareUnitSec;
}

// Turns "time left in the window, as of right now" into the AppMessage keys
// the firmware wants (absolute epoch seconds). Blank/unparseable input means
// "leave whatever the watch already has alone", so this only adds keys it
// actually has a value for.
function claudeCalibrationMessage(remain5h, remain7d) {
  var msg = {};
  var sec5h = parseDuration(remain5h, 60);
  if (sec5h !== null) msg.CLAUDE_5H_RESET = Math.floor(Date.now() / 1000) + sec5h;
  var sec7d = parseDuration(remain7d, 3600);
  if (sec7d !== null) msg.CLAUDE_WK_RESET = Math.floor(Date.now() / 1000) + sec7d;
  return msg;
}

// Codex's Usage panel exposes a used percentage and reset countdown, but there
// is no documented personal-plan quota API for a Pebble companion to call.
// Calibrate either window from the panel; blank fields leave saved watch data
// untouched.
function codexCalibrationMessage(pct5h, remain5h, pct7d, remain7d) {
  var msg = {};
  function add(prefix, pctText, remainText, bareUnitSec) {
    if ((pctText || '').trim() !== '') {
      var pct = Number(pctText);
      if (!isNaN(pct)) msg[prefix + '_PCT'] = Math.max(0, Math.min(100, Math.round(pct)));
    }
    var seconds = parseDuration(remainText, bareUnitSec);
    if (seconds !== null) msg[prefix + '_RESET'] = Math.floor(Date.now() / 1000) + seconds;
  }
  add('CODEX_5H', pct5h, remain5h, 60);
  add('CODEX_WK', pct7d, remain7d, 3600);
  return msg;
}

// ------------------------------------------------------------------- quotas

function toEpochSeconds(v) {
  if (v === null || v === undefined) return 0;
  if (typeof v === 'string') {
    var parsed = Date.parse(v);
    if (!isNaN(parsed)) return Math.floor(parsed / 1000);
    v = Number(v);
  }
  if (typeof v !== 'number' || isNaN(v)) return 0;
  return v > 1e11 ? Math.floor(v / 1000) : Math.floor(v);
}

function toPct(win) {
  if (!win) return null;
  if (typeof win.used_pct === 'number') return Math.round(win.used_pct);
  if (typeof win.percent === 'number') return Math.round(win.percent);
  if (typeof win.used === 'number' && typeof win.limit === 'number' && win.limit > 0) {
    return Math.round((win.used / win.limit) * 100);
  }
  return null;
}

// Turn one {used_pct, reset_at} window into its two AppMessage keys.
function windowMessage(prefix, win) {
  var msg = {};
  if (!win) return msg;
  var pct = toPct(win);
  if (pct !== null) msg[prefix + '_PCT'] = pct;
  var reset = toEpochSeconds(win.reset_at || win.resets_at || win.reset);
  if (reset) msg[prefix + '_RESET'] = reset;
  return msg;
}

function merge(into, from) {
  Object.keys(from).forEach(function (k) { into[k] = from[k]; });
  return into;
}

// One-time bank calibration. Keep these fields out of saved settings: sending
// old values again on every companion restart would overwrite newer bank data.
function bankBalanceMessage(text, balanceKey, timeKey, at) {
  text = (text || '').trim().replace(/,/g, '');
  var m = /^(\d+)(?:\.(\d{1,2}))?$/.exec(text);
  if (!m) return {};
  var whole = Number(m[1]);
  var fraction = (m[2] || '') + '00';
  var cents = whole * 100 + Number(fraction.substring(0, 2));
  if (!isFinite(cents) || cents < 0 || cents > 2147483647) return {};
  var message = {};
  message[balanceKey] = Math.round(cents);
  message[timeKey] = at;
  return message;
}

// --- chatgpt codex, direct

function codexWindowMessage(win) {
  if (!win || typeof win.used_percent !== 'number') return {};
  var seconds = Number(win.limit_window_seconds) || 0;
  // Older Codex plans expose 5h + 7d windows; current plans may expose only a
  // weekly shared-agentic window. Classify by duration and leave an absent
  // window untouched so manual fallback data remains visible.
  var prefix = seconds > 0 && seconds <= 24 * 60 * 60 ? 'CODEX_5H' : 'CODEX_WK';
  var reset = Number(win.reset_at) || 0;
  if (!reset && typeof win.reset_after_seconds === 'number') {
    reset = Math.floor(Date.now() / 1000) + win.reset_after_seconds;
  }
  return windowMessage(prefix, { used_pct: win.used_percent, reset_at: reset });
}

function sendCodexUsage(raw) {
  var rate = raw && raw.rate_limit;
  if (!rate) {
    console.log('codex: usage response has no rate_limit');
    return;
  }
  var msg = {};
  merge(msg, codexWindowMessage(rate.primary_window));
  merge(msg, codexWindowMessage(rate.secondary_window));
  if (Object.keys(msg).length) send(msg);
}

function fetchCodexWithToken(tokens, retried) {
  var headers = { Authorization: 'Bearer ' + tokens.accessToken };
  if (tokens.accountId) headers['ChatGPT-Account-Id'] = tokens.accountId;
  getJSON(CODEX_USAGE_URL, headers,
    function (raw) {
      codexBackoffUntil = 0;
      sendCodexUsage(raw);
    },
    function (status) {
      if (status === 401 && !retried) {
        // Force refresh even if the JWT's stated expiry is still in the future.
        var stale = loadCodexTokens();
        stale.expiresAt = 0;
        saveCodexTokens(stale);
        refreshCodexToken(function (fresh) {
          if (fresh) fetchCodexWithToken(fresh, true);
        });
        return;
      }
      if (status === 429) codexBackoffUntil = Date.now() + CODEX_BACKOFF_MS;
      console.log('codex: usage fetch failed with status ' + status +
                  ' — keeping the last/manual values');
    });
}

function fetchCodex() {
  if (Date.now() < codexBackoffUntil) return;
  withCodexToken(function (tokens) {
    if (tokens) fetchCodexWithToken(tokens, false);
  });
}

// --- minimax, direct

// MiniMax reports quota the other way round from Claude: a *remaining* percent
// per model, with window boundaries as epoch milliseconds.
function minimaxWindow(entry, pctKey, endKey) {
  var remaining = entry[pctKey];
  if (typeof remaining !== 'number') return null;
  var out = { used_pct: Math.round(100 - remaining) };
  if (typeof entry[endKey] === 'number' && entry[endKey] > 0) out.reset_at = entry[endKey];
  return out;
}

function fetchMiniMax() {
  var key = SECRETS.minimaxKey;
  if (!key) return;
  getJSON(SECRETS.minimaxUrl || MINIMAX_USAGE_URL,
    { Authorization: 'Bearer ' + key, 'Content-Type': 'application/json' },
    function (raw) {
      var resp = raw.base_resp || {};
      if (resp.status_code !== 0 && resp.status_code !== undefined) {
        console.log('minimax: ' + resp.status_msg + ' (' + resp.status_code + ')');
        return;
      }
      var wanted = SECRETS.minimaxModel || 'general';
      var entries = raw.model_remains || [];
      var entry = null;
      for (var i = 0; i < entries.length; i++) {
        if (entries[i].model_name === wanted) { entry = entries[i]; break; }
      }
      if (!entry) {
        console.log('minimax: no "' + wanted + '" entry — set minimax_model in config.json');
        return;
      }
      var msg = {};
      merge(msg, windowMessage('MINIMAX_5H',
        minimaxWindow(entry, 'current_interval_remaining_percent', 'end_time')));
      merge(msg, windowMessage('MINIMAX_WK',
        minimaxWindow(entry, 'current_weekly_remaining_percent', 'weekly_end_time')));
      send(msg);
    });
}

// --- github, direct

function localDateString() {
  var d = new Date();
  function pad(n) { return n < 10 ? '0' + n : String(n); }
  return d.getFullYear() + '-' + pad(d.getMonth() + 1) + '-' + pad(d.getDate());
}

// Commit Search gives an actual commit count rather than counting push events.
// Without a token this covers public commits; an optional token also lets the
// query see repositories that credential can access.
function githubHeaders() {
  var headers = { Accept: 'application/vnd.github+json' };
  var token = getSettings().githubToken;
  if (token) headers.Authorization = 'Bearer ' + token;
  return headers;
}

function sendLatestGitHubCommit(item) {
  var raw = item && item.commit && item.commit.author && item.commit.author.date;
  var millis = Date.parse(raw || '');
  if (!isNaN(millis)) {
    send({ GITHUB_LATEST_COMMIT_AT: Math.floor(millis / 1000) });
  }
}

function fetchLatestGitHubCommit(username) {
  var query = 'author:' + username;
  var url = 'https://api.github.com/search/commits?q=' + encodeURIComponent(query) +
            '&per_page=1&sort=author-date&order=desc';
  getJSON(url, githubHeaders(), function (raw) {
    if (raw.items && raw.items.length) sendLatestGitHubCommit(raw.items[0]);
  });
}

function fetchGitHubCommits() {
  var username = (getSettings().githubUsername || '').trim();
  if (!username) return;
  var query = 'author:' + username + ' author-date:' + localDateString();
  var url = 'https://api.github.com/search/commits?q=' + encodeURIComponent(query) +
            '&per_page=1&sort=author-date&order=desc';
  getJSON(url, githubHeaders(), function (raw) {
    if (typeof raw.total_count !== 'number') {
      console.log('github: search response has no total_count');
      return;
    }
    send({ GITHUB_TODAY_COMMITS: raw.total_count });
    if (raw.items && raw.items.length) {
      sendLatestGitHubCommit(raw.items[0]);
    } else {
      fetchLatestGitHubCommit(username);
    }
  });
}

function refreshQuotas(force) {
  var now = Date.now();
  if (!force && now - lastQuotaFetch < MIN_REFRESH_GAP_MS) return;
  lastQuotaFetch = now;
  fetchGitHubCommits();
  fetchCodex();
  fetchMiniMax();
}

// ------------------------------------------------------------------ weather

function hourlyIndexFor(times, targetSec) {
  var best = -1;
  var bestDiff = Infinity;
  for (var i = 0; i < times.length; i++) {
    var diff = Math.abs(times[i] - targetSec);
    if (diff < bestDiff) {
      bestDiff = diff;
      best = i;
    }
  }
  return best;
}

function buildWeatherMessage(data) {
  var msg = {};
  var now = Math.floor(Date.now() / 1000);

  if (data.current && typeof data.current.temperature_2m === 'number') {
    msg.WX_TEMP_NOW = Math.round(data.current.temperature_2m);
    msg.WX_CODE_NOW = data.current.weather_code;
  }

  var hourly = data.hourly;
  if (hourly && hourly.time && hourly.time.length) {
    [[6, '6H'], [24, '24H']].forEach(function (spec) {
      var idx = hourlyIndexFor(hourly.time, now + spec[0] * 3600);
      if (idx < 0) return;
      msg['WX_TEMP_' + spec[1]] = Math.round(hourly.temperature_2m[idx]);
      msg['WX_CODE_' + spec[1]] = hourly.weather_code[idx];
    });
    if (msg.WX_TEMP_NOW === undefined) {
      var i0 = hourlyIndexFor(hourly.time, now);
      if (i0 >= 0) {
        msg.WX_TEMP_NOW = Math.round(hourly.temperature_2m[i0]);
        msg.WX_CODE_NOW = hourly.weather_code[i0];
      }
    }
  }
  return msg;
}

function fetchWeather(lat, lon) {
  var cfg = getSettings();
  var url = 'https://api.open-meteo.com/v1/forecast' +
            '?latitude=' + lat + '&longitude=' + lon +
            '&current=temperature_2m,weather_code' +
            '&hourly=temperature_2m,weather_code' +
            '&forecast_days=3&timeformat=unixtime&timezone=UTC' +
            '&temperature_unit=' + cfg.units;
  getJSON(url, {}, function (data) { send(buildWeatherMessage(data)); });
}

// GPS first, then the configured lat/lon. Phones deny location to the Pebble app
// more often than not, so without a fallback the weather row just stays at "--".
function refreshWeather() {
  var cfg = getSettings();
  var fallback = function (why) {
    if (cfg.lat !== '' && cfg.lon !== '') {
      console.log('weather: ' + why + '; using configured lat/lon');
      fetchWeather(cfg.lat, cfg.lon);
    } else {
      console.log('weather: ' + why + ' and no lat/lon configured — set one in settings');
    }
  };
  if (!navigator.geolocation) {
    fallback('no geolocation on this phone');
    return;
  }
  navigator.geolocation.getCurrentPosition(
    function (pos) { fetchWeather(pos.coords.latitude, pos.coords.longitude); },
    function (err) { fallback('location error: ' + err.message); },
    { timeout: 15000, maximumAge: 30 * 60 * 1000 });
}

// ---------------------------------------------------------------- lifecycle

Pebble.addEventListener('ready', function () {
  var cfg = getSettings();
  var pendingLogin = loadCodexLogin();
  if (pendingLogin) pollCodexLogin(pendingLogin);
  sendAppearance();
  sendTargetDate();
  refreshQuotas(true);
  refreshWeather();
  setInterval(function () { refreshQuotas(true); },
              Math.max(1, cfg.refreshMin) * 60 * 1000);
  setInterval(refreshWeather, WEATHER_REFRESH_MIN * 60 * 1000);
});

// The watch asks for this on wrist-flick, on reconnect, and whenever its data
// has gone stale — this is what makes the face feel live rather than polled.
Pebble.addEventListener('appmessage', function (e) {
  if (!e.payload) return;
  if (e.payload.AI_PROVIDER !== undefined) {
    var cfg = getSettings();
    cfg.aiProvider = e.payload.AI_PROVIDER ? 'codex' : 'claude';
    saveSettings(cfg);
  }
  if (!e.payload.REQUEST_REFRESH) return;
  refreshQuotas();
  refreshWeather();
});

// ------------------------------------------------------------ configuration

// Settings page, served as a data: URL — no collector, no origin needed. Some
// phone apps refuse to navigate to data: URLs; if the gear icon does nothing,
// the non-Claude settings below are all available in DEFAULTS at the top of
// this file instead.
function configPage(cfg, codexConnected, codexPending) {
  return '<!DOCTYPE html><html><head><meta charset="utf-8">' +
    '<meta name="viewport" content="width=device-width,initial-scale=1">' +
    '<title>AI Quota</title><style>' +
    'body{font:16px -apple-system,Roboto,sans-serif;margin:0;padding:18px;background:#111;color:#eee}' +
    'h1{font-size:19px;margin:0 0 4px}p{color:#999;font-size:13px;margin:0 0 18px}' +
    'label{display:block;margin:14px 0 4px;font-size:13px;color:#bbb}' +
    'input,select{width:100%;box-sizing:border-box;padding:10px;font-size:16px;' +
    'border:1px solid #444;border-radius:6px;background:#1c1c1c;color:#eee}' +
    'button{width:100%;margin-top:22px;padding:13px;font-size:16px;border:0;' +
    'border-radius:6px;background:#e07b39;color:#fff}' +
    'fieldset{border:1px solid #333;border-radius:8px;margin:20px 0 0;padding:12px}' +
    'legend{padding:0 6px;font-size:13px;color:#bbb}' +
    '.code{font:bold 26px monospace;letter-spacing:3px;color:#7ddc7d;text-align:center}' +
    '.loginlink{display:block;text-align:center;margin:12px 0;color:#6bbcff}' +
    '.secondary{background:#444;margin-top:10px}' +
    '</style></head><body><h1>AI Quota</h1>' +
    '<p>The first row shows today’s GitHub commits. MiniMax quota is fetched live. ' +
    'Claude has no live quota API the phone can ' +
    'use reliably, so its 7-day window is a countdown you calibrate below.</p>' +
    '<label>GitHub username</label>' +
    '<input id="githubUsername" value="' + cfg.githubUsername + '" placeholder="e.g. LinBlink">' +
    '<label>GitHub read-only token (blank = keep current)</label>' +
    '<input id="githubToken" type="password" value="" placeholder="Needed for private repositories">' +
    '<p style="margin:6px 0 0">Only commits already pushed to GitHub can be counted.</p>' +
    '<label>7-day quota row</label><select id="aiProvider">' +
    '<option value="claude"' + (cfg.aiProvider === 'claude' ? ' selected' : '') + '>Claude</option>' +
    '<option value="codex"' + (cfg.aiProvider === 'codex' ? ' selected' : '') + '>ChatGPT Codex</option>' +
    '</select>' +
    '<p style="margin:6px 0 0">Press the watch select button to switch the 7D row instantly.</p>' +
    '<fieldset><legend>Claude — calibrate remaining time</legend>' +
    '<p style="margin:0 0 10px">Check claude.ai/settings/usage (or `claude /usage`) ' +
    'for how much is left in each window right now, then enter it here. Formats: ' +
    '"4d12h", "2:30" (H:MM), or a bare number (hours). ' +
    'Leave blank to leave the watch’s current countdown alone.</p>' +
    '<label>7-day window: time left</label>' +
    '<input id="claude7d" value="" placeholder="e.g. 4d12h">' +
    '</fieldset>' +
    '<fieldset><legend>ChatGPT Codex — automatic sync</legend>' +
    '<p id="codexStatus" style="margin:0 0 10px">' +
    (codexConnected ? 'Connected. The phone refreshes the token and quota over Wi-Fi or cellular.' :
     codexPending ? 'Authorization is pending in the phone companion.' :
                    'Not connected. Authorization runs on the phone; no computer is needed.') + '</p>' +
    '<button type="button" id="codexLogin">' +
    (codexConnected ? 'Reconnect Codex' : 'Connect Codex') + '</button>' +
    '<button type="button" id="codexDisconnect" class="secondary"' +
    (codexConnected ? '' : ' style="display:none"') + '>Disconnect Codex</button>' +
    '<p style="margin:10px 0">Connect closes settings and displays a one-time code. ' +
    'Open the shown address manually in Chrome or Safari. The companion keeps ' +
    'checking authorization after that page is closed.</p>' +
    '<p style="margin:14px 0 10px">Manual fallback: copy values from Codex Settings → Usage. ' +
    'Blank fields preserve the last automatic or manual values.</p>' +
    '<label>7-day used %</label><input id="codex7pct" type="number" min="0" max="100" placeholder="e.g. 61">' +
    '<label>7-day time left</label><input id="codex7d" placeholder="e.g. 2d8h">' +
    '</fieldset>' +
    '<fieldset><legend>Bank balance</legend>' +
    '<p style="margin:0 0 10px">Blank fields preserve existing values. A value ' +
    'sets that bank\'s latest baseline; only newer SMS or App notifications can ' +
    'replace it. The watch cycles NJ, ZS, GS and SUM once per second.</p>' +
    '<label>NJ current balance (CNY)</label>' +
    '<input id="njBalance" inputmode="decimal" placeholder="e.g. 1234.56">' +
    '<label>ZS current balance (CNY)</label>' +
    '<input id="zsBalance" inputmode="decimal" placeholder="e.g. 1234.56">' +
    '<label>GS current balance (CNY)</label>' +
    '<input id="gsBalance" inputmode="decimal" placeholder="e.g. 1234.56">' +
    '</fieldset>' +
    '<label>Countdown target date (blank = hide)</label>' +
    '<input id="targetDate" type="date" value="' + cfg.targetDate + '">' +
    '<label>Weather latitude (blank = phone GPS)</label>' +
    '<input id="lat" value="' + cfg.lat + '">' +
    '<label>Weather longitude</label><input id="lon" value="' + cfg.lon + '">' +
    '<label>Units</label><select id="units">' +
    '<option value="celsius"' + (cfg.units === 'celsius' ? ' selected' : '') + '>°C</option>' +
    '<option value="fahrenheit"' + (cfg.units === 'fahrenheit' ? ' selected' : '') + '>°F</option>' +
    '</select>' +
    '<label>Theme</label><select id="theme">' +
    '<option value="light"' + (cfg.theme === 'light' ? ' selected' : '') + '>Light</option>' +
    '<option value="dark"' + (cfg.theme === 'dark' ? ' selected' : '') + '>Dark</option>' +
    '</select>' +
    '<label>Clock font</label><select id="timeFont">' +
    '<option value="bitham"' + (cfg.timeFont === 'bitham' ? ' selected' : '') + '>Bitham</option>' +
    '<option value="consolas"' + (cfg.timeFont === 'consolas' ? ' selected' : '') + '>Consolas</option>' +
    '</select>' +
    '<label>Refresh interval (minutes)</label>' +
    '<input id="refreshMin" type="number" min="1" value="' + cfg.refreshMin + '">' +
    '<button id="save">Save</button><script>' +
    'var codexStart=false,codexClear=false;' +
    'function statusText(s){document.getElementById("codexStatus").textContent=s}' +
    'function formResult(){var g=function(i){return document.getElementById(i).value.trim()};' +
    'return {refreshMin:parseInt(g("refreshMin"),10)||5,' +
    'units:g("units"),lat:g("lat"),lon:g("lon"),targetDate:g("targetDate"),' +
    'githubUsername:g("githubUsername"),' +
    'githubToken:g("githubToken"),' +
    'theme:g("theme"),timeFont:g("timeFont"),aiProvider:g("aiProvider"),' +
    'claude7d:g("claude7d"),' +
    'codex7pct:g("codex7pct"),codex7d:g("codex7d"),' +
    'njBalance:g("njBalance"),zsBalance:g("zsBalance"),gsBalance:g("gsBalance"),' +
    'codexStart:codexStart,codexClear:codexClear}}' +
    'function saveAndClose(){location.href="pebblejs://close#"+' +
    'encodeURIComponent(JSON.stringify(formResult()))}' +
    'document.getElementById("codexLogin").onclick=function(){' +
    'codexStart=true;codexClear=false;statusText("Starting authorization on this phone…");saveAndClose()};' +
    'document.getElementById("codexDisconnect").onclick=function(){' +
    'codexStart=false;codexClear=true;statusText("Disconnecting…");saveAndClose()};' +
    'document.getElementById("save").onclick=saveAndClose;' +
    '<\/script></body></html>';
}

Pebble.addEventListener('showConfiguration', function () {
  var url;
  try {
    url = 'data:text/html;charset=utf-8,' +
          encodeURIComponent(configPage(getSettings(), !!loadCodexTokens().refreshToken,
                                         !!loadCodexLogin()));
  } catch (e) {
    // Never leave the gear icon silently dead: a broken settings object should
    // still get you a page you can type into.
    console.log('config build error: ' + e);
    url = 'data:text/html;charset=utf-8,' + encodeURIComponent(configPage(DEFAULTS, false, false));
  }
  console.log('showConfiguration -> ' + url.substring(0, 40) + '… (' + url.length + ' chars)');
  Pebble.openURL(url);
});

Pebble.addEventListener('webviewclosed', function (e) {
  if (!e.response) return;
  try {
    var parsed = JSON.parse(decodeURIComponent(e.response));
    if (!parsed.githubToken) parsed.githubToken = getSettings().githubToken;
    var calibration = claudeCalibrationMessage('', parsed.claude7d);
    merge(calibration, codexCalibrationMessage('', '',
                                                parsed.codex7pct, parsed.codex7d));
    var bankCalibration = {};
    var calibratedAt = Math.floor(Date.now() / 1000);
    merge(bankCalibration, bankBalanceMessage(parsed.njBalance,
                                               'BANK_BALANCE_CENTS',
                                               'BANK_UPDATED_AT', calibratedAt));
    merge(bankCalibration, bankBalanceMessage(parsed.zsBalance,
                                               'CMB_BALANCE_CENTS',
                                               'CMB_EVENT_AT', calibratedAt));
    merge(bankCalibration, bankBalanceMessage(parsed.gsBalance,
                                               'ICBC_BALANCE_CENTS',
                                               'ICBC_UPDATED_AT', calibratedAt));
    delete parsed.claude7d;
    delete parsed.codex7pct;
    delete parsed.codex7d;
    delete parsed.njBalance;
    delete parsed.zsBalance;
    delete parsed.gsBalance;
    if (parsed.codexClear) {
      localStorage.removeItem('codex_oauth');
      clearCodexLogin();
    }
    var startCodex = parsed.codexStart;
    delete parsed.codexClear;
    delete parsed.codexStart;
    saveSettings(parsed);
    sendAppearance();
    sendTargetDate();
    send(calibration);
    send(bankCalibration);
    refreshQuotas(true);
    refreshWeather();
    if (startCodex) startCodexLogin();
  } catch (err) {
    console.log('config parse error: ' + err);
  }
});
