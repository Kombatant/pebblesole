// Pebblesole — PebbleKit JS: location + Open-Meteo weather + Clay config.

var Clay = require('pebble-clay');
var clayConfig = require('./config');
// autoHandleEvents off: showConfiguration is handled manually so the page can
// be filled with fresh battery stats from the watch before it opens.
var clay = new Clay(clayConfig, null, { autoHandleEvents: false });

// Icon enum — MUST match the C enum in main.c.
var ICON = {
  CLEAR: 0,
  CLOUDS: 1,
  RAIN: 2,
  SNOW: 3,
  THUNDER: 4,
  FOG: 5
};

// Map WMO weather_code -> our icon id.
// https://open-meteo.com/en/docs  (WMO Weather interpretation codes)
function wmoToIcon(code) {
  if (code === 0) return ICON.CLEAR;                 // clear sky
  if (code === 1 || code === 2 || code === 3) return ICON.CLOUDS; // mainly clear/partly/overcast
  if (code === 45 || code === 48) return ICON.FOG;   // fog
  if (code >= 51 && code <= 57) return ICON.RAIN;    // drizzle
  if (code >= 61 && code <= 67) return ICON.RAIN;    // rain
  if (code >= 71 && code <= 77) return ICON.SNOW;    // snow
  if (code >= 80 && code <= 82) return ICON.RAIN;    // rain showers
  if (code === 85 || code === 86) return ICON.SNOW;  // snow showers
  if (code >= 95 && code <= 99) return ICON.THUNDER; // thunderstorm
  return ICON.CLOUDS;
}

function getTempUnits() {
  // Clay stores under key TEMP_UNITS; value 'C' or 'F'. Default C.
  var v = localStorage.getItem('TEMP_UNITS');
  return v === 'F' ? 1 : 0;
}

function getWeatherIntervalMin() {
  var v = parseInt(localStorage.getItem('weatherInterval') || '30', 10);
  return (v > 0) ? v : 30;
}

function sendToWatch(tempC, iconId) {
  Pebble.sendAppMessage(
    {
      TEMP: Math.round(tempC),       // always Celsius; watch converts for display
      ICON: iconId,
      TEMP_UNITS: getTempUnits()
    },
    function () {
      localStorage.setItem('lastWeatherFetch', String(Date.now()));
      console.log('Pebblesole: weather sent');
    },
    function (e) { console.log('Pebblesole: send failed ' + JSON.stringify(e)); }
  );
}

function fetchWeather(lat, lon) {
  var url = 'https://api.open-meteo.com/v1/forecast?latitude=' + lat +
            '&longitude=' + lon + '&current=temperature_2m,weather_code';
  var req = new XMLHttpRequest();
  req.open('GET', url, true);
  req.onload = function () {
    if (req.status !== 200) {
      console.log('Pebblesole: open-meteo HTTP ' + req.status);
      return;
    }
    try {
      var data = JSON.parse(req.responseText);
      var tempC = data.current.temperature_2m;
      var code = data.current.weather_code;
      sendToWatch(tempC, wmoToIcon(code));
    } catch (e) {
      console.log('Pebblesole: parse error ' + e);
    }
  };
  req.onerror = function () { console.log('Pebblesole: open-meteo request error'); };
  req.send();
}

function locationSuccess(pos) {
  fetchWeather(pos.coords.latitude, pos.coords.longitude);
}

function locationError(err) {
  console.log('Pebblesole: location error ' + err.code + ' ' + err.message);
}

function refresh() {
  navigator.geolocation.getCurrentPosition(locationSuccess, locationError, {
    timeout: 15000,
    maximumAge: 60000
  });
}

Pebble.addEventListener('ready', function () {
  console.log('Pebblesole: PKJS ready');
  // The JS environment restarts often (phone app relaunch, BT reconnect);
  // an unconditional fetch here would push off-schedule weather updates that
  // wake the watch. Fetch only if the last successful send is older than the
  // configured refresh interval. Watch-driven REQUEST_WEATHER always fetches.
  var lastFetch = parseInt(localStorage.getItem('lastWeatherFetch') || '0', 10);
  if (Date.now() - lastFetch >= getWeatherIntervalMin() * 60 * 1000) {
    refresh();
  }
});

// The watch drives the periodic refresh schedule (REQUEST_WEATHER on its
// minute tick, honoring the configured interval), so no setInterval here.
Pebble.addEventListener('appmessage', function (e) {
  if (e.payload && e.payload.REQUEST_WEATHER !== undefined) {
    refresh();
  }
  if (e.payload && typeof e.payload.BATTERY_ESTIMATE !== 'undefined') {
    localStorage.setItem('batteryEstimate', String(e.payload.BATTERY_ESTIMATE));
    localStorage.setItem('batterySinceCharge', String(e.payload.BATTERY_SINCE_CHARGE || ''));
    localStorage.setItem('batteryRateMilli', String(e.payload.BATTERY_RATE_MILLI || 0));
    console.log('Pebblesole: battery info ' + e.payload.BATTERY_ESTIMATE +
                ', since charge ' + e.payload.BATTERY_SINCE_CHARGE +
                ' (' + e.payload.BATTERY_RATE_MILLI + ' m%/h)');
    // Fresh values arrived — if the config page is waiting on them, open it
    // now so the user sees current data instead of the stale cache.
    openConfigWhenReady();
  }
});

// =============================================================================
// Configuration page (Clay, opened manually)
// =============================================================================
// Config-page open is deferred until fresh battery info arrives (or a short
// timeout elapses), so the page never shows stale battery stats. These guard
// against opening twice or leaving a stale timer behind.
var configOpenPending = false;
var configOpenTimer = null;

// Parse a "Xd Yh" or "Yh Zm" duration string into total hours.
// Returns null if the string is not a parseable estimate.
function parseDurationHours(s) {
  if (!s) return null;
  var d = s.match(/(\d+)d/);
  var h = s.match(/(\d+)h/);
  var m = s.match(/(\d+)m/);
  if (!d && !h && !m) return null;
  return (d ? parseInt(d[1], 10) * 24 : 0) +
         (h ? parseInt(h[1], 10) : 0) +
         (m ? parseInt(m[1], 10) / 60 : 0);
}

function getBatteryStatsHtml() {
  var est = localStorage.getItem('batteryEstimate') || '—';
  var since = localStorage.getItem('batterySinceCharge') || '—';
  var rateMilli = parseInt(localStorage.getItem('batteryRateMilli') || '0', 10);
  var rateStr, rateDayStr;
  if (!rateMilli || rateMilli <= 0) {
    rateStr = '—';
    rateDayStr = '—';
  } else {
    rateStr = (rateMilli / 1000).toFixed(2) + '%';
    rateDayStr = (rateMilli * 24 / 1000).toFixed(1) + '%';
  }

  // Next-charge date: today + remaining hours from the estimate string.
  var nextStr = '—';
  var hours = parseDurationHours(est);
  if (hours !== null) {
    var when = new Date(Date.now() + hours * 3600 * 1000);
    nextStr = when.getDate() + '/' + (when.getMonth() + 1);
  }

  return 'Time since last charge: <strong>' + since + '</strong><br>' +
         'Time remaining: <strong>' + est + '</strong><br>' +
         'Next charge: <strong>' + nextStr + '</strong><br>' +
         'Discharge rate per hour: <strong>' + rateStr + '</strong><br>' +
         'Discharge rate per day: <strong>' + rateDayStr + '</strong><br>' +
         '<em>Estimate is learned from your usage; allow a day of wear ' +
         'before it stabilizes.</em>';
}

// Find the battery-stats text item in the Clay config and fill it in.
function injectBatteryStats(config) {
  for (var i = 0; i < config.length; i++) {
    var item = config[i];
    if (item.items) { injectBatteryStats(item.items); }
    if (item.id === 'batteryStats') {
      item.defaultValue = getBatteryStatsHtml();
    }
  }
}

function openConfigWhenReady() {
  if (!configOpenPending) { return; }
  configOpenPending = false;
  if (configOpenTimer !== null) {
    clearTimeout(configOpenTimer);
    configOpenTimer = null;
  }
  injectBatteryStats(clay.config);
  Pebble.openURL(clay.generateUrl());
}

Pebble.addEventListener('showConfiguration', function () {
  // Ask the watch for a fresh estimate, then defer opening the page until it
  // arrives via 'appmessage' (see openConfigWhenReady). A timeout fallback
  // opens with cached values if the watch is slow or disconnected, so the
  // page always opens promptly.
  configOpenPending = true;
  if (configOpenTimer !== null) { clearTimeout(configOpenTimer); }
  configOpenTimer = setTimeout(function () {
    configOpenTimer = null;
    openConfigWhenReady();
  }, 1500);

  Pebble.sendAppMessage({ REQUEST_BATTERY_INFO: 1 },
    function () { console.log('Pebblesole: battery info requested'); },
    function (e) {
      console.log('Pebblesole: battery info request failed ' + JSON.stringify(e));
      // Watch unreachable — open immediately with cached values.
      openConfigWhenReady();
    }
  );
});

// When Clay config is saved, push the settings to the watch and re-fetch.
Pebble.addEventListener('webviewclosed', function (e) {
  if (!e || !e.response) return;
  // convert=false returns settings keyed by messageKey NAME (and still
  // persists them for the page pre-fill). The default (converted) form keys
  // by numeric message-key id and leaves select values as strings, which the
  // C side would misread as garbage int32s.
  var settings = clay.getSettings(e.response, false);

  // Flatten: each entry is either a raw value or {value: ...}.
  var dict = {};
  Object.keys(settings).forEach(function (k) {
    var v = settings[k];
    dict[k] = (v && typeof v === 'object') ? v.value : v;
  });

  // Clay select/radiogroup values are strings; the C side reads int32.
  var intKeys = ['TEMP_UNITS', 'WEATHER_INTERVAL', 'NIGHT_START_HOUR',
                 'NIGHT_END_HOUR', 'NIGHT_UPDATE_INTERVAL', 'BACKLIGHT_COLOR'];
  for (var i = 0; i < intKeys.length; i++) {
    if (dict[intKeys[i]] !== undefined) {
      dict[intKeys[i]] = parseInt(dict[intKeys[i]], 10) || 0;
    }
  }
  dict.NIGHT_MODE_ENABLED = dict.NIGHT_MODE_ENABLED ? 1 : 0;

  // Persist the units choice for getTempUnits().
  localStorage.setItem('TEMP_UNITS', dict.TEMP_UNITS === 1 ? 'F' : 'C');
  // Persist the refresh interval for the ready-event staleness check.
  if (dict.WEATHER_INTERVAL) {
    localStorage.setItem('weatherInterval', String(dict.WEATHER_INTERVAL));
  }

  Pebble.sendAppMessage(dict,
    function () { console.log('Pebblesole: settings sent ' + JSON.stringify(dict)); },
    function (er) { console.log('Pebblesole: settings send failed ' + JSON.stringify(er)); }
  );
  refresh();
});
