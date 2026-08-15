/*
 * Phone-side companion. The watch has no network of its own, so this fetches
 * everything and pushes it over AppMessage:
 *
 *   - Claude quota, straight from api.anthropic.com using OAuth tokens baked in
 *     by tools/make_secrets.py. The phone refreshes the token itself.
 *   - MiniMax quota, straight from api.minimaxi.com with the coding-plan key.
 *   - Weather for now / +6h / +24h, from Open-Meteo (free, no API key).
 *
 * Nothing here needs the PC collector. tools/quota_collector.py still works and
 * still wins if you point DEFAULTS.url at it — see refreshQuotas below.
 *
 * On Claude's refresh token: it ROTATES. The moment this code refreshes, the
 * copy in ~/.claude/.credentials.json on the PC is dead and Claude Code there
 * will ask you to log in again. That is the accepted trade for having the watch
 * work away from the PC. After any such re-login, re-run make_secrets.py and
 * reinstall to re-seed the phone.
 */

// Substituted by tools/make_secrets.py, which renders this template into
// src/pkjs/index.js. It is inlined rather than `require`d from a second file
// because require() needs enableMultiJS, which needs webpack — one build
// dependency more than this is worth.
var SECRETS = /*__SECRETS__*/{};

// First line of output in `pebble logs`. If you don't see it, the JS never
// loaded at all — which looks identical to "the gear icon does nothing", since
// an unloaded pkjs registers no showConfiguration listener either.
console.log('pkjs boot: claude=' + (SECRETS.claudeRefreshToken ? 'yes' : 'NO') +
            ' minimax=' + (SECRETS.minimaxKey ? 'yes' : 'NO'));

// ---------------------------------------------------------------- defaults
//
// Everything the watchface needs is baked in here, so it works with no settings
// page at all. Anything saved through the settings page overrides these.
var DEFAULTS = {
  url: '',            // optional PC collector, e.g. 'https://x.trycloudflare.com/quota.json'
  token: '',          // optional bearer token for that endpoint
  refreshMin: 5,
  units: 'celsius',   // or 'fahrenheit'
  lat: '',            // blank = use phone GPS
  lon: '',
  targetDate: ''      // 'YYYY-MM-DD' for the day counter; blank hides it
};

var DEFAULT_QUOTA_REFRESH_MIN = 5;
var WEATHER_REFRESH_MIN = 30;
// Ignore watch-initiated refreshes that arrive faster than this, so a shaky
// wrist can't hammer the endpoints.
var MIN_REFRESH_GAP_MS = 20 * 1000;

// Claude Code's own OAuth client. The token endpoint moved to platform.claude.com;
// console.anthropic.com still answers, so try both before giving up.
var CLAUDE_CLIENT_ID = '9d1c250a-e61b-44d9-88ed-5944d1962f5e';
var CLAUDE_TOKEN_URLS = [
  'https://platform.claude.com/v1/oauth/token',
  'https://console.anthropic.com/v1/oauth/token'
];
var CLAUDE_USAGE_URL = 'https://api.anthropic.com/api/oauth/usage';
var MINIMAX_USAGE_URL = 'https://api.minimaxi.com/v1/token_plan/remains';

// Refresh a bit before the token actually dies, so a slow request can't land
// on the far side of the expiry.
var TOKEN_EARLY_REFRESH_MS = 5 * 60 * 1000;

var lastQuotaFetch = 0;

// The usage endpoint rate-limits, and the watch can ask for a refresh on every
// wrist flick and reconnect. Without a backoff those requests keep the limit
// alive; MiniMax and weather carry on regardless.
var CLAUDE_BACKOFF_MS = 15 * 60 * 1000;
var claudeBackoffUntil = 0;

function getSettings() {
  var raw = localStorage.getItem('settings');
  var s = {};
  if (raw) {
    try { s = JSON.parse(raw); } catch (e) { s = {}; }
  }
  var units = s.units || DEFAULTS.units;
  return {
    url: s.url || DEFAULTS.url,
    token: s.token || DEFAULTS.token,
    refreshMin: s.refreshMin || DEFAULTS.refreshMin || DEFAULT_QUOTA_REFRESH_MIN,
    units: units === 'fahrenheit' ? 'fahrenheit' : 'celsius',
    lat: s.lat || DEFAULTS.lat,
    lon: s.lon || DEFAULTS.lon,
    targetDate: s.targetDate || DEFAULTS.targetDate
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
      fail(req.status);
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

// ------------------------------------------------------------- claude tokens
//
// The token set lives in localStorage so a rotation survives a phone restart.
// It is seeded from the baked-in copy, and re-seeded whenever a rebuild brings
// a different refresh token (i.e. after you log in to Claude Code again).

function loadTokens() {
  var t = {};
  var raw = localStorage.getItem('claude_oauth');
  if (raw) {
    try { t = JSON.parse(raw); } catch (e) { t = {}; }
  }
  var baked = SECRETS.claudeRefreshToken || '';
  if (baked && t.seededFrom !== baked) {
    // First run, or the build carries a newer token than what we rotated to.
    t = {
      refreshToken: baked,
      accessToken: SECRETS.claudeAccessToken || '',
      expiresAt: SECRETS.claudeExpiresAt || 0,
      seededFrom: baked
    };
    localStorage.setItem('claude_oauth', JSON.stringify(t));
    console.log('claude: seeded tokens from the build');
  }
  return t;
}

function saveTokens(t) {
  localStorage.setItem('claude_oauth', JSON.stringify(t));
}

// Swap the refresh token for a fresh pair. The rotated refresh token is saved
// before anything else happens: losing it means re-logging-in on the PC and
// rebuilding, so it must never be dropped because a later step failed.
function refreshClaudeToken(onDone, urlIndex) {
  var t = loadTokens();
  if (!t.refreshToken) {
    console.log('claude: no refresh token — run tools/make_secrets.py and rebuild');
    onDone(null);
    return;
  }
  var i = urlIndex || 0;
  if (i >= CLAUDE_TOKEN_URLS.length) {
    console.log('claude: token refresh failed on every endpoint');
    onDone(null);
    return;
  }

  var body = JSON.stringify({
    grant_type: 'refresh_token',
    refresh_token: t.refreshToken,
    client_id: CLAUDE_CLIENT_ID
  });

  // Note: Claude Code also sends User-Agent and x-app headers here. Those are
  // forbidden headers for XHR, so the phone cannot set them — if Cloudflare
  // starts requiring them this call returns 403 and the Claude rows go stale.
  request('POST', CLAUDE_TOKEN_URLS[i], { 'Content-Type': 'application/json' }, body,
    function (data) {
      if (!data.access_token) {
        console.log('claude: refresh response had no access_token');
        onDone(null);
        return;
      }
      var next = {
        accessToken: data.access_token,
        refreshToken: data.refresh_token || t.refreshToken,
        expiresAt: Date.now() + (data.expires_in ? data.expires_in * 1000 : 3600 * 1000),
        seededFrom: t.seededFrom
      };
      saveTokens(next);
      console.log('claude: token refreshed' +
                  (data.refresh_token ? ' (refresh token rotated — the PC copy is now dead)' : ''));
      onDone(next.accessToken);
    },
    function () { refreshClaudeToken(onDone, i + 1); });
}

function withClaudeToken(onToken) {
  var t = loadTokens();
  if (t.accessToken && t.expiresAt && t.expiresAt - TOKEN_EARLY_REFRESH_MS > Date.now()) {
    onToken(t.accessToken);
    return;
  }
  refreshClaudeToken(onToken);
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

// --- claude, direct

// api.anthropic.com reports utilization as a used percentage, 0-100.
function claudeWindow(block) {
  if (!block || typeof block.utilization !== 'number') return null;
  return { used_pct: Math.round(block.utilization), reset_at: block.resets_at };
}

function fetchClaude(retrying) {
  if (Date.now() < claudeBackoffUntil) return;
  withClaudeToken(function (token) {
    if (!token) return;
    getJSON(CLAUDE_USAGE_URL,
      { Authorization: 'Bearer ' + token, 'anthropic-beta': 'oauth-2025-04-20' },
      function (raw) {
        var msg = {};
        merge(msg, windowMessage('CLAUDE_5H', claudeWindow(raw.five_hour)));
        merge(msg, windowMessage('CLAUDE_WK', claudeWindow(raw.seven_day)));
        send(msg);
      },
      function (status) {
        if (status === 429) {
          claudeBackoffUntil = Date.now() + CLAUDE_BACKOFF_MS;
          console.log('claude: rate limited — pausing for ' +
                      (CLAUDE_BACKOFF_MS / 60000) + ' min');
          return;
        }
        // A token can be rejected before its stated expiry — refresh once and retry.
        if (status === 401 && !retrying) {
          refreshClaudeToken(function (fresh) { if (fresh) fetchClaude(true); });
        }
      });
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

// --- optional PC collector
//
// Tolerate both the nested shape and a flat one like {claude_5h_pct: 42, ...}
function pickWindow(data, provider, which) {
  var p = data[provider];
  if (p) {
    var w = p[which] || p[which === 'five_hour' ? '5h' : 'week'];
    if (w) return w;
  }
  var flatKey = provider + '_' + (which === 'five_hour' ? '5h' : 'week');
  if (data[flatKey]) return data[flatKey];
  if (typeof data[flatKey + '_pct'] === 'number') {
    return { used_pct: data[flatKey + '_pct'], reset_at: data[flatKey + '_reset'] };
  }
  return null;
}

function fetchCollector(cfg) {
  var headers = cfg.token ? { Authorization: 'Bearer ' + cfg.token } : {};
  getJSON(cfg.url, headers, function (data) {
    var msg = {};
    [['claude',  'five_hour', 'CLAUDE_5H'],
     ['claude',  'weekly',    'CLAUDE_WK'],
     ['minimax', 'five_hour', 'MINIMAX_5H'],
     ['minimax', 'weekly',    'MINIMAX_WK']].forEach(function (spec) {
      merge(msg, windowMessage(spec[2], pickWindow(data, spec[0], spec[1])));
    });
    send(msg);
  });
}

function refreshQuotas(force) {
  var cfg = getSettings();
  var now = Date.now();
  if (!force && now - lastQuotaFetch < MIN_REFRESH_GAP_MS) return;
  lastQuotaFetch = now;

  // A configured collector replaces both direct fetches — it already merges the
  // two providers, and going through it keeps the tokens off the phone.
  if (cfg.url) {
    fetchCollector(cfg);
    return;
  }
  fetchClaude(false);
  fetchMiniMax();
}

// ------------------------------------------------------------------ weather

// Open-Meteo returns hourly arrays; pick the entry closest to the wanted time.
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
    // Fall back to the hourly series if the current block was missing.
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
  if (!e.payload || !e.payload.REQUEST_REFRESH) return;
  refreshQuotas();
  refreshWeather();
});

// ------------------------------------------------------------ configuration

// Settings page. With the tokens baked in there is no collector to host it, so
// it is served as a data: URL. Some phone apps refuse to navigate to those — if
// the gear icon does nothing, the settings below are all available in DEFAULTS
// at the top of this file instead, and a running collector's /config still wins.
function configPage(cfg) {
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
    '</style></head><body><h1>AI Quota</h1>' +
    '<p>Quotas come straight from the phone. Leave the collector URL blank unless ' +
    'you run tools/quota_collector.py.</p>' +
    '<label>Countdown target date (blank = hide)</label>' +
    '<input id="targetDate" type="date" value="' + cfg.targetDate + '">' +
    '<label>Weather latitude (blank = phone GPS)</label>' +
    '<input id="lat" value="' + cfg.lat + '">' +
    '<label>Weather longitude</label><input id="lon" value="' + cfg.lon + '">' +
    '<label>Units</label><select id="units">' +
    '<option value="celsius"' + (cfg.units === 'celsius' ? ' selected' : '') + '>°C</option>' +
    '<option value="fahrenheit"' + (cfg.units === 'fahrenheit' ? ' selected' : '') + '>°F</option>' +
    '</select>' +
    '<label>Refresh interval (minutes)</label>' +
    '<input id="refreshMin" type="number" min="1" value="' + cfg.refreshMin + '">' +
    '<label>Collector URL (optional)</label><input id="url" value="' + cfg.url + '">' +
    '<label>Collector token (optional)</label><input id="token" value="' + cfg.token + '">' +
    '<button id="save">Save</button><script>' +
    'document.getElementById("save").onclick=function(){' +
    'var g=function(i){return document.getElementById(i).value.trim()};' +
    'var out={url:g("url"),token:g("token"),refreshMin:parseInt(g("refreshMin"),10)||5,' +
    'units:g("units"),lat:g("lat"),lon:g("lon"),targetDate:g("targetDate")};' +
    'location.href="pebblejs://close#"+encodeURIComponent(JSON.stringify(out))};' +
    '<\/script></body></html>';
}

// A running collector serves the same page at /config, from a real origin.
function collectorConfigUrl(cfg) {
  var m = /^(https?:\/\/[^/?#]+)/.exec(cfg.url);
  if (!m) return null;
  var current = {
    url: cfg.url, token: cfg.token, refreshMin: cfg.refreshMin,
    units: cfg.units, lat: cfg.lat, lon: cfg.lon, targetDate: cfg.targetDate
  };
  return m[1] + '/config?current=' + encodeURIComponent(JSON.stringify(current));
}

Pebble.addEventListener('showConfiguration', function () {
  var url;
  try {
    var cfg = getSettings();
    url = collectorConfigUrl(cfg);
    if (!url) url = 'data:text/html;charset=utf-8,' + encodeURIComponent(configPage(cfg));
  } catch (e) {
    // Never leave the gear icon silently dead: a broken settings object should
    // still get you a page you can type into.
    console.log('config build error: ' + e);
    url = 'data:text/html;charset=utf-8,' + encodeURIComponent(configPage(DEFAULTS));
  }
  console.log('showConfiguration -> ' + url.substring(0, 40) + '… (' + url.length + ' chars)');
  Pebble.openURL(url);
});

Pebble.addEventListener('webviewclosed', function (e) {
  if (!e.response) return;
  try {
    saveSettings(JSON.parse(decodeURIComponent(e.response)));
    sendTargetDate();
    refreshQuotas(true);
    refreshWeather();
  } catch (err) {
    console.log('config parse error: ' + err);
  }
});
