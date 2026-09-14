#include "portal.h"
#include <WiFi.h>

static WebServer server(80);
static DNSServer dnsServer;
static bool apModeActive = false;
static bool serverStarted = false;
static AppConfig *activeConfig = nullptr;
bool Portal::pendingLiveRefresh = false;

static const char PORTAL_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Allsky & Radar View Setup</title>
<link rel="stylesheet" href="https://unpkg.com/leaflet@1.9.4/dist/leaflet.css" />
<script src="https://unpkg.com/leaflet@1.9.4/dist/leaflet.js"></script>
<style>
  :root {
    --bg: #0b111a;
    --card: #131c2a;
    --border: #1f3047;
    --primary: #00d2ff;
    --accent: #00f2a0;
    --text: #e6f1ff;
    --text-muted: #7e9bb6;
    --danger: #ff4757;
  }
  * { box-sizing: border-box; margin: 0; padding: 0; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; }
  body { background: var(--bg); color: var(--text); padding: 16px; min-height: 100vh; }
  .container { max-width: 540px; margin: 0 auto; }
  .header { text-align: center; margin-bottom: 20px; padding: 12px 0; }
  .header h1 { font-size: 24px; color: var(--primary); letter-spacing: 1px; }
  .header p { color: var(--text-muted); font-size: 13px; margin-top: 4px; }
  .badge { display: inline-block; background: rgba(0,210,255,0.15); border: 1px solid var(--primary); color: var(--primary); padding: 2px 10px; border-radius: 12px; font-size: 11px; margin-top: 6px; }
  
  .card { background: var(--card); border: 1px solid var(--border); border-radius: 12px; padding: 18px; margin-bottom: 16px; box-shadow: 0 4px 16px rgba(0,0,0,0.4); }
  .card-title { font-size: 15px; font-weight: 600; color: var(--primary); margin-bottom: 14px; display: flex; align-items: center; justify-content: space-between; }
  
  .form-group { margin-bottom: 14px; }
  label { display: block; font-size: 12px; font-weight: 600; color: var(--text-muted); text-transform: uppercase; letter-spacing: 0.5px; margin-bottom: 6px; }
  input[type="text"], input[type="password"], input[type="number"], select {
    width: 100%; padding: 10px 12px; background: #080d14; border: 1px solid var(--border); border-radius: 8px; color: var(--text); font-size: 14px; outline: none; transition: border 0.2s;
  }
  input:focus, select:focus { border-color: var(--primary); }
  .row { display: flex; gap: 10px; }
  .row > div { flex: 1; }
  
  .btn { display: inline-flex; align-items: center; justify-content: center; width: 100%; padding: 12px; font-size: 14px; font-weight: 600; border-radius: 8px; cursor: pointer; border: none; transition: all 0.2s; }
  .btn-primary { background: linear-gradient(135deg, #00d2ff, #0088ff); color: #000; margin-top: 8px; }
  .btn-primary:hover { opacity: 0.9; }
  .btn-secondary { background: #1c2a3e; color: var(--text); border: 1px solid var(--border); font-size: 12px; padding: 8px; margin-bottom: 8px; }
  .btn-secondary:hover { background: #253952; }
  .btn-danger { background: rgba(255,71,87,0.15); color: var(--danger); border: 1px solid var(--danger); margin-top: 8px; font-size: 13px; }
  
  .presets { display: flex; flex-wrap: wrap; gap: 6px; margin-top: 8px; }
  .preset-btn { background: #0b1420; border: 1px solid var(--border); color: var(--text-muted); font-size: 11px; padding: 4px 8px; border-radius: 6px; cursor: pointer; }
  .preset-btn:hover { border-color: var(--primary); color: var(--primary); }

  .checkbox-group { display: flex; align-items: center; gap: 10px; margin-top: 10px; }
  .checkbox-group input { width: 18px; height: 18px; accent-color: var(--primary); cursor: pointer; }
  .checkbox-group label { margin-bottom: 0; text-transform: none; font-size: 13px; color: var(--text); cursor: pointer; }
  
  .range-container { display: flex; align-items: center; gap: 10px; }
  .range-container input { flex: 1; accent-color: var(--primary); }
  .range-val { font-size: 13px; color: var(--primary); min-width: 32px; text-align: right; }

  #map { background: #080d14; }
  .leaflet-container { background: #0b111a; }
</style>
</head>
<body>
<div class="container">
  <div class="header">
    <h1>ALLSKY & RADAR VIEW</h1>
    <p>Unified Dual/Single Display Weather & Sky Scope</p>
    <div class="badge">v1.0.1 &bull; Dual GC9B72 360x360 &bull; MQTT &bull; RainViewer</div>
  </div>

  <form action="/save" method="POST">
    <!-- Display & Operating Mode -->
    <div class="card" style="border-color:var(--accent);">
      <div class="card-title" style="color:var(--accent);">🖥️ Display & Operating Mode</div>
      <div class="form-group">
        <label>Hardware Configuration & Screen Routing</label>
        <div style="display:flex; gap:8px; align-items:center;">
          <select name="mode" id="mode" onchange="updateModeUI()" style="flex:1;">
            <option value="0" {{MODE_0}}>✨ Dual Screens (Screen 1: Allsky, Screen 2: Radar)</option>
            <option value="1" {{MODE_1}}>🌧️ Single Screen: Radar Only</option>
            <option value="2" {{MODE_2}}>🌌 Single Screen: Allsky Camera Only</option>
            <option value="3" {{MODE_3}}>🔄 Single Screen: Timed Carousel (Allsky & Radar)</option>
          </select>
          <button type="button" id="btn-apply-mode" class="btn" style="width:auto; padding:10px 16px; margin:0; white-space:nowrap; background:linear-gradient(135deg, #00f2a0, #00b0ff); color:#000; font-weight:700;" onclick="applyModeQuick()">⚡ Switch</button>
        </div>
        <div id="mode-status" style="font-size:12px; color:#00f2a0; margin-top:6px; min-height:16px; text-align:right;"></div>
      </div>
      <div class="form-group" id="carousel-group" style="display:none;">
        <label>Carousel Switch Interval (Seconds)</label>
        <input type="number" name="car_int" id="car_int" min="5" max="300" value="{{CAR_INT}}">
      </div>
    </div>

    <!-- Wi-Fi Configuration -->
    <div class="card">
      <div class="card-title">Wi-Fi Connection</div>
      <button type="button" class="btn btn-secondary" onclick="scanWifi()">Scan Wi-Fi Networks</button>
      <div id="wifi-scan-list"></div>
      <div class="form-group">
        <label>Wi-Fi SSID</label>
        <input type="text" name="ssid" id="ssid" value="{{SSID}}" required placeholder="Your Wi-Fi SSID">
      </div>
      <div class="form-group">
        <label>Password</label>
        <input type="password" name="pass" id="pass" value="{{PASS}}" placeholder="Wi-Fi Password">
      </div>
    </div>

    <!-- Allsky Camera MQTT Configuration -->
    <div class="card" id="allsky-card">
      <div class="card-title">🌌 Allsky Camera (MQTT Stream)</div>
      <div class="row">
        <div class="form-group" style="flex:2;">
          <label>MQTT Broker IP / Host</label>
          <input type="text" name="mq_host" value="{{MQ_HOST}}" placeholder="e.g. 192.168.0.6">
        </div>
        <div class="form-group" style="flex:1;">
          <label>Port</label>
          <input type="number" name="mq_port" value="{{MQ_PORT}}" placeholder="1883">
        </div>
      </div>
      <div class="form-group">
        <label>MQTT Image Topic</label>
        <input type="text" name="mq_topic" value="{{MQ_TOPIC}}" placeholder="indi-allsky/thumbnail">
      </div>
      <div class="row">
        <div class="form-group">
          <label>MQTT User (Optional)</label>
          <input type="text" name="mq_user" value="{{MQ_USER}}">
        </div>
        <div class="form-group">
          <label>MQTT Password (Optional)</label>
          <input type="password" name="mq_pass" value="{{MQ_PASS}}">
        </div>
      </div>
    </div>

    <!-- Data Product & Radar Layer Selection -->
    <div class="card" id="radar-layer-card">
      <div class="card-title">🌧️ Radar & Cloud Data Product</div>
      <div class="form-group">
        <label>Layer Source</label>
        <div style="display:flex; gap:8px; align-items:center;">
          <select name="product" id="product" style="flex:1;">
            <option value="0" {{PROD_0}}>🌧️ RainViewer Precipitation Radar (Default - No Key)</option>
            <option value="1" {{PROD_1}}>☁️ OpenWeatherMap Satellite Cloud Cover (Requires API Key)</option>
          </select>
          <button type="button" id="btn-apply-layer" class="btn" style="width:auto; padding:10px 16px; margin:0; white-space:nowrap; background:linear-gradient(135deg, #00d2ff, #0077ff); color:#fff; font-weight:700;" onclick="applyLayerQuick()">⚡ Switch</button>
        </div>
        <div id="layer-status" style="font-size:12px; color:#00f2a0; margin-top:6px; min-height:16px; text-align:right;"></div>
      </div>
      <div class="form-group" style="margin-top:10px;">
        <label>OpenWeatherMap API Key (Free from openweathermap.org)</label>
        <input type="password" name="owm_key" id="owm_key" value="{{OWM_KEY}}" placeholder="Enter 32-character OWM API Key">
      </div>
    </div>

    <!-- Geolocation & Scope Location -->
    <div class="card" id="radar-geo-card">
      <div class="card-title">📍 Radar Center Location</div>
      <div class="form-group">
        <label>Location Label</label>
        <input type="text" name="loc" id="loc" value="{{LOC}}" placeholder="e.g. London, UK" maxlength="32">
      </div>
      <div class="row">
        <div class="form-group">
          <label>Latitude</label>
          <input type="number" step="0.0001" name="lat" id="lat" value="{{LAT}}" required>
        </div>
        <div class="form-group">
          <label>Longitude</label>
          <input type="number" step="0.0001" name="lon" id="lon" value="{{LON}}" required>
        </div>
      </div>
      <div class="row" style="margin-top:4px;">
        <button type="button" class="btn btn-secondary" onclick="detectGPS()">📍 Use GPS</button>
        <button type="button" class="btn btn-secondary" onclick="toggleMap()" style="border-color:var(--primary); color:var(--primary);">🗺️ Leaflet Map Picker</button>
      </div>

      <!-- Leaflet Interactive Map Container -->
      <div id="map-container" style="display:none; margin-top:10px;">
        <div id="map" style="height:280px; width:100%; border-radius:8px; border:1px solid var(--border);"></div>
        <p style="font-size:11px; color:var(--text-muted); margin-top:6px; text-align:center;">Click map or drag the pin to set center. Blue circle = radar scope area.</p>
      </div>

      <label style="margin-top:10px;">Quick Presets</label>
      <div class="presets">
        <button type="button" class="preset-btn" onclick="setLoc('London, UK', 51.5074, -0.1278)">London</button>
        <button type="button" class="preset-btn" onclick="setLoc('New York, US', 40.7128, -74.0060)">New York</button>
        <button type="button" class="preset-btn" onclick="setLoc('Paris, FR', 48.8566, 2.3522)">Paris</button>
        <button type="button" class="preset-btn" onclick="setLoc('Berlin, DE', 52.5200, 13.4050)">Berlin</button>
        <button type="button" class="preset-btn" onclick="setLoc('Tokyo, JP', 35.6762, 139.6503)">Tokyo</button>
        <button type="button" class="preset-btn" onclick="setLoc('Sydney, AU', -33.8688, 151.2093)">Sydney</button>
        <button type="button" class="preset-btn" onclick="setLoc('Los Angeles, US', 34.0522, -118.2437)">Los Angeles</button>
        <button type="button" class="preset-btn" onclick="setLoc('Chicago, US', 41.8781, -87.6298)">Chicago</button>
      </div>
    </div>

    <!-- Live Weather Telemetry -->
    <div class="card" id="radar-telem-card">
      <div class="card-title">Live Weather & Telemetry (Open-Meteo)</div>
      <div class="checkbox-group">
        <input type="checkbox" name="telem" id="telem" value="1" {{TELEM_CHECKED}}>
        <label for="telem">Show Outside Temp, Humidity & Dynamic Wind Vector</label>
      </div>
      <div class="form-group" style="margin-top:10px;">
        <label>Temperature & Speed Units</label>
        <select name="units">
          <option value="0" {{UNIT_0}}>Celsius (°C) & km/h</option>
          <option value="1" {{UNIT_1}}>Fahrenheit (°F) & mph</option>
        </select>
      </div>
    </div>

    <!-- Radar & Cloud Motion Animation -->
    <div class="card" id="radar-anim-card">
      <div class="card-title">Motion Loop & History Animation</div>
      <div class="form-group">
        <label>History Frames (Zero Heap LittleFS Stream)</label>
        <select name="aframes">
          <option value="5" {{AFRAME_5}}>5 Frames (~40 min history, Recommended)</option>
          <option value="3" {{AFRAME_3}}>3 Frames (~20 min history)</option>
          <option value="8" {{AFRAME_8}}>8 Frames (~1.2 hr history)</option>
          <option value="0" {{AFRAME_0}}>Disabled (Static latest frame only)</option>
        </select>
      </div>
      <div class="form-group">
        <label>Animation Frame Speed</label>
        <select name="aspeed">
          <option value="400" {{ASPEED_400}}>Fast (400 ms)</option>
          <option value="700" {{ASPEED_700}}>Normal (700 ms, Recommended)</option>
          <option value="1000" {{ASPEED_1000}}>Slow (1.0 s)</option>
        </select>
      </div>
      <div class="form-group">
        <label>Pause on Live Frame</label>
        <select name="adwell">
          <option value="2000" {{ADWELL_2000}}>2.0 Seconds</option>
          <option value="3500" {{ADWELL_3500}}>3.5 Seconds (Standard)</option>
          <option value="5000" {{ADWELL_5000}}>5.0 Seconds</option>
        </select>
      </div>
    </div>

    <!-- Radar Map Visuals -->
    <div class="card" id="radar-vis-card">
      <div class="card-title">Radar Visuals & Palettes</div>
      <div class="form-group">
        <label>Zoom Level (Radius)</label>
        <select name="zoom" id="zoom-select">
          <option value="4" {{ZOOM_4}}>Z4 - Regional (~200 km scope)</option>
          <option value="5" {{ZOOM_5}}>Z5 - Sub-Regional (~100 km scope)</option>
          <option value="6" {{ZOOM_6}}>Z6 - Metro Area (~50 km scope, Default)</option>
          <option value="7" {{ZOOM_7}}>Z7 - Local Focus (~25 km scope)</option>
        </select>
      </div>
      <div class="form-group">
        <label>Radar Color Palette (Rain Mode)</label>
        <select name="color">
          <option value="2" {{COL_2}}>Universal Blue (Recommended)</option>
          <option value="1" {{COL_1}}>Original RainViewer</option>
          <option value="3" {{COL_3}}>TITAN Weather</option>
          <option value="4" {{COL_4}}>The Weather Channel</option>
          <option value="5" {{COL_5}}>Meteored</option>
          <option value="6" {{COL_6}}>NEXRAD Level III</option>
          <option value="7" {{COL_7}}>Rainbow Palette</option>
          <option value="8" {{COL_8}}>Dark Sky</option>
          <option value="0" {{COL_0}}>Black & White</option>
        </select>
      </div>
      <div class="form-group">
        <label>Radar Refresh Interval</label>
        <select name="refresh">
          <option value="5" {{REF_5}}>Every 5 Minutes (Fastest)</option>
          <option value="10" {{REF_10}}>Every 10 Minutes (Standard)</option>
          <option value="15" {{REF_15}}>Every 15 Minutes</option>
          <option value="30" {{REF_30}}>Every 30 Minutes</option>
        </select>
      </div>
      <div class="checkbox-group">
        <input type="checkbox" name="smooth" id="smooth" value="1" {{SMOOTH_CHECKED}}>
        <label for="smooth">Smooth radar interpolation</label>
      </div>
      <div class="checkbox-group">
        <input type="checkbox" name="snow" id="snow" value="1" {{SNOW_CHECKED}}>
        <label for="snow">Show distinct snow/ice colors</label>
      </div>
    </div>

    <!-- Display & Overlays -->
    <div class="card">
      <div class="card-title">Display & Overlays</div>
      <div class="checkbox-group">
        <input type="checkbox" name="rings" id="rings" value="1" {{RINGS_CHECKED}}>
        <label for="rings">Show Tactical Range Rings & Cardinal Markers</label>
      </div>
      <div class="checkbox-group">
        <input type="checkbox" name="clock" id="clock" value="1" {{CLOCK_CHECKED}}>
        <label for="clock">Show Local Time & Capture Timestamp</label>
      </div>
      <div class="form-group" style="margin-top:14px;">
        <label>Display Brightness</label>
        <div class="range-container">
          <input type="range" name="brightness" min="20" max="255" value="{{BRIGHTNESS}}" oninput="document.getElementById('bval').innerText = this.value">
          <span class="range-val" id="bval">{{BRIGHTNESS}}</span>
        </div>
      </div>
      <div class="form-group">
        <label>Timezone (POSIX string)</label>
        <input type="text" name="tz" value="{{TZ}}" placeholder="GMT0BST,M3.5.0/1,M10.5.0">
      </div>
    </div>

    <button type="submit" class="btn btn-primary">Save & Apply Configuration</button>
  </form>

  <form action="/restart" method="POST" onsubmit="return confirm('Reboot device?')">
    <button type="submit" class="btn btn-danger">Reboot Device</button>
  </form>

  <div style="text-align:center; margin-top:24px; padding-bottom:16px; font-size:12px; color:var(--text-muted);">
    <a href="https://github.com/oldjiberjaber/AllskyRadarView" target="_blank" style="color:var(--primary); text-decoration:none; display:inline-flex; align-items:center; gap:6px;">
      <svg height="16" width="16" viewBox="0 0 16 16" fill="currentColor" style="vertical-align:middle;"><path d="M8 0C3.58 0 0 3.58 0 8c0 3.54 2.29 6.53 5.47 7.59.4.07.55-.17.55-.38 0-.19-.01-.82-.01-1.49-2.01.37-2.53-.49-2.69-.94-.09-.23-.48-.94-.82-1.13-.28-.15-.68-.52-.01-.53.63-.01 1.08.58 1.23.82.72 1.21 1.87.87 2.33.66.07-.52.28-.87.51-1.07-1.78-.2-3.64-.89-3.64-3.95 0-.87.31-1.59.82-2.15-.08-.2-.36-1.02.08-2.12 0 0 .67-.21 2.2.82.64-.18 1.32-.27 2-.27.68 0 1.36.09 2 .27 1.53-1.04 2.2-.82 2.2-.82.44 1.1.16 1.92.08 2.12.51.56.82 1.27.82 2.15 0 3.07-1.87 3.75-3.65 3.95.29.25.54.73.54 1.48 0 1.07-.01 1.93-.01 2.2 0 .21.15.46.55.38A8.013 8.013 0 0016 8c0-4.42-3.58-8-8-8z"></path></svg>
      GitHub: oldjiberjaber/AllskyRadarView (v1.0.0)
    </a>
  </div>
</div>

<script>
var map = null;
var marker = null;
var rangeCircle = null;

function updateModeUI() {
  var mode = parseInt(document.getElementById('mode').value);
  var carGroup = document.getElementById('carousel-group');
  if (carGroup) carGroup.style.display = (mode === 3) ? 'block' : 'none';
}
updateModeUI();

function getRadarRadiusMeters(zoom) {
  if (zoom <= 4) return 200000;
  if (zoom == 5) return 100000;
  if (zoom == 6) return 50000;
  return 25000;
}

function initOrUpdateMap() {
  var lat = parseFloat(document.getElementById('lat').value) || 51.5074;
  var lon = parseFloat(document.getElementById('lon').value) || -0.1278;
  var zoomSelect = document.getElementById('zoom-select');
  var zoomLevel = parseInt(zoomSelect ? zoomSelect.value : 6) || 6;
  var mapZoom = zoomLevel + 2;

  if (!map) {
    map = L.map('map').setView([lat, lon], mapZoom);
    L.tileLayer('https://{s}.basemaps.cartocdn.com/rastertiles/voyager/{z}/{x}/{y}{r}.png', {
      attribution: '&copy; OpenStreetMap &copy; CARTO',
      maxZoom: 18
    }).addTo(map);

    marker = L.marker([lat, lon], { draggable: true }).addTo(map);

    rangeCircle = L.circle([lat, lon], {
      radius: getRadarRadiusMeters(zoomLevel),
      color: '#00d2ff',
      fillColor: '#00d2ff',
      fillOpacity: 0.15,
      weight: 2,
      dashArray: '5, 5'
    }).addTo(map);

    marker.on('dragend', function(e) {
      var pos = marker.getLatLng();
      updateFromLatLng(pos.lat, pos.lng);
    });

    map.on('click', function(e) {
      marker.setLatLng(e.latlng);
      updateFromLatLng(e.latlng.lat, e.latlng.lng);
    });

    if (zoomSelect) {
      zoomSelect.addEventListener('change', function() {
        var newZ = parseInt(this.value) || 6;
        if (rangeCircle) rangeCircle.setRadius(getRadarRadiusMeters(newZ));
        if (map) map.setZoom(newZ + 2);
      });
    }
  } else {
    map.setView([lat, lon], mapZoom);
    marker.setLatLng([lat, lon]);
    rangeCircle.setLatLng([lat, lon]);
    rangeCircle.setRadius(getRadarRadiusMeters(zoomLevel));
    map.invalidateSize();
  }
}

function updateFromLatLng(lat, lng) {
  document.getElementById('lat').value = lat.toFixed(4);
  document.getElementById('lon').value = lng.toFixed(4);
  if (rangeCircle) rangeCircle.setLatLng([lat, lng]);

  fetch('https://nominatim.openstreetmap.org/reverse?format=json&lat=' + lat + '&lon=' + lng)
    .then(function(res) { return res.json(); })
    .then(function(data) {
      if (data && data.address) {
        var city = data.address.city || data.address.town || data.address.village || data.address.county || "Custom";
        var country = data.address.country_code ? data.address.country_code.toUpperCase() : "";
        document.getElementById('loc').value = (city + ', ' + country).substring(0, 32);
      }
    }).catch(function() {});
}

function toggleMap() {
  var container = document.getElementById('map-container');
  if (container.style.display === 'none') {
    container.style.display = 'block';
    setTimeout(initOrUpdateMap, 150);
  } else {
    container.style.display = 'none';
  }
}

function setLoc(name, lat, lon) {
  document.getElementById('loc').value = name;
  document.getElementById('lat').value = lat;
  document.getElementById('lon').value = lon;
  if (map) initOrUpdateMap();
}

function detectGPS() {
  if (navigator.geolocation) {
    navigator.geolocation.getCurrentPosition(function(pos) {
      var lat = pos.coords.latitude;
      var lon = pos.coords.longitude;
      document.getElementById('lat').value = lat.toFixed(4);
      document.getElementById('lon').value = lon.toFixed(4);
      document.getElementById('loc').value = "My Location";
      updateFromLatLng(lat, lon);
      if (map) initOrUpdateMap();
    }, function() { alert("Could not obtain GPS coordinates from browser."); });
  } else { alert("Geolocation not supported by browser."); }
}

function scanWifi() {
  var list = document.getElementById('wifi-scan-list');
  list.innerHTML = '<p style="font-size:12px; color:var(--primary); padding:6px 0;">Scanning networks...</p>';
  fetch('/scan').then(res => res.json()).then(data => {
    if (data.length === 0) { list.innerHTML = '<p style="font-size:12px; color:var(--text-muted)">No networks found.</p>'; return; }
    var html = '<div class="presets" style="margin-bottom:12px;">';
    data.forEach(net => {
      html += `<button type="button" class="preset-btn" onclick="document.getElementById('ssid').value='${net.ssid}'">${net.ssid} (${net.rssi}dBm)</button>`;
    });
    html += '</div>';
    list.innerHTML = html;
  }).catch(() => { list.innerHTML = '<p style="font-size:12px; color:var(--danger)">Scan failed.</p>'; });
}

function applyLayerQuick() {
  var prod = document.getElementById('product').value;
  var key = document.getElementById('owm_key') ? encodeURIComponent(document.getElementById('owm_key').value) : '';
  var btn = document.getElementById('btn-apply-layer');
  var status = document.getElementById('layer-status');
  btn.innerText = 'Switching...';
  btn.disabled = true;
  status.innerText = 'Updating radar display...';
  fetch('/switch-layer?product=' + prod + '&owm_key=' + key, { method: 'POST' })
    .then(r => r.json())
    .then(data => {
      btn.innerText = '✓ Applied';
      status.innerText = '✓ Switched to ' + (prod == '1' ? 'OWM Cloud Cover' : 'Precipitation Radar');
      setTimeout(() => {
        btn.innerText = '⚡ Switch';
        btn.disabled = false;
        status.innerText = '';
      }, 2500);
    })
    .catch(() => {
      btn.innerText = '⚡ Switch';
      btn.disabled = false;
      status.innerText = 'Error applying layer';
    });
}

function applyModeQuick() {
  var mode = document.getElementById('mode').value;
  var carIntInput = document.getElementById('car_int');
  var carInt = carIntInput ? carIntInput.value : 30;
  var btn = document.getElementById('btn-apply-mode');
  var status = document.getElementById('mode-status');
  btn.innerText = 'Switching...';
  btn.disabled = true;
  status.innerText = 'Updating display mode...';
  fetch('/switch-mode?mode=' + mode + '&car_int=' + carInt, { method: 'POST' })
    .then(r => r.json())
    .then(data => {
      btn.innerText = '✓ Applied';
      var modeNames = ['Dual Screens', 'Radar Only', 'Allsky Only', 'Timed Carousel'];
      status.innerText = '✓ Switched to ' + (modeNames[parseInt(mode)] || 'Selected Mode');
      setTimeout(() => {
        btn.innerText = '⚡ Switch';
        btn.disabled = false;
        status.innerText = '';
      }, 2500);
    })
    .catch(() => {
      btn.innerText = '⚡ Switch';
      btn.disabled = false;
      status.innerText = 'Error switching mode';
    });
}
</script>
</body>
</html>
)rawliteral";

void Portal::setupRoutes() {
    server.on("/", HTTP_GET, handleRoot);
    server.on("/save", HTTP_POST, handleSave);
    server.on("/switch-mode", HTTP_ANY, handleSwitchMode);
    server.on("/switch-layer", HTTP_ANY, handleSwitchLayer);
    server.on("/scan", HTTP_GET, handleScan);
    server.on("/restart", HTTP_POST, handleRestart);
    server.on("/favicon.ico", HTTP_GET, []() { server.send(204); });
    server.onNotFound(handleNotFound);
}

void Portal::startAP(AppConfig &currentConfig) {
    activeConfig = &currentConfig;
    apModeActive = true;

    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP("AllskyRadar-Setup", "");

    IPAddress apIP(192, 168, 4, 1);
    WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));

    dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
    dnsServer.start(53, "*", apIP);

    setupRoutes();
    server.begin();
    serverStarted = true;
    Serial.printf("[PORTAL] Captive Access Point started: SSID='AllskyRadar-Setup', IP=%s\n", WiFi.softAPIP().toString().c_str());
}

void Portal::startLocalServer(AppConfig &currentConfig) {
    activeConfig = &currentConfig;
    apModeActive = false;

    setupRoutes();
    server.begin();
    serverStarted = true;
    Serial.printf("[PORTAL] Local HTTP Server running at http://%s/\n", WiFi.localIP().toString().c_str());
}

void Portal::handle() {
    if (!serverStarted) return;
    if (apModeActive) {
        dnsServer.processNextRequest();
    }
    server.handleClient();
}

bool Portal::isAPMode() {
    return apModeActive;
}

bool Portal::isRunning() {
    return serverStarted;
}

void Portal::handleRoot() {
    if (!activeConfig) {
        server.send(500, "text/plain", "Configuration pointer missing");
        return;
    }

    String html = FPSTR(PORTAL_HTML);

    // Operating Mode
    html.replace("{{MODE_0}}", activeConfig->display_mode == MODE_DUAL_DISPLAY ? "selected" : "");
    html.replace("{{MODE_1}}", activeConfig->display_mode == MODE_SINGLE_RADAR ? "selected" : "");
    html.replace("{{MODE_2}}", activeConfig->display_mode == MODE_SINGLE_ALLSKY ? "selected" : "");
    html.replace("{{MODE_3}}", activeConfig->display_mode == MODE_SINGLE_CAROUSEL ? "selected" : "");
    html.replace("{{CAR_INT}}", String(activeConfig->carousel_interval_sec));

    // Wi-Fi
    html.replace("{{SSID}}", String(activeConfig->wifi_ssid));
    html.replace("{{PASS}}", String(activeConfig->wifi_pass));

    // Allsky MQTT
    html.replace("{{MQ_HOST}}", String(activeConfig->mqtt_host));
    html.replace("{{MQ_PORT}}", String(activeConfig->mqtt_port));
    html.replace("{{MQ_TOPIC}}", String(activeConfig->mqtt_topic));
    html.replace("{{MQ_USER}}", String(activeConfig->mqtt_user));
    html.replace("{{MQ_PASS}}", String(activeConfig->mqtt_pass));

    // Location & Radar
    html.replace("{{LOC}}", String(activeConfig->location_name));
    html.replace("{{LAT}}", String(activeConfig->latitude, 4));
    html.replace("{{LON}}", String(activeConfig->longitude, 4));

    // Data Product
    html.replace("{{PROD_0}}", activeConfig->data_product == 0 ? "selected" : "");
    html.replace("{{PROD_1}}", activeConfig->data_product == 1 ? "selected" : "");
    html.replace("{{OWM_KEY}}", String(activeConfig->owm_api_key));

    // Telemetry & Units
    html.replace("{{TELEM_CHECKED}}", activeConfig->show_telemetry ? "checked" : "");
    html.replace("{{UNIT_0}}", activeConfig->temp_units == 0 ? "selected" : "");
    html.replace("{{UNIT_1}}", activeConfig->temp_units == 1 ? "selected" : "");

    // Zoom
    html.replace("{{ZOOM_4}}", activeConfig->zoom == 4 ? "selected" : "");
    html.replace("{{ZOOM_5}}", activeConfig->zoom == 5 ? "selected" : "");
    html.replace("{{ZOOM_6}}", activeConfig->zoom == 6 ? "selected" : "");
    html.replace("{{ZOOM_7}}", activeConfig->zoom == 7 ? "selected" : "");

    // Colors
    for (int i = 0; i <= 8; i++) {
        char tag[16];
        snprintf(tag, sizeof(tag), "{{COL_%d}}", i);
        html.replace(tag, activeConfig->color_scheme == i ? "selected" : "");
    }

    // Motion Loop & History
    html.replace("{{AFRAME_0}}", activeConfig->anim_frames == 0 ? "selected" : "");
    html.replace("{{AFRAME_3}}", activeConfig->anim_frames == 3 ? "selected" : "");
    html.replace("{{AFRAME_5}}", activeConfig->anim_frames == 5 ? "selected" : "");
    html.replace("{{AFRAME_8}}", activeConfig->anim_frames == 8 ? "selected" : "");

    html.replace("{{ASPEED_400}}", activeConfig->anim_speed_ms == 400 ? "selected" : "");
    html.replace("{{ASPEED_700}}", activeConfig->anim_speed_ms == 700 ? "selected" : "");
    html.replace("{{ASPEED_1000}}", activeConfig->anim_speed_ms == 1000 ? "selected" : "");

    html.replace("{{ADWELL_2000}}", activeConfig->anim_dwell_ms == 2000 ? "selected" : "");
    html.replace("{{ADWELL_3500}}", activeConfig->anim_dwell_ms == 3500 ? "selected" : "");
    html.replace("{{ADWELL_5000}}", activeConfig->anim_dwell_ms == 5000 ? "selected" : "");

    // Refresh intervals
    html.replace("{{REF_5}}", activeConfig->refresh_interval_min == 5 ? "selected" : "");
    html.replace("{{REF_10}}", activeConfig->refresh_interval_min == 10 ? "selected" : "");
    html.replace("{{REF_15}}", activeConfig->refresh_interval_min == 15 ? "selected" : "");
    html.replace("{{REF_30}}", activeConfig->refresh_interval_min == 30 ? "selected" : "");

    // Checkboxes
    html.replace("{{SMOOTH_CHECKED}}", activeConfig->smooth ? "checked" : "");
    html.replace("{{SNOW_CHECKED}}", activeConfig->snow ? "checked" : "");
    html.replace("{{RINGS_CHECKED}}", activeConfig->show_range_rings ? "checked" : "");
    html.replace("{{CLOCK_CHECKED}}", activeConfig->show_clock ? "checked" : "");

    // Display & System
    html.replace("{{BRIGHTNESS}}", String(activeConfig->brightness));
    html.replace("{{TZ}}", String(activeConfig->timezone));

    server.send(200, "text/html", html);
}

void Portal::handleSave() {
    if (!activeConfig) return;

    char oldSsid[33];
    char oldPass[65];
    strncpy(oldSsid, activeConfig->wifi_ssid, sizeof(oldSsid) - 1);
    strncpy(oldPass, activeConfig->wifi_pass, sizeof(oldPass) - 1);
    oldSsid[sizeof(oldSsid) - 1] = '\0';
    oldPass[sizeof(oldPass) - 1] = '\0';

    if (server.hasArg("mode")) activeConfig->display_mode = server.arg("mode").toInt();
    if (server.hasArg("car_int")) activeConfig->carousel_interval_sec = server.arg("car_int").toInt();

    if (server.hasArg("ssid")) strncpy(activeConfig->wifi_ssid, server.arg("ssid").c_str(), sizeof(activeConfig->wifi_ssid) - 1);
    if (server.hasArg("pass")) strncpy(activeConfig->wifi_pass, server.arg("pass").c_str(), sizeof(activeConfig->wifi_pass) - 1);

    if (server.hasArg("mq_host")) strncpy(activeConfig->mqtt_host, server.arg("mq_host").c_str(), sizeof(activeConfig->mqtt_host) - 1);
    if (server.hasArg("mq_port")) activeConfig->mqtt_port = server.arg("mq_port").toInt();
    if (server.hasArg("mq_topic")) strncpy(activeConfig->mqtt_topic, server.arg("mq_topic").c_str(), sizeof(activeConfig->mqtt_topic) - 1);
    if (server.hasArg("mq_user")) strncpy(activeConfig->mqtt_user, server.arg("mq_user").c_str(), sizeof(activeConfig->mqtt_user) - 1);
    if (server.hasArg("mq_pass")) strncpy(activeConfig->mqtt_pass, server.arg("mq_pass").c_str(), sizeof(activeConfig->mqtt_pass) - 1);

    if (server.hasArg("loc")) strncpy(activeConfig->location_name, server.arg("loc").c_str(), sizeof(activeConfig->location_name) - 1);
    if (server.hasArg("product")) activeConfig->data_product = server.arg("product").toInt();
    if (server.hasArg("owm_key")) strncpy(activeConfig->owm_api_key, server.arg("owm_key").c_str(), sizeof(activeConfig->owm_api_key) - 1);
    if (server.hasArg("units")) activeConfig->temp_units = server.arg("units").toInt();
    activeConfig->show_telemetry = server.hasArg("telem") ? 1 : 0;

    if (server.hasArg("lat")) activeConfig->latitude = server.arg("lat").toFloat();
    if (server.hasArg("lon")) activeConfig->longitude = server.arg("lon").toFloat();
    if (server.hasArg("zoom")) activeConfig->zoom = server.arg("zoom").toInt();
    if (server.hasArg("color")) activeConfig->color_scheme = server.arg("color").toInt();
    if (server.hasArg("refresh")) activeConfig->refresh_interval_min = server.arg("refresh").toInt();

    if (server.hasArg("aframes")) activeConfig->anim_frames = server.arg("aframes").toInt();
    if (server.hasArg("aspeed")) activeConfig->anim_speed_ms = server.arg("aspeed").toInt();
    if (server.hasArg("adwell")) activeConfig->anim_dwell_ms = server.arg("adwell").toInt();

    activeConfig->smooth = server.hasArg("smooth") ? 1 : 0;
    activeConfig->snow = server.hasArg("snow") ? 1 : 0;
    activeConfig->show_range_rings = server.hasArg("rings") ? 1 : 0;
    activeConfig->show_clock = server.hasArg("clock") ? 1 : 0;

    if (server.hasArg("brightness")) activeConfig->brightness = server.arg("brightness").toInt();
    if (server.hasArg("tz")) strncpy(activeConfig->timezone, server.arg("tz").c_str(), sizeof(activeConfig->timezone) - 1);

    ConfigManager::save(*activeConfig);

    bool wifiChanged = (strcmp(oldSsid, activeConfig->wifi_ssid) != 0) || (strcmp(oldPass, activeConfig->wifi_pass) != 0);

    if (wifiChanged || apModeActive) {
        String resp = "<html><body style='background:#0b111a;color:#00d2ff;font-family:sans-serif;text-align:center;padding-top:50px;'>"
                      "<h2>Wi-Fi Updated!</h2><p style='color:#e6f1ff;margin-top:10px;'>Rebooting device to connect to new network...</p>"
                      "<script>setTimeout(function(){ window.location.href='/'; }, 4000);</script></body></html>";
        server.send(200, "text/html", resp);
        delay(1000);
        ESP.restart();
    } else {
        setenv("TZ", activeConfig->timezone, 1);
        tzset();
        pendingLiveRefresh = true;

        String resp = "<html><body style='background:#0b111a;color:#00d2ff;font-family:sans-serif;text-align:center;padding-top:50px;'>"
                      "<h2>Settings Applied!</h2><p style='color:#00f2a0;font-size:18px;margin-top:10px;'>Updating display live (No reboot required)...</p>"
                      "<script>setTimeout(function(){ window.location.href='/'; }, 1500);</script></body></html>";
        server.send(200, "text/html", resp);
    }
}

bool Portal::isLiveRefreshRequested() {
    bool r = pendingLiveRefresh;
    pendingLiveRefresh = false;
    return r;
}

void Portal::handleSwitchMode() {
    if (!activeConfig) {
        server.send(500, "application/json", "{\"status\":\"error\"}");
        return;
    }
    if (server.hasArg("mode")) {
        activeConfig->display_mode = server.arg("mode").toInt();
        if (server.hasArg("car_int")) {
            activeConfig->carousel_interval_sec = server.arg("car_int").toInt();
        }
        ConfigManager::save(*activeConfig);
        pendingLiveRefresh = true;
        const char* modeNames[] = {"Dual Screens", "Radar Only", "Allsky Only", "Timed Carousel"};
        int m = activeConfig->display_mode;
        const char* name = (m >= 0 && m <= 3) ? modeNames[m] : "Unknown";
        Serial.printf("[PORTAL] Quick switched display mode to: %d (%s)\n", m, name);
    }
    server.send(200, "application/json", "{\"status\":\"ok\",\"mode\":" + String(activeConfig->display_mode) + "}");
}

void Portal::handleSwitchLayer() {
    if (!activeConfig) {
        server.send(500, "application/json", "{\"status\":\"error\"}");
        return;
    }
    if (server.hasArg("product")) {
        activeConfig->data_product = server.arg("product").toInt();
    }
    if (server.hasArg("owm_key")) {
        strncpy(activeConfig->owm_api_key, server.arg("owm_key").c_str(), sizeof(activeConfig->owm_api_key) - 1);
    }
    ConfigManager::save(*activeConfig);
    pendingLiveRefresh = true;
    Serial.printf("[PORTAL] Quick switched layer source to: %s\n", 
        activeConfig->data_product == 1 ? "OPENWEATHERMAP CLOUD" : "PRECIPITATION RADAR");
    server.send(200, "application/json", "{\"status\":\"ok\",\"product\":" + String(activeConfig->data_product) + "}");
}

void Portal::handleScan() {
    int n = WiFi.scanNetworks();
    String json = "[";
    for (int i = 0; i < n; ++i) {
        if (i > 0) json += ",";
        json += "{\"ssid\":\"" + WiFi.SSID(i) + "\",\"rssi\":" + String(WiFi.RSSI(i)) + "}";
    }
    json += "]";
    server.send(200, "application/json", json);
}

void Portal::handleRestart() {
    server.send(200, "text/html", "<html><body style='background:#0b111a;color:#00d2ff;font-family:sans-serif;text-align:center;padding-top:50px;'><h2>Rebooting...</h2></body></html>");
    delay(1000);
    ESP.restart();
}

void Portal::handleNotFound() {
    if (apModeActive) {
        server.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/", true);
        server.send(302, "text/plain", "");
    } else {
        server.send(404, "text/plain", "Not Found");
    }
}

