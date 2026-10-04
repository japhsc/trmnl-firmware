#ifdef BAHNBRETT_LOCAL_RENDER

#include "bahnbrett_config_web.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <DNSServer.h>
#include <Preferences.h>

#include <ESPAsyncWebServer.h>
#include <trmnl_log.h>

#include <bahnbrett/config.h>
#include <bahnbrett/config_store.h>
#include <bahnbrett/json.h>
#include <bahnbrett/types.h>

#include <algorithm>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// HTML template for the config editor.
// {{CONFIG_JSON}} is replaced server-side with the current config as JSON.
// ---------------------------------------------------------------------------
static const char kConfigPageHtml[] PROGMEM = R"=====(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Bahnbrett Config</title>
<style>
  *,*::before,*::after{box-sizing:border-box}
  body{font-family:-apple-system,BlinkMacSystemFont,sans-serif;margin:0;padding:16px;
       background:#1a1a2e;color:#e0e0e0;font-size:14px}
  h1{font-size:20px;margin:0 0 16px;color:#e94560}
  h2{font-size:16px;margin:20px 0 8px;color:#e94560;border-bottom:1px solid #333;padding-bottom:4px}
  label{display:block;margin:8px 0 4px;font-weight:600;font-size:13px}
  input,textarea,select{width:100%;padding:8px 10px;border:1px solid #444;border-radius:6px;
      background:#16213e;color:#e0e0e0;font-size:14px;margin-bottom:4px}
  textarea{resize:vertical;min-height:60px}
  input[type=checkbox]{width:auto;margin-right:8px}
  input[type=range]{padding:0}
  .check-row{display:flex;align-items:center;margin:8px 0}
  .check-row label{margin:0;font-weight:400}
  .hint{font-size:11px;color:#888;margin-bottom:8px}
  .btn-row{display:flex;gap:12px;margin-top:24px}
  button{flex:1;padding:12px;border:none;border-radius:6px;font-size:16px;font-weight:600;cursor:pointer}
  .btn-save{background:#e94560;color:#fff}
  .btn-cancel{background:#333;color:#ccc}
  .status{display:none;padding:12px;border-radius:6px;margin-bottom:16px;text-align:center}
  .status-ok{background:#1b5e20;color:#a5d6a7}
  .status-err{background:#b71c1c;color:#ef9a9a}
</style>
</head>
<body>
<h1>Bahnbrett Configuration</h1>
<div id="status" class="status"></div>

<form id="cfg-form">
<h2>Stations</h2>
<label for="station_names">Station names (one per line)</label>
<textarea id="station_names" rows="4"></textarea>
<div class="hint">Enter stop names exactly as they appear on departures (e.g. "U Turmstr.")</div>

<h2>Layout</h2>
<label for="grid_cols">Grid columns</label>
<input type="number" id="grid_cols" min="0" max="6">
<div class="hint">0 = auto</div>

<label for="grid_rows">Grid rows</label>
<input type="number" id="grid_rows" min="0" max="6">
<div class="hint">0 = auto</div>

<label for="col_width_ratios">Column width ratios</label>
<input type="text" id="col_width_ratios" placeholder="1,1,1">
<div class="hint">Comma-separated weights (1-10). Empty = equal widths. Only active when columns are explicit.</div>

<label for="row_height_ratios">Row height ratios</label>
<input type="text" id="row_height_ratios" placeholder="1,1,1">
<div class="hint">Comma-separated weights (1-10). Empty = equal heights. Only active when rows are explicit.</div>

<label for="max_rows_per_widget">Max rows per widget</label>
<input type="number" id="max_rows_per_widget" min="1" max="60">

<label for="max_departures_per_group">Max departures per grouped row</label>
<input type="number" id="max_departures_per_group" min="1" max="20">

<div class="check-row">
  <input type="checkbox" id="aggregate_by_destination">
  <label for="aggregate_by_destination">Aggregate by destination</label>
</div>

<h2>Timing</h2>
<label for="countdown_minutes">Countdown threshold (minutes)</label>
<input type="number" id="countdown_minutes" min="1" max="60">
<div class="hint">Departures within this many minutes are shown as "X min" instead of clock time</div>

<label for="min_departure_minutes">Min departure offset (minutes)</label>
<input type="number" id="min_departure_minutes" min="0" max="120">
<div class="hint">Hide departures sooner than this many minutes from now</div>

<label for="max_departure_minutes">Max departure lookahead (minutes)</label>
<input type="number" id="max_departure_minutes" min="5" max="1440">

<label for="refresh_seconds">Refresh interval (seconds)</label>
<input type="number" id="refresh_seconds" min="30" max="86400">
<div class="hint">Sleep time between two renders. Short intervals drain the battery faster. Default: 900</div>

<h2>Appearance</h2>
<label for="header_font_scale">Header font scale</label>
<input type="range" id="header_font_scale" min="0.5" max="4" step="0.1">
<span id="header_font_scale_val"></span>

<label for="widget_font_scale">Widget font scale</label>
<input type="range" id="widget_font_scale" min="0.5" max="4" step="0.1">
<span id="widget_font_scale_val"></span>

<div class="check-row">
  <input type="checkbox" id="show_delays">
  <label for="show_delays">Show delay annotations (+N min)</label>
</div>

<h2>Location &amp; Time</h2>
<label for="timezone">POSIX timezone string</label>
<input type="text" id="timezone">
<div class="hint">e.g. "CET-1CEST,M3.5.0,M10.5.0/3" for Berlin</div>

<label for="home_city">Home city</label>
<input type="text" id="home_city">

<h2>Filters</h2>
<label for="line_filters">Line filters (one per line, empty = all)</label>
<textarea id="line_filters" rows="2"></textarea>
<div class="hint">e.g. "U2", "S5"</div>

<label for="route_filters">Route filters (one per line, empty = all)</label>
<textarea id="route_filters" rows="2"></textarea>

<label for="direction_filters">Direction filters (one per line, empty = all)</label>
<textarea id="direction_filters" rows="2"></textarea>

<label for="mode_filters">Mode filters (one per line, empty = all)</label>
<textarea id="mode_filters" rows="2"></textarea>
<div class="hint">Transit modes, e.g. "SUBURBAN", "SUBWAY", "BUS", "REGIONAL_RAIL"</div>

<h2>Data Source</h2>
<label for="provider">Departures provider</label>
<select id="provider">
  <option value="transitous">Transitous (Berlin, Hamburg, all of Germany)</option>
  <option value="vbb">VBB (Berlin and Brandenburg)</option>
  <option value="hvv">HVV Geofox (Hamburg, needs credentials)</option>
</select>
<div class="hint">Transitous is the default. The device re-resolves all station names when the provider changes.</div>

<label for="transitous_base_url">Transitous API base URL</label>
<input type="text" id="transitous_base_url" placeholder="https://api.transitous.org/api/">
<div class="hint">Applies when the provider is Transitous. Empty = public Transitous API.</div>

<label for="api_base_url">VBB API base URL</label>
<input type="text" id="api_base_url" placeholder="https://v6.vbb.transport.rest">
<div class="hint">Applies when the provider is VBB. Point this at a self-hosted vbb-rest instance. Empty = public VBB API.</div>

<label for="hvv_user">HVV Geofox user</label>
<input type="text" id="hvv_user" autocomplete="off">
<div class="hint">Applies when the provider is HVV. Request access at hvv.de (e-mail to api@hochbahn.de).</div>

<label for="hvv_key">HVV Geofox key</label>
<input type="password" id="hvv_key" autocomplete="off">
<div class="hint">The HMAC secret that HVV assigns to the user. The HVV provider needs both fields.</div>

<label for="hvv_base_url">HVV Geofox base URL</label>
<input type="text" id="hvv_base_url" placeholder="https://gti.geofox.de/gti/public">
<div class="hint">Applies when the provider is HVV. Empty = public GTI service.</div>

<label for="min_poll_seconds">Minimum poll interval (seconds)</label>
<input type="number" id="min_poll_seconds" min="30" max="3600">
<div class="hint">Shortest spacing between two departures requests for the same stop. The floor is 30 seconds.</div>

<h2>Fetch</h2>
<label for="max_departures_to_fetch_per_widget">Max departures to fetch per widget</label>
<input type="number" id="max_departures_to_fetch_per_widget" min="5" max="200">

<h2>Remote Config</h2>
<label for="remote_config_url">Config URL (optional)</label>
<input type="text" id="remote_config_url" placeholder="https://example.com/bahnbrett-config.json">
<div class="hint">Device fetches this JSON on wakeup and merges it over local settings</div>

<label for="remote_config_interval">Remote fetch interval (wakeups)</label>
<input type="number" id="remote_config_interval" min="1" max="144">
<div class="hint">Fetch remote config every N wakeups (1 = every time)</div>

<div class="btn-row">
  <button type="button" class="btn-cancel" onclick="cancelConfig()">Cancel</button>
  <button type="button" class="btn-save" onclick="saveConfig()">Save &amp; Reboot</button>
</div>
</form>

<p class="hint">
  Departure data: <a href="https://transitous.org/sources/" target="_blank">Transitous</a>
  (community service, non-commercial and open-source use only) or
  <a href="https://vbb.transport.rest/" target="_blank">VBB</a>.
  HVV data requires registration with HVV.
</p>

<script>
var cfg = CONFIG_JSON_PLACEHOLDER;

function populate() {
  var setVal = function(id, val) {
    var el = document.getElementById(id);
    if (!el) return;
    if (el.type === 'checkbox') el.checked = !!val;
    else if (el.type === 'range') { el.value = val; updateRange(id + '_val', val); }
    else el.value = val !== undefined && val !== null ? val : '';
  };
  var setLines = function(id, arr) {
    setVal(id, Array.isArray(arr) ? arr.join('\n') : '');
  };

  setLines('station_names', cfg.station_names);
  setVal('grid_cols', cfg.grid_cols);
  setVal('grid_rows', cfg.grid_rows);
  setVal('col_width_ratios', Array.isArray(cfg.col_width_ratios) ? cfg.col_width_ratios.join(',') : '');
  setVal('row_height_ratios', Array.isArray(cfg.row_height_ratios) ? cfg.row_height_ratios.join(',') : '');
  setVal('max_rows_per_widget', cfg.max_rows_per_widget);
  setVal('max_departures_per_group', cfg.max_departures_per_group);
  setVal('aggregate_by_destination', cfg.aggregate_by_destination);
  setVal('countdown_minutes', cfg.countdown_minutes);
  setVal('min_departure_minutes', cfg.min_departure_minutes);
  setVal('max_departure_minutes', cfg.max_departure_minutes);
  setVal('refresh_seconds', cfg.refresh_seconds);
  setVal('header_font_scale', cfg.header_font_scale);
  setVal('widget_font_scale', cfg.widget_font_scale);
  setVal('show_delays', cfg.show_delays);
  setVal('timezone', cfg.timezone);
  setVal('home_city', cfg.home_city);
  setLines('line_filters', cfg.line_filters);
  setLines('route_filters', cfg.route_filters);
  setLines('direction_filters', cfg.direction_filters);
  setLines('mode_filters', cfg.mode_filters);
  setVal('provider', cfg.provider || 'transitous');
  setVal('transitous_base_url', cfg.transitous_base_url);
  setVal('api_base_url', cfg.api_base_url);
  setVal('hvv_user', cfg.hvv_user);
  setVal('hvv_key', cfg.hvv_key);
  setVal('hvv_base_url', cfg.hvv_base_url);
  setVal('min_poll_seconds', cfg.min_poll_seconds);
  setVal('max_departures_to_fetch_per_widget', cfg.max_departures_to_fetch_per_widget);
  setVal('remote_config_url', cfg.remote_config_url);
  setVal('remote_config_interval', cfg.remote_config_interval);

  updateRange('header_font_scale_val', cfg.header_font_scale);
  updateRange('widget_font_scale_val', cfg.widget_font_scale);
}

function updateRange(spanId, val) {
  var s = document.getElementById(spanId);
  if (s) s.textContent = parseFloat(val).toFixed(1);
}

document.getElementById('header_font_scale').oninput = function() {
  updateRange('header_font_scale_val', this.value);
};
document.getElementById('widget_font_scale').oninput = function() {
  updateRange('widget_font_scale_val', this.value);
};

function showStatus(msg, ok) {
  var s = document.getElementById('status');
  s.textContent = msg;
  s.className = 'status ' + (ok ? 'status-ok' : 'status-err');
  s.style.display = 'block';
}

function parseRatios(str) {
  if (!str.trim()) return [];
  return str.split(',').map(function(s) {
    var n = parseInt(s.trim(), 10);
    return (isNaN(n) || n < 1) ? 1 : (n > 10 ? 10 : n);
  });
}

function getFormData() {
  var getLines = function(id) {
    var v = document.getElementById(id).value.trim();
    return v ? v.split('\n').map(function(s){return s.trim()}).filter(function(s){return s}) : [];
  };
  return {
    station_names: getLines('station_names'),
    grid_cols: parseInt(document.getElementById('grid_cols').value) || 0,
    grid_rows: parseInt(document.getElementById('grid_rows').value) || 0,
    col_width_ratios: parseRatios(document.getElementById('col_width_ratios').value),
    row_height_ratios: parseRatios(document.getElementById('row_height_ratios').value),
    max_rows_per_widget: parseInt(document.getElementById('max_rows_per_widget').value) || 10,
    max_departures_per_group: parseInt(document.getElementById('max_departures_per_group').value) || 3,
    aggregate_by_destination: document.getElementById('aggregate_by_destination').checked,
    countdown_minutes: parseInt(document.getElementById('countdown_minutes').value) || 15,
    min_departure_minutes: parseInt(document.getElementById('min_departure_minutes').value) || 0,
    max_departure_minutes: parseInt(document.getElementById('max_departure_minutes').value) || 180,
    refresh_seconds: parseInt(document.getElementById('refresh_seconds').value) || 900,
    header_font_scale: parseFloat(document.getElementById('header_font_scale').value) || 1.0,
    widget_font_scale: parseFloat(document.getElementById('widget_font_scale').value) || 1.0,
    show_delays: document.getElementById('show_delays').checked,
    timezone: document.getElementById('timezone').value.trim() || 'CET-1CEST,M3.5.0,M10.5.0/3',
    home_city: document.getElementById('home_city').value.trim() || 'Berlin',
    line_filters: getLines('line_filters'),
    route_filters: getLines('route_filters'),
    direction_filters: getLines('direction_filters'),
    mode_filters: getLines('mode_filters'),
    provider: document.getElementById('provider').value,
    transitous_base_url: document.getElementById('transitous_base_url').value.trim(),
    api_base_url: document.getElementById('api_base_url').value.trim() || 'https://v6.vbb.transport.rest',
    hvv_user: document.getElementById('hvv_user').value.trim(),
    hvv_key: document.getElementById('hvv_key').value.trim(),
    hvv_base_url: document.getElementById('hvv_base_url').value.trim(),
    min_poll_seconds: parseInt(document.getElementById('min_poll_seconds').value) || 60,
    max_departures_to_fetch_per_widget: parseInt(document.getElementById('max_departures_to_fetch_per_widget').value) || 50,
    remote_config_url: document.getElementById('remote_config_url').value.trim(),
    remote_config_interval: parseInt(document.getElementById('remote_config_interval').value) || 1
  };
}

function saveConfig() {
  var data = getFormData();
  var xhr = new XMLHttpRequest();
  xhr.open('POST', '/api/config/save', true);
  xhr.setRequestHeader('Content-Type', 'application/json');
  xhr.onload = function() {
    if (xhr.status === 200) {
      showStatus('Config saved. Rebooting...', true);
      setTimeout(function(){ document.body.innerHTML = '<h1>Config saved!</h1><p>Device is rebooting. Reconnect to your regular WiFi network.</p>'; }, 1000);
    } else {
      showStatus('Save failed: ' + xhr.responseText, false);
    }
  };
  xhr.onerror = function() { showStatus('Network error.', false); };
  xhr.send(JSON.stringify(data));
}

function cancelConfig() {
  var xhr = new XMLHttpRequest();
  xhr.open('POST', '/api/config/cancel', true);
  xhr.onload = function() { document.body.innerHTML = '<h1>Config cancelled</h1><p>Device will return to normal mode. Reconnect to your regular WiFi network.</p>'; };
  xhr.send();
}

populate();
</script>
</body>
</html>
)=====";

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

// Escapes '<' as a JSON unicode escape so a config value such as
// "</script>" cannot close the page's script block. Every '<' in the JSON
// text sits inside a string literal, so the escape keeps the JSON valid
// and the browser decodes it back to '<'; see ADR 0042.
std::string EscapeForScriptEmbed(const std::string& json) {
  std::string out;
  out.reserve(json.size());
  for (char c : json) {
    if (c == '<') {
      out += "\\u003c";
    } else {
      out += c;
    }
  }
  return out;
}

// Build a JSON string for the current config (including remote_config_url).
std::string CurrentConfigJson() {
  bahnbrett::DashboardConfig cfg = bahnbrett::LoadEffectiveConfig();

  // Also read remote_config_url and remote_config_interval from NVS
  // (these aren't DashboardConfig fields, they're stored separately).
  Preferences prefs;
  std::string remote_url;
  int remote_interval = 1;
  if (prefs.begin("bahnbrett", true)) {
    if (prefs.isKey("remote_url")) {
      remote_url = prefs.getString("remote_url").c_str();
    }
    if (prefs.isKey("remote_intv")) {
      remote_interval = prefs.getInt("remote_intv");
    }
    prefs.end();
  }

  bahnbrett::JsonBuilder jb;
  jb.StartObject();
  auto WriteStrVec = [&](const char* key, const std::vector<std::string>& vec) {
    jb.Key(key);
    jb.StartArray();
    for (const auto& s : vec) jb.String(s);
    jb.EndArray();
    jb.Comma();
  };

  jb.KeyNumber("max_rows_per_widget", cfg.max_rows_per_widget);
  jb.KeyNumber("max_departures_to_fetch_per_widget", cfg.max_departures_to_fetch_per_widget);
  jb.KeyNumber("countdown_minutes", cfg.countdown_minutes);
  jb.KeyNumber("refresh_seconds", cfg.refresh_seconds);
  jb.KeyNumber("min_poll_seconds", cfg.min_poll_seconds);
  jb.KeyNumberD("header_font_scale", cfg.header_font_scale);
  jb.KeyNumberD("widget_font_scale", cfg.widget_font_scale);
  jb.KeyString("timezone", cfg.timezone);
  jb.KeyString("provider", cfg.provider);
  jb.KeyString("api_base_url", cfg.api_base_url);
  jb.KeyString("transitous_base_url", cfg.transitous_base_url);
  jb.KeyString("hvv_base_url", cfg.hvv_base_url);
  jb.KeyString("hvv_user", cfg.hvv_user);
  jb.KeyString("hvv_key", cfg.hvv_key);
  WriteStrVec("station_names", cfg.station_names);
  WriteStrVec("line_filters", cfg.line_filters);
  WriteStrVec("route_filters", cfg.route_filters);
  WriteStrVec("direction_filters", cfg.direction_filters);
  WriteStrVec("mode_filters", cfg.mode_filters);
  jb.KeyBool("aggregate_by_destination", cfg.aggregate_by_destination);
  jb.KeyNumber("max_departures_per_group", cfg.max_departures_per_group);
  jb.KeyNumber("min_departure_minutes", cfg.min_departure_minutes);
  jb.KeyNumber("max_departure_minutes", cfg.max_departure_minutes);
  jb.KeyString("home_city", cfg.home_city);
  jb.KeyBool("show_delays", cfg.show_delays);
  jb.KeyNumber("grid_cols", cfg.grid_cols);
  jb.KeyNumber("grid_rows", cfg.grid_rows);

  // col_width_ratios (int array)
  jb.Key("col_width_ratios");
  jb.StartArray();
  for (size_t i = 0; i < cfg.col_width_ratios.size(); ++i)
    jb.Number(cfg.col_width_ratios[i]);
  jb.EndArray();
  jb.Comma();

  // row_height_ratios (int array)
  jb.Key("row_height_ratios");
  jb.StartArray();
  for (size_t i = 0; i < cfg.row_height_ratios.size(); ++i)
    jb.Number(cfg.row_height_ratios[i]);
  jb.EndArray();
  jb.Comma();

  jb.KeyString("remote_config_url", remote_url);
  jb.KeyNumber("remote_config_interval", remote_interval);
  jb.EndObject();

  return jb.str();
}

// Deserialize form JSON into DashboardConfig and save to NVS.
// Also saves remote_config_url and remote_config_interval. On failure the
// function writes a full sentence into *error that states the cause and
// the fix; see ADR 0042.
bool SaveConfigFromJson(const std::string& json_str, std::string* error) {
  bahnbrett::JsonValue root;
  if (!bahnbrett::ParseJson(json_str, &root) || root.type != bahnbrett::JsonValue::Type::kObject) {
    *error = "The config body is not valid JSON. Check the form fields and save the config again.";
    return false;
  }

  bahnbrett::DashboardConfig config = bahnbrett::DefaultDashboardConfig();

  // Parse all known fields from the form JSON
  auto getInt = [&](const char* key, int fallback) -> int {
    const auto* v = root.Find(key);
    return (v && v->type == bahnbrett::JsonValue::Type::kNumber) ? static_cast<int>(v->number_value) : fallback;
  };
  auto getFloat = [&](const char* key, float fallback) -> float {
    const auto* v = root.Find(key);
    return (v && v->type == bahnbrett::JsonValue::Type::kNumber) ? static_cast<float>(v->number_value) : fallback;
  };
  auto getBool = [&](const char* key, bool fallback) -> bool {
    const auto* v = root.Find(key);
    return (v && v->type == bahnbrett::JsonValue::Type::kBool) ? v->bool_value : fallback;
  };
  auto getString = [&](const char* key, const std::string& fallback) -> std::string {
    const auto* v = root.Find(key);
    return (v && v->type == bahnbrett::JsonValue::Type::kString) ? v->string_value : fallback;
  };
  auto getStrVec = [&](const char* key) -> std::vector<std::string> {
    const auto* v = root.Find(key);
    std::vector<std::string> out;
    if (v && v->type == bahnbrett::JsonValue::Type::kArray) {
      for (const auto& item : v->array) {
        if (item.type == bahnbrett::JsonValue::Type::kString) {
          out.push_back(item.string_value);
        }
      }
    }
    return out;
  };
  auto getIntVec = [&](const char* key) -> std::vector<int> {
    const auto* v = root.Find(key);
    std::vector<int> out;
    if (v && v->type == bahnbrett::JsonValue::Type::kArray) {
      for (const auto& item : v->array) {
        if (item.type == bahnbrett::JsonValue::Type::kNumber) {
          out.push_back(static_cast<int>(item.number_value));
        }
      }
    }
    return out;
  };

  config.station_names = getStrVec("station_names");
  if (config.station_names.empty()) {
    config.station_names = bahnbrett::DefaultDashboardConfig().station_names;
  }
  config.grid_cols = getInt("grid_cols", 0);
  config.grid_rows = getInt("grid_rows", 0);
  config.col_width_ratios = getIntVec("col_width_ratios");
  config.row_height_ratios = getIntVec("row_height_ratios");
  config.max_rows_per_widget = getInt("max_rows_per_widget", 10);
  config.max_departures_per_group = getInt("max_departures_per_group", 3);
  config.aggregate_by_destination = getBool("aggregate_by_destination", true);
  config.countdown_minutes = getInt("countdown_minutes", 15);
  config.min_departure_minutes = getInt("min_departure_minutes", 0);
  config.max_departure_minutes = getInt("max_departure_minutes", 180);
  config.refresh_seconds = getInt("refresh_seconds", 900);
  config.min_poll_seconds = std::max(30, getInt("min_poll_seconds", 60));
  config.header_font_scale = getFloat("header_font_scale", 1.0f);
  config.widget_font_scale = getFloat("widget_font_scale", 1.0f);
  config.show_delays = getBool("show_delays", true);
  config.timezone = getString("timezone", "CET-1CEST,M3.5.0,M10.5.0/3");
  config.home_city = getString("home_city", "Berlin");
  config.line_filters = getStrVec("line_filters");
  config.route_filters = getStrVec("route_filters");
  config.direction_filters = getStrVec("direction_filters");
  config.mode_filters = getStrVec("mode_filters");
  // An unknown provider name selects the default provider; see
  // ParseProviderKind() in config.h.
  config.provider = bahnbrett::ProviderKindName(
      bahnbrett::ParseProviderKind(getString("provider", "transitous")));
  config.max_departures_to_fetch_per_widget = getInt("max_departures_to_fetch_per_widget", 50);
  config.api_base_url = getString("api_base_url", bahnbrett::kDefaultApiBaseUrl);
  if (config.api_base_url.empty()) {
    config.api_base_url = bahnbrett::kDefaultApiBaseUrl;
  }
  config.transitous_base_url = getString("transitous_base_url", "");
  config.hvv_base_url = getString("hvv_base_url", "");
  config.hvv_user = getString("hvv_user", "");
  config.hvv_key = getString("hvv_key", "");

  // Use the free function to persist. The NVS store logs the exact key
  // that fails, so this message names the general cause only.
  if (!bahnbrett::SaveConfigToNvs(config)) {
    *error = "The device cannot store the config because a value is too large for NVS. Remove stations or filters and save the config again.";
    return false;
  }

  // Save extra fields not in DashboardConfig
  std::string remote_url = getString("remote_config_url", "");
  int remote_interval = getInt("remote_config_interval", 1);

  Preferences prefs;
  if (prefs.begin("bahnbrett", false)) {
    if (!remote_url.empty()) {
      prefs.putString("remote_url", remote_url.c_str());
    } else {
      prefs.remove("remote_url");
    }
    prefs.putInt("remote_intv", remote_interval);
    prefs.end();
  }

  return true;
}

// Flag to signal that config was saved (set by save endpoint, read by main loop).
volatile bool g_config_saved = false;
volatile bool g_config_cancelled = false;

// Body buffer for the save endpoint. The web server delivers the body in
// TCP-sized chunks; the upload callback appends each chunk and the request
// callback parses the complete body exactly once; see ADR 0042.
constexpr size_t kMaxSaveBodyBytes = 8 * 1024;
std::string g_save_body;
bool g_save_body_overflow = false;

}  // namespace

// ---------------------------------------------------------------------------
// Public entrypoint
// ---------------------------------------------------------------------------

bool bahnbrett_config_web_start(void) {
  Log_info("[bahnbrett_config] Starting config mode");

  g_config_saved = false;
  g_config_cancelled = false;

  // Set up AP
  WiFi.mode(WIFI_MODE_AP);
  const IPAddress subnetMask(255, 255, 255, 0);
  const IPAddress localIP(10, 254, 0, 1);
  const IPAddress gatewayIP(10, 254, 0, 1);

  WiFi.disconnect();
  delay(50);
  WiFi.softAPConfig(localIP, gatewayIP, subnetMask);
  delay(50);
  WiFi.softAP("Bahnbrett-Config", NULL, 1, 0, 4);
  delay(200);

  // Disable AMPDU RX (fixes Android compatibility)
  esp_wifi_stop();
  esp_wifi_deinit();
  wifi_init_config_t my_config = WIFI_INIT_CONFIG_DEFAULT();
  my_config.ampdu_rx_enable = false;
  esp_wifi_init(&my_config);
  esp_wifi_start();
  vTaskDelay(100 / portTICK_PERIOD_MS);

  Log_info("[bahnbrett_config] AP started: Bahnbrett-Config at %s", localIP.toString().c_str());

  // Captive portal: DNS server that resolves everything to 10.254.0.1.
  DNSServer dnsServer;
  dnsServer.setTTL(3600);
  dnsServer.start(53, "*", localIP);

  // Set up web server
  AsyncWebServer server(80);

  // GET / — serve the config editor page with embedded current config
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
    std::string config_json = EscapeForScriptEmbed(CurrentConfigJson());
    std::string page = kConfigPageHtml;
    // Replace placeholder with the escaped JSON
    const char* placeholder = "CONFIG_JSON_PLACEHOLDER";
    size_t pos = page.find(placeholder);
    if (pos != std::string::npos) {
      page.replace(pos, strlen(placeholder), config_json);
    }
    request->send(200, "text/html", page.c_str());
  });

  // GET /api/config/current — return current config as JSON
  server.on("/api/config/current", HTTP_GET, [](AsyncWebServerRequest* request) {
    std::string json = CurrentConfigJson();
    request->send(200, "application/json", json.c_str());
  });

  // POST /api/config/save — save config and signal reboot
  // The web server delivers the body in TCP-sized chunks. The upload
  // callback only buffers the chunks; the request callback runs once the
  // body is complete and parses the JSON exactly once; see ADR 0042.
  server.on("/api/config/save", HTTP_POST,
    [](AsyncWebServerRequest* request) {
      if (g_save_body_overflow) {
        g_save_body_overflow = false;
        request->send(400, "text/plain",
          "The config body exceeds 8192 bytes. Remove stations or filters and save the config again.");
        return;
      }
      // The body carries the HVV secret, so the log records the length only.
      Log_info("[bahnbrett_config] The save request carries %u bytes.",
               (unsigned)g_save_body.size());
      std::string error;
      if (SaveConfigFromJson(g_save_body, &error)) {
        g_config_saved = true;
        request->redirect("/saved");
      } else {
        request->send(400, "text/plain", error.c_str());
      }
      g_save_body.clear();
      g_save_body.shrink_to_fit();
    },
    nullptr,
    [](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
      (void)request;
      (void)total;
      if (index == 0) {
        g_save_body.clear();
        g_save_body_overflow = false;
      }
      if (g_save_body_overflow) {
        return;
      }
      if (g_save_body.size() + len > kMaxSaveBodyBytes) {
        g_save_body_overflow = true;
        g_save_body.clear();
        g_save_body.shrink_to_fit();
        return;
      }
      g_save_body.append(reinterpret_cast<const char*>(data), len);
    });

  // POST /api/config/cancel — cancel and signal sleep
  server.on("/api/config/cancel", HTTP_POST,
    [](AsyncWebServerRequest* request) {
      g_config_cancelled = true;
      request->send(200, "text/plain", "OK");
    });

  // GET /saved — confirmation page after successful save
  server.on("/saved", HTTP_GET, [](AsyncWebServerRequest* request) {
    request->send(200, "text/html", R"=====(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Config Saved</title>
<style>
  body{font-family:-apple-system,sans-serif;margin:0;padding:32px;background:#1a1a2e;
       color:#e0e0e0;text-align:center}
  h1{color:#4caf50}
  p{font-size:16px;margin:16px 0}
</style>
</head>
<body>
<h1>Config saved!</h1>
<p>The device will now reboot and apply the new settings.</p>
<p>Reconnect to your regular WiFi network.</p>
</body>
</html>
)=====");
  });

  server.begin();
  Log_info("[bahnbrett_config] Web server started");

  // Main loop: wait for save/cancel or 5-minute timeout.
  // Process DNS requests for captive portal behaviour.
  unsigned long start_ms = millis();
  const unsigned long kTimeoutMs = 5 * 60 * 1000;

  while (!g_config_saved && !g_config_cancelled && (millis() - start_ms < kTimeoutMs)) {
    dnsServer.processNextRequest();
    delay(10);
  }

  // Shut down
  dnsServer.stop();
  server.end();
  WiFi.softAPdisconnect(true);
  delay(100);
  WiFi.mode(WIFI_OFF);

  if (g_config_saved) {
    Log_info("[bahnbrett_config] Config saved, will reboot to normal mode");
    return true;
  } else {
    Log_info("[bahnbrett_config] Config cancelled or timed out, will deep sleep");
    return false;
  }
}

#endif  // BAHNBRETT_LOCAL_RENDER
