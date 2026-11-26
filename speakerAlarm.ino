#include <Arduino.h>
#include <driver/i2s.h>
#include <math.h>
#include <WebServer.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <time.h>

constexpr gpio_num_t I2S_LRC = GPIO_NUM_15;
constexpr gpio_num_t I2S_BCLK = GPIO_NUM_16;
constexpr gpio_num_t I2S_DIN = GPIO_NUM_17;
constexpr uint8_t AMP_SD = 18;

const char *WIFI_SSID = "DEA";
const char *WIFI_PASSWORD = "40542992";

constexpr uint32_t SAMPLE_RATE = 44100;
constexpr uint16_t TONE_HZ = 440;
constexpr uint32_t TONE_SAMPLES = SAMPLE_RATE / 2;
constexpr size_t BUFFER_SAMPLES = 256;
constexpr size_t MAX_ALARMS = 20;
constexpr size_t MAX_TIMERS = 20;
constexpr size_t AUDIO_UPLOAD_RESERVE_BYTES = 4096;
constexpr char TIME_ZONE[] = "PST8PDT,M3.2.0/2,M11.1.0/2";

WebServer server(80);
Preferences preferences;
struct Alarm {
  char name[32];
  uint32_t secondsOfDay;
  uint8_t volume;
  uint8_t daysMask;
  uint32_t id;
};
struct WavInfo {
  uint32_t dataOffset;
  uint32_t dataBytes;
  uint32_t sampleRate;
  uint16_t channels;
  uint16_t bitsPerSample;
  uint16_t blockAlign;
};
struct LegacyAlarm {
  char name[32];
  uint16_t minuteOfDay;
  uint8_t volume;
  uint8_t daysMask;
};
struct LegacyAlarmStorage {
  uint8_t version;
  uint8_t count;
  LegacyAlarm alarms[MAX_ALARMS];
};
struct LegacyAlarmV2 {
  char name[32];
  uint16_t minuteOfDay;
  uint8_t volume;
  uint8_t daysMask;
  uint32_t id;
};
struct LegacyAlarmStorageV2 {
  uint8_t version;
  uint8_t count;
  uint32_t nextId;
  LegacyAlarmV2 alarms[MAX_ALARMS];
};
struct AlarmStorage {
  uint8_t version;
  uint8_t count;
  uint32_t nextId;
  Alarm alarms[MAX_ALARMS];
};
struct Timer {
  char name[32];
  uint32_t durationSeconds;
  uint32_t remainingSeconds;
  uint32_t deadlineMillis;
  uint8_t volume;
  uint8_t running;
  uint32_t id;
};
struct TimerStorage {
  uint8_t version;
  uint8_t count;
  uint32_t nextId;
  Timer timers[MAX_TIMERS];
};
AlarmStorage alarmStorage = {};
TimerStorage timerStorage = {};
int lastFiredDay[MAX_ALARMS] = {};
bool preferencesReady = false;
bool clockReady = false;
bool audioReady = false;
bool fileSystemReady = false;
bool serverStarted = false;
bool mdnsStarted = false;
bool wasWifiConnected = false;
volatile bool wifiAssociated = false;
volatile bool wifiHasIp = false;
volatile uint32_t wifiIpWaitStartedAt = 0;
IPAddress reportedIp;
uint32_t lastReconnectAttempt = 0;
uint32_t lastMdnsAttempt = 0;
File audioUploadFile;
bool audioUploadStarted = false;
bool audioUploadComplete = false;
bool audioUploadFailed = false;
uint32_t audioUploadId = 0;
bool audioUploadIsTimer = false;
size_t audioUploadBytes = 0;
int audioUploadStatus = 400;
String audioUploadError;

void handleWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  switch (event) {
  case ARDUINO_EVENT_WIFI_STA_CONNECTED:
    wifiAssociated = true;
    wifiHasIp = false;
    wifiIpWaitStartedAt = millis();
    Serial.println("Associated with Wi-Fi; waiting for a DHCP address.");
    break;
  case ARDUINO_EVENT_WIFI_STA_GOT_IP:
    wifiAssociated = true;
    wifiHasIp = true;
    Serial.print("DHCP assigned IP: ");
    Serial.println(IPAddress(info.got_ip.ip_info.ip.addr));
    break;
  case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
    wifiAssociated = false;
    wifiHasIp = false;
    wifiIpWaitStartedAt = millis();
    Serial.printf("Wi-Fi disconnected; reason code %u.\n",
                  info.wifi_sta_disconnected.reason);
    break;
  case ARDUINO_EVENT_WIFI_STA_LOST_IP:
    wifiHasIp = false;
    wifiIpWaitStartedAt = millis();
    Serial.println("Wi-Fi lost its IP address; waiting for DHCP.");
    break;
  default:
    break;
  }
}

const char PAGE[] PROGMEM = R"rawliteral(
<!doctype html>
<html lang="en">
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <meta charset="utf-8">
  <title>Speaker Alarm</title>
  <style>
    :root { color-scheme: light; --bg:#f4f5f7; --card:#fff; --text:#20242a; --muted:#606975; --line:#d5dae0; --accent:#1769d2; --control-accent:#198754; }
    :root[data-theme="dark"] { color-scheme: dark; --bg:#171a1f; --card:#232831; --text:#edf0f4; --muted:#b0b8c2; --line:#414955; --accent:#78aaff; --control-accent:#45c878; }
    * { box-sizing:border-box; }
    body { font:16px system-ui,sans-serif; max-width:42rem; margin:2rem auto; padding:0 1rem; background:var(--bg); color:var(--text); }
    header { display:flex; align-items:center; justify-content:space-between; gap:1rem; flex-wrap:wrap; margin-bottom:1.5rem; }
    h1 { margin:0; font-size:1.7rem; }
    button,input,select { font:inherit; }
    button { padding:.65rem .9rem; border:1px solid var(--line); border-radius:.55rem; background:var(--card); color:var(--text); cursor:pointer; }
    button.primary { background:var(--accent); color:#fff; border-color:var(--accent); }
    button:disabled { opacity:.6; cursor:wait; }
    .actions { display:flex; align-items:center; gap:.9rem; }
    .theme-control { display:flex; align-items:center; gap:.5rem; }
    #clockStatus,.muted { color:var(--muted); }
    #alarmList,#timerList { display:grid; grid-template-columns:1fr; gap:.75rem; }
    .section-heading { display:flex; align-items:center; justify-content:space-between; gap:1rem; margin:1.5rem 0 .75rem; }
    .section-heading h2 { margin:0; font-size:1.25rem; }
    .alarm,.timer { background:var(--card); border:1px solid var(--line); border-radius:.7rem; overflow:hidden; }
    .alarm summary,.timer summary { display:flex; align-items:center; justify-content:space-between; gap:1rem; padding:1rem; cursor:pointer; list-style:none; }
    .timer-summary-actions { display:flex; align-items:center; gap:.4rem; flex-wrap:wrap; justify-content:flex-end; }
    .timer-summary-actions button { padding:.45rem .55rem; font-size:.85rem; }
    .alarm summary::-webkit-details-marker,.timer summary::-webkit-details-marker { display:none; }
    .alarm summary::after,.timer summary::after { content:""; width:.55rem; height:.55rem; border-right:2px solid var(--muted); border-bottom:2px solid var(--muted); transform:rotate(45deg); transition:transform .15s; flex:none; margin-right:.25rem; }
    .alarm[open] summary::after,.timer[open] summary::after { transform:rotate(225deg); }
    .alarm-summary-main,.timer-summary-main { display:grid; gap:.2rem; }
    .alarm h2,.timer h2 { margin:0; font-size:1.1rem; }
    .alarm-time,.timer-time { font-size:1.6rem; font-variant-numeric:tabular-nums; }
    .alarm-info,.timer-info { color:var(--muted); font-size:.9rem; }
    dialog { width:min(30rem,calc(100% - 2rem)); border:1px solid var(--line); border-radius:.8rem; background:var(--card); color:var(--text); }
    dialog::backdrop { background:#0008; }
    form { display:grid; gap:1rem; }
    form h2 { margin:0; }
    form > label { display:grid; gap:.35rem; }
    input[type=text],input[type=time],select { padding:.55rem; border:1px solid var(--line); border-radius:.4rem; background:var(--bg); color:var(--text); }
    #alarmDialog details { border:1px solid var(--line); border-radius:.4rem; padding:.55rem; }
    summary { cursor:pointer; }
    .day-options { display:grid; grid-template-columns:repeat(2,1fr); gap:.65rem; padding-top:.7rem; }
    .day-toggle { display:flex; align-items:center; justify-content:space-between; gap:.5rem; }
    .toggle-switch { position:relative; display:inline-block; width:2.6rem; height:1.45rem; flex:none; vertical-align:middle; }
    .toggle-switch input { position:absolute; width:1px; height:1px; opacity:0; }
    .switch-track { position:absolute; inset:0; border-radius:999px; background:var(--line); cursor:pointer; transition:background .15s; }
    .switch-track::before { content:""; position:absolute; width:1.05rem; height:1.05rem; left:.2rem; top:.2rem; border-radius:50%; background:#fff; transition:transform .15s; }
    .toggle-switch input:checked + .switch-track { background:var(--control-accent); }
    .toggle-switch input:checked + .switch-track::before { transform:translateX(1.15rem); }
    .toggle-switch input:focus-visible + .switch-track { outline:2px solid var(--control-accent); outline-offset:2px; }
    .alarm-editor,.timer-editor { display:grid; gap:1rem; padding:1rem; border-top:1px solid var(--line); }
    .alarm-editor > label,.timer-editor > label { display:grid; gap:.35rem; }
    .volume-line { display:flex; align-items:center; gap:.7rem; }
    .volume-line input { flex:1; accent-color:var(--control-accent); }
    .timer-duration-fields { display:grid; grid-template-columns:repeat(3,1fr); gap:.6rem; }
    .timer-duration-fields label { display:grid; gap:.25rem; color:var(--muted); font-size:.85rem; }
    .timer-duration-fields input { width:100%; padding:.55rem; border:1px solid var(--line); border-radius:.4rem; background:var(--bg); color:var(--text); }
    .audio-controls { display:grid; justify-items:start; gap:.4rem; }
    .form-actions { display:flex; justify-content:flex-end; gap:.5rem; }
    #formError { color:#c22; margin:0; }
    .edit-error { color:#c22; margin:0; }
  </style>
</head>
<body>
  <header>
    <h1>Speaker Alarm</h1>
    <div class="actions">
      <label class="theme-control" for="darkMode">Dark mode
        <span class="toggle-switch">
          <input id="darkMode" type="checkbox" role="switch">
          <span class="switch-track"></span>
        </span>
      </label>
      <button id="addAlarm" class="primary" type="button">Add alarm</button>
    </div>
  </header>
  <p id="clockStatus" role="status">Checking clock...</p>
  <section id="alarmList" aria-live="polite"></section>
  <div class="section-heading">
    <h2>Timers</h2>
    <button id="addTimer" class="primary" type="button">Add timer</button>
  </div>
  <section id="timerList" aria-live="polite"></section>
  <dialog id="alarmDialog">
    <form id="alarmForm">
      <h2>Add alarm</h2>
      <label>Name <input name="name" type="text" maxlength="31" required></label>
      <label>Time <input name="time" type="time" step="1" required></label>
      <label>Volume <span><input name="volume" type="range" min="1" max="100" value="70"> <output id="volumeValue">70%</output></span></label>
      <details>
        <summary>Select day(s)</summary>
        <div class="day-options">
          <label class="day-toggle"><span>Sunday</span><span class="toggle-switch"><input name="day" type="checkbox" value="0" role="switch"><span class="switch-track"></span></span></label>
          <label class="day-toggle"><span>Monday</span><span class="toggle-switch"><input name="day" type="checkbox" value="1" role="switch"><span class="switch-track"></span></span></label>
          <label class="day-toggle"><span>Tuesday</span><span class="toggle-switch"><input name="day" type="checkbox" value="2" role="switch"><span class="switch-track"></span></span></label>
          <label class="day-toggle"><span>Wednesday</span><span class="toggle-switch"><input name="day" type="checkbox" value="3" role="switch"><span class="switch-track"></span></span></label>
          <label class="day-toggle"><span>Thursday</span><span class="toggle-switch"><input name="day" type="checkbox" value="4" role="switch"><span class="switch-track"></span></span></label>
          <label class="day-toggle"><span>Friday</span><span class="toggle-switch"><input name="day" type="checkbox" value="5" role="switch"><span class="switch-track"></span></span></label>
          <label class="day-toggle"><span>Saturday</span><span class="toggle-switch"><input name="day" type="checkbox" value="6" role="switch"><span class="switch-track"></span></span></label>
        </div>
      </details>
      <p id="formError" role="alert"></p>
      <div class="form-actions">
        <button id="cancelAlarm" type="button">Cancel</button>
        <button class="primary" type="submit">Save alarm</button>
      </div>
    </form>
  </dialog>
  <dialog id="timerDialog">
    <form id="timerForm">
      <h2>Add timer</h2>
      <label>Name <input name="name" type="text" maxlength="31" required></label>
      <div>Duration
        <div class="timer-duration-fields">
          <label><span>Hours</span><input name="hours" type="number" min="0" max="24" value="0" required></label>
          <label><span>Minutes</span><input name="minutes" type="number" min="0" max="59" value="5" required></label>
          <label><span>Seconds</span><input name="seconds" type="number" min="0" max="59" value="0" required></label>
        </div>
      </div>
      <label>Volume <span><input name="volume" type="range" min="1" max="100" value="70"> <output id="timerVolumeValue">70%</output></span></label>
      <p id="timerFormError" role="alert"></p>
      <div class="form-actions">
        <button id="cancelTimer" type="button">Cancel</button>
        <button class="primary" type="submit">Save timer</button>
      </div>
    </form>
  </dialog>
  <script>
    const root = document.documentElement;
    const list = document.getElementById('alarmList');
    const dialog = document.getElementById('alarmDialog');
    const form = document.getElementById('alarmForm');
    const timerList = document.getElementById('timerList');
    const timerDialog = document.getElementById('timerDialog');
    const timerForm = document.getElementById('timerForm');
    const clockStatus = document.getElementById('clockStatus');
    const dayNames = ['Sun','Mon','Tue','Wed','Thu','Fri','Sat'];
    const themeSwitch = document.getElementById('darkMode');
    let openAlarmId = null;
    let openTimerId = null;

    function setTheme(dark) {
      root.dataset.theme = dark ? 'dark' : 'light';
      themeSwitch.checked = dark;
      localStorage.setItem('darkMode', dark ? '1' : '0');
    }
    setTheme(localStorage.getItem('darkMode') === '1');
    themeSwitch.addEventListener('change', () => setTheme(themeSwitch.checked));
    document.getElementById('addAlarm').addEventListener('click', () => {
      form.reset();
      document.getElementById('volumeValue').textContent = '70%';
      document.getElementById('formError').textContent = '';
      dialog.showModal();
    });
    document.getElementById('cancelAlarm').addEventListener('click', () => dialog.close());
    form.elements.volume.addEventListener('input', event => {
      document.getElementById('volumeValue').textContent = event.target.value + '%';
    });
    document.getElementById('addTimer').addEventListener('click', () => {
      timerForm.reset();
      timerForm.elements.hours.value = '0';
      timerForm.elements.minutes.value = '5';
      timerForm.elements.seconds.value = '0';
      document.getElementById('timerVolumeValue').textContent = '70%';
      document.getElementById('timerFormError').textContent = '';
      timerDialog.showModal();
    });
    document.getElementById('cancelTimer').addEventListener('click', () => timerDialog.close());
    timerForm.elements.volume.addEventListener('input', event => {
      document.getElementById('timerVolumeValue').textContent = event.target.value + '%';
    });

    function makeDayToggle(day, checked) {
      const label = document.createElement('label');
      label.className = 'day-toggle';
      const title = document.createElement('span');
      title.textContent = ['Sunday','Monday','Tuesday','Wednesday','Thursday','Friday','Saturday'][day];
      const toggle = document.createElement('span');
      toggle.className = 'toggle-switch';
      const input = document.createElement('input');
      input.type = 'checkbox';
      input.name = 'day';
      input.value = String(day);
      input.checked = checked;
      input.setAttribute('role', 'switch');
      const track = document.createElement('span');
      track.className = 'switch-track';
      toggle.append(input, track);
      label.append(title, toggle);
      return label;
    }

    function addField(formElement, title, control) {
      const label = document.createElement('label');
      label.append(document.createTextNode(title), control);
      formElement.appendChild(label);
    }

    function formatDurationParts(value) {
      const parts = value.split(':').map(Number);
      return {
        hours: parts[0] || 0,
        minutes: parts[1] || 0,
        seconds: parts[2] || 0
      };
    }

    function makeDurationFields(value) {
      const parts = formatDurationParts(value);
      const container = document.createElement('div');
      container.className = 'timer-duration-fields';
      const fields = {};
      [['hours','Hours',24,parts.hours], ['minutes','Minutes',59,parts.minutes], ['seconds','Seconds',59,parts.seconds]]
        .forEach(([name, title, max, initial]) => {
          const label = document.createElement('label');
          const text = document.createElement('span');
          text.textContent = title;
          const input = document.createElement('input');
          input.type = 'number';
          input.name = name;
          input.min = '0';
          input.max = String(max);
          input.value = String(initial);
          input.required = true;
          fields[name] = input;
          label.append(text, input);
          container.appendChild(label);
        });
      return { container, fields };
    }

    function durationValue(fields) {
      const pad = value => String(value).padStart(2, '0');
      return pad(fields.hours.value) + ':' +
        pad(fields.minutes.value) + ':' +
        pad(fields.seconds.value);
    }

    function fillPayload(targetForm, payload) {
      payload.set('name', targetForm.elements.name.value);
      payload.set('time', targetForm.elements.time.value);
      payload.set('volume', targetForm.elements.volume.value);
      targetForm.querySelectorAll('input[name="day"]:checked')
        .forEach(day => payload.append('day', day.value));
    }

    async function uploadAlarmAudio(alarm, input, button, status, onUploaded) {
      const file = input.files[0];
      if (!file) {
        status.textContent = 'Choose a WAV file first.';
        return;
      }
      button.disabled = true;
      try {
        if (!file.name.toLowerCase().endsWith('.wav')) {
          throw new Error('Choose a WAV file (.wav).');
        }
        status.textContent = 'Uploading WAV file...';
        const audio = new FormData();
        audio.append('audio', file, file.name);
        const response = await fetch('/api/audio?id=' + encodeURIComponent(alarm.id), {
          method: 'POST',
          body: audio
        });
        if (!response.ok) throw new Error(await response.text());
        status.textContent = 'WAV file uploaded. It will play at alarm time.';
        input.value = '';
        onUploaded();
      } catch (error) {
        status.textContent = 'WAV upload failed: ' + error.message;
      } finally {
        button.disabled = false;
      }
    }

    async function timerAction(timer, action) {
      const payload = new URLSearchParams();
      payload.set('action', action);
      const response = await fetch('/api/timers?id=' + timer.id, {
        method: 'PUT',
        body: payload
      });
      if (!response.ok) throw new Error(await response.text());
      await loadTimers();
    }

    async function uploadTimerAudio(timer, input, button, status, onUploaded) {
      const file = input.files[0];
      if (!file) {
        status.textContent = 'Choose a WAV file first.';
        return;
      }
      button.disabled = true;
      try {
        if (!file.name.toLowerCase().endsWith('.wav')) {
          throw new Error('Choose a WAV file (.wav).');
        }
        status.textContent = 'Uploading WAV file...';
        const audio = new FormData();
        audio.append('audio', file, file.name);
        const response = await fetch('/api/audio?type=timer&id=' + encodeURIComponent(timer.id), {
          method: 'POST',
          body: audio
        });
        if (!response.ok) throw new Error(await response.text());
        status.textContent = 'WAV file uploaded. It will play when the timer finishes.';
        input.value = '';
        onUploaded();
      } catch (error) {
        status.textContent = 'WAV upload failed: ' + error.message;
      } finally {
        button.disabled = false;
      }
    }

    async function loadTimers(requestedOpenId) {
      if (requestedOpenId === undefined) {
        const currentlyOpen = timerList.querySelector('.timer[open]');
        requestedOpenId = currentlyOpen ? Number(currentlyOpen.dataset.id) : openTimerId;
      }
      const response = await fetch('/api/timers');
      if (!response.ok) throw new Error(await response.text());
      const data = await response.json();
      timerList.replaceChildren();
      if (!data.timers.length) {
        const empty = document.createElement('p');
        empty.className = 'muted';
        empty.textContent = 'No timers yet.';
        timerList.appendChild(empty);
        return;
      }
      data.timers.forEach(timer => {
        const card = document.createElement('details');
        card.className = 'timer';
        card.dataset.id = timer.id;
        card.open = timer.id === requestedOpenId;
        if (card.open) openTimerId = timer.id;
        const summary = document.createElement('summary');
        const summaryMain = document.createElement('span');
        summaryMain.className = 'timer-summary-main';
        const name = document.createElement('h2');
        name.textContent = timer.name;
        const time = document.createElement('div');
        time.className = 'timer-time';
        time.textContent = timer.remaining;
        const remainingParts = timer.remaining.split(':').map(Number);
        time.dataset.remaining = String(
          remainingParts[0] * 3600 + remainingParts[1] * 60 + remainingParts[2]
        );
        time.dataset.running = timer.running ? 'true' : 'false';
        const info = document.createElement('div');
        info.className = 'timer-info';
        info.textContent = timer.running
          ? 'Running · Volume ' + timer.volume + '%'
          : 'Stopped · Volume ' + timer.volume + '%';
        summaryMain.append(name, time, info);
        const summaryActions = document.createElement('span');
        summaryActions.className = 'timer-summary-actions';
        const summaryRemove = document.createElement('button');
        summaryRemove.type = 'button';
        summaryRemove.textContent = 'Delete';
        summaryRemove.addEventListener('click', async event => {
          event.stopPropagation();
          if (!window.confirm('Delete timer "' + timer.name + '"?')) return;
          try {
            const result = await fetch('/api/timers?id=' + timer.id, { method: 'DELETE' });
            if (!result.ok) throw new Error(await result.text());
            openTimerId = null;
            await loadTimers(null);
          } catch (actionError) {
            clockStatus.textContent = 'Timer action failed: ' + actionError.message;
          }
        });
        const summaryReset = document.createElement('button');
        summaryReset.type = 'button';
        summaryReset.textContent = 'Reset';
        summaryReset.addEventListener('click', async event => {
          event.stopPropagation();
          try { await timerAction(timer, 'reset'); }
          catch (actionError) { clockStatus.textContent = 'Timer action failed: ' + actionError.message; }
        });
        const summaryToggle = document.createElement('button');
        summaryToggle.type = 'button';
        summaryToggle.textContent = timer.running ? 'Pause' : 'Start';
        summaryToggle.addEventListener('click', async event => {
          event.stopPropagation();
          try { await timerAction(timer, timer.running ? 'pause' : 'start'); }
          catch (actionError) { clockStatus.textContent = 'Timer action failed: ' + actionError.message; }
        });
        summaryActions.append(summaryRemove, summaryReset, summaryToggle);
        summary.append(summaryMain, summaryActions);
        summary.addEventListener('click', event => {
          if (event.target.closest('button')) return;
          openTimerId = card.open ? null : timer.id;
        });

        const editor = document.createElement('form');
        editor.className = 'timer-editor';
        const nameInput = document.createElement('input');
        nameInput.type = 'text';
        nameInput.name = 'name';
        nameInput.maxLength = 31;
        nameInput.required = true;
        nameInput.value = timer.name;
        addField(editor, 'Name', nameInput);
        const duration = makeDurationFields(timer.duration);
        addField(editor, 'Duration', duration.container);
        const volumeInput = document.createElement('input');
        volumeInput.type = 'range';
        volumeInput.name = 'volume';
        volumeInput.min = '1';
        volumeInput.max = '100';
        volumeInput.value = timer.volume;
        const volumeOutput = document.createElement('output');
        volumeOutput.textContent = timer.volume + '%';
        const volumeLine = document.createElement('span');
        volumeLine.className = 'volume-line';
        volumeLine.append(volumeInput, volumeOutput);
        addField(editor, 'Volume', volumeLine);
        volumeInput.addEventListener('input', () => {
          volumeOutput.textContent = volumeInput.value + '%';
        });
        const audioInput = document.createElement('input');
        audioInput.type = 'file';
        audioInput.accept = '.wav,audio/wav,audio/x-wav';
        addField(editor, 'Timer sound (WAV)', audioInput);
        const audioStatus = document.createElement('p');
        audioStatus.className = 'timer-info';
        audioStatus.textContent = timer.hasAudio
          ? 'WAV already uploaded. It will play when the timer finishes; choose another WAV to replace it.'
          : 'No WAV file uploaded; the default beep will play.';
        const uploadButton = document.createElement('button');
        uploadButton.type = 'button';
        uploadButton.textContent = 'Upload WAV';
        uploadButton.disabled = true;
        audioInput.addEventListener('change', () => {
          uploadButton.disabled = !audioInput.files.length;
        });
        uploadButton.addEventListener('click', () => {
          uploadTimerAudio(timer, audioInput, uploadButton, audioStatus, () => {
            timer.hasAudio = true;
          });
        });
        const audioControls = document.createElement('div');
        audioControls.className = 'audio-controls';
        audioControls.append(uploadButton, audioStatus);
        editor.appendChild(audioControls);

        const error = document.createElement('p');
        error.className = 'edit-error';
        error.setAttribute('role', 'alert');
        const actions = document.createElement('div');
        actions.className = 'form-actions';
        const remove = document.createElement('button');
        remove.type = 'button';
        remove.textContent = 'Delete';
        remove.addEventListener('click', async () => {
          if (!window.confirm('Delete timer "' + timer.name + '"?')) return;
          try {
            const result = await fetch('/api/timers?id=' + timer.id, { method: 'DELETE' });
            if (!result.ok) throw new Error(await result.text());
            openTimerId = null;
            await loadTimers(null);
          } catch (deleteError) {
            error.textContent = deleteError.message;
          }
        });
        const reset = document.createElement('button');
        reset.type = 'button';
        reset.textContent = 'Reset';
        reset.addEventListener('click', async () => {
          try { await timerAction(timer, 'reset'); }
          catch (actionError) { error.textContent = actionError.message; }
        });
        const toggle = document.createElement('button');
        toggle.type = 'button';
        toggle.textContent = timer.running ? 'Pause' : 'Start';
        toggle.addEventListener('click', async () => {
          try { await timerAction(timer, timer.running ? 'pause' : 'start'); }
          catch (actionError) { error.textContent = actionError.message; }
        });
        const save = document.createElement('button');
        save.type = 'submit';
        save.className = 'primary';
        save.textContent = 'Save changes';
        actions.append(remove, reset, toggle, save);
        editor.append(error, actions);

        const hasChanges = () =>
          nameInput.value !== timer.name ||
          durationValue(duration.fields) !== timer.duration ||
          Number(volumeInput.value) !== timer.volume;
        const updateActionLabel = () => {
          const changed = hasChanges();
          save.textContent = changed ? 'Save changes' : 'Cancel';
          save.classList.toggle('primary', changed);
        };
        editor.addEventListener('input', updateActionLabel);
        editor.addEventListener('change', updateActionLabel);
        updateActionLabel();
        editor.addEventListener('submit', async event => {
          event.preventDefault();
          if (!hasChanges()) {
            card.open = false;
            openTimerId = null;
            return;
          }
          const payload = new URLSearchParams();
          payload.set('name', nameInput.value);
          payload.set('duration', durationValue(duration.fields));
          payload.set('volume', volumeInput.value);
          save.disabled = true;
          error.textContent = '';
          try {
            const result = await fetch('/api/timers?id=' + timer.id, {
              method: 'PUT',
              body: payload
            });
            if (!result.ok) throw new Error(await result.text());
            openTimerId = null;
            await loadTimers(null);
          } catch (saveError) {
            error.textContent = saveError.message;
          } finally {
            save.disabled = false;
          }
        });
        card.append(summary, editor);
        timerList.appendChild(card);
      });
    }

    async function loadAlarms(requestedOpenId) {
      if (requestedOpenId === undefined) {
        const currentlyOpen = list.querySelector('.alarm[open]');
        requestedOpenId = currentlyOpen ? Number(currentlyOpen.dataset.id) : openAlarmId;
      }
      const response = await fetch('/api/alarms');
      if (!response.ok) throw new Error(await response.text());
      const data = await response.json();
      clockStatus.textContent = data.clockReady
        ? 'Clock synced. Alarm times use America/Los_Angeles.'
        : 'Waiting for network time sync; alarms will run when the clock is synced.';
      list.replaceChildren();
      if (!data.alarms.length) {
        const empty = document.createElement('p');
        empty.className = 'muted';
        empty.textContent = 'No alarms yet.';
        list.appendChild(empty);
      }
      data.alarms.forEach(alarm => {
        const card = document.createElement('details');
        card.className = 'alarm';
        card.dataset.id = alarm.id;
        card.open = alarm.id === requestedOpenId;
        if (card.open) openAlarmId = alarm.id;
        const summary = document.createElement('summary');
        const summaryMain = document.createElement('span');
        summaryMain.className = 'alarm-summary-main';
        const name = document.createElement('h2');
        name.textContent = alarm.name;
        const time = document.createElement('div');
        time.className = 'alarm-time';
        time.textContent = alarm.time;
        const info = document.createElement('div');
        info.className = 'alarm-info';
        info.textContent = alarm.days.map((day, index) => day ? dayNames[index] : '').filter(Boolean).join(', ')
          + ' · Volume ' + alarm.volume + '% · ' + (alarm.hasAudio ? 'Uploaded WAV' : 'Default beep');
        summaryMain.append(name, time, info);
        summary.appendChild(summaryMain);
        summary.addEventListener('click', () => {
          openAlarmId = card.open ? null : alarm.id;
        });
        const editor = document.createElement('form');
        editor.className = 'alarm-editor';
        let audioChanged = false;
        const nameInput = document.createElement('input');
        nameInput.type = 'text';
        nameInput.name = 'name';
        nameInput.maxLength = 31;
        nameInput.required = true;
        nameInput.value = alarm.name;
        addField(editor, 'Name', nameInput);
        const timeInput = document.createElement('input');
        timeInput.type = 'time';
        timeInput.step = '1';
        timeInput.name = 'time';
        timeInput.required = true;
        timeInput.value = alarm.time;
        addField(editor, 'Time', timeInput);
        const volumeInput = document.createElement('input');
        volumeInput.type = 'range';
        volumeInput.name = 'volume';
        volumeInput.min = '1';
        volumeInput.max = '100';
        volumeInput.value = alarm.volume;
        const volumeOutput = document.createElement('output');
        volumeOutput.textContent = alarm.volume + '%';
        const volumeLine = document.createElement('span');
        volumeLine.className = 'volume-line';
        volumeLine.append(volumeInput, volumeOutput);
        addField(editor, 'Volume', volumeLine);
        volumeInput.addEventListener('input', () => {
          volumeOutput.textContent = volumeInput.value + '%';
        });
        const dayOptions = document.createElement('div');
        dayOptions.className = 'day-options';
        alarm.days.forEach((selected, day) => {
          dayOptions.appendChild(makeDayToggle(day, selected));
        });
        const daysLabel = document.createElement('div');
        daysLabel.textContent = 'Repeat on';
        editor.append(daysLabel, dayOptions);
        const audioInput = document.createElement('input');
        audioInput.type = 'file';
        audioInput.accept = '.wav,audio/wav,audio/x-wav';
        addField(editor, 'Alarm sound (WAV)', audioInput);
        const audioStatus = document.createElement('p');
        audioStatus.className = 'alarm-info';
        audioStatus.textContent = alarm.hasAudio
          ? 'WAV already uploaded. It will play for this alarm; choose another WAV to replace it.'
          : 'No WAV file uploaded; the default beep will play.';
        const uploadButton = document.createElement('button');
        uploadButton.type = 'button';
        uploadButton.textContent = 'Upload WAV';
        uploadButton.disabled = true;
        audioInput.addEventListener('change', () => {
          uploadButton.disabled = !audioInput.files.length;
        });
        uploadButton.addEventListener('click', () => {
          uploadAlarmAudio(alarm, audioInput, uploadButton, audioStatus, () => {
            audioChanged = true;
            alarm.hasAudio = true;
            info.textContent = alarm.days.map((day, index) => day ? dayNames[index] : '').filter(Boolean).join(', ')
              + ' · Volume ' + alarm.volume + '% · Uploaded WAV';
            updateActionLabel();
          });
        });
        const audioControls = document.createElement('div');
        audioControls.className = 'audio-controls';
        audioControls.append(uploadButton, audioStatus);
        editor.appendChild(audioControls);
        const error = document.createElement('p');
        error.className = 'edit-error';
        error.setAttribute('role', 'alert');
        const actions = document.createElement('div');
        actions.className = 'form-actions';
        const remove = document.createElement('button');
        remove.type = 'button';
        remove.textContent = 'Delete';
        remove.addEventListener('click', async () => {
          if (!window.confirm('Delete alarm "' + alarm.name + '"?')) return;
          try {
            const result = await fetch('/api/alarms?id=' + alarm.id, { method: 'DELETE' });
            if (!result.ok) throw new Error(await result.text());
            openAlarmId = null;
            await loadAlarms(null);
          } catch (deleteError) {
            error.textContent = deleteError.message;
          }
        });
        const save = document.createElement('button');
        save.type = 'submit';
        save.className = 'primary';
        save.textContent = 'Save changes';
        actions.append(remove, save);
        editor.append(error, actions);
        const hasChanges = () => {
          const daysChanged = Array.from(editor.querySelectorAll('input[name="day"]'))
            .some((day, index) => day.checked !== alarm.days[index]);
          return audioChanged ||
            nameInput.value !== alarm.name ||
            timeInput.value !== alarm.time ||
            Number(volumeInput.value) !== alarm.volume ||
            daysChanged;
        };
        const updateActionLabel = () => {
          const changed = hasChanges();
          save.textContent = changed ? 'Save changes' : 'Cancel';
          save.classList.toggle('primary', changed);
        };
        editor.addEventListener('input', updateActionLabel);
        editor.addEventListener('change', updateActionLabel);
        updateActionLabel();
        editor.addEventListener('submit', async event => {
          event.preventDefault();
          if (!hasChanges()) {
            card.open = false;
            openAlarmId = null;
            return;
          }
          const payload = new URLSearchParams();
          fillPayload(editor, payload);
          save.disabled = true;
          error.textContent = '';
          try {
            const result = await fetch('/api/alarms?id=' + alarm.id, {
              method: 'PUT',
              body: payload
            });
            if (!result.ok) throw new Error(await result.text());
            openAlarmId = null;
            await loadAlarms(null);
          } catch (saveError) {
            error.textContent = saveError.message;
          } finally {
            save.disabled = false;
          }
        });
        card.append(summary, editor);
        list.appendChild(card);
      });
    }

    form.addEventListener('submit', async event => {
      event.preventDefault();
      const payload = new URLSearchParams();
      fillPayload(form, payload);
      const error = document.getElementById('formError');
      error.textContent = '';
      try {
        const response = await fetch('/api/alarms', { method: 'POST', body: payload });
        if (!response.ok) throw new Error(await response.text());
        const created = await response.json();
        dialog.close();
        openAlarmId = created.id;
        await loadAlarms(created.id);
      } catch (saveError) {
        error.textContent = saveError.message;
      }
    });

    timerForm.addEventListener('submit', async event => {
      event.preventDefault();
      const payload = new URLSearchParams();
      payload.set('name', timerForm.elements.name.value);
      const timerDuration =
        String(timerForm.elements.hours.value).padStart(2, '0') + ':' +
        String(timerForm.elements.minutes.value).padStart(2, '0') + ':' +
        String(timerForm.elements.seconds.value).padStart(2, '0');
      payload.set('duration', timerDuration);
      payload.set('volume', timerForm.elements.volume.value);
      const error = document.getElementById('timerFormError');
      error.textContent = '';
      try {
        const response = await fetch('/api/timers', { method: 'POST', body: payload });
        if (!response.ok) throw new Error(await response.text());
        const created = await response.json();
        timerDialog.close();
        openTimerId = created.id;
        await loadTimers(created.id);
      } catch (saveError) {
        error.textContent = saveError.message;
      }
    });

    loadAlarms().catch(error => { clockStatus.textContent = 'Could not load alarms: ' + error.message; });
    loadTimers().catch(error => { clockStatus.textContent += ' Could not load timers: ' + error.message; });
    setInterval(() => {
      timerList.querySelectorAll('.timer-time').forEach(time => {
        if (time.dataset.running !== 'true') return;
        let remaining = Math.max(0, Number(time.dataset.remaining) - 1);
        time.dataset.remaining = String(remaining);
        const hours = Math.floor(remaining / 3600);
        const minutes = Math.floor((remaining % 3600) / 60);
        const seconds = remaining % 60;
        time.textContent =
          String(hours).padStart(2, '0') + ':' +
          String(minutes).padStart(2, '0') + ':' +
          String(seconds).padStart(2, '0');
      });
    }, 1000);
    setInterval(() => {
      if (dialog.open || timerDialog.open ||
          list.querySelector('.alarm[open]') ||
          timerList.querySelector('.timer[open]')) return;
      loadAlarms().catch(error => {
        clockStatus.textContent = 'Could not refresh alarms: ' + error.message;
      });
      loadTimers().catch(error => {
        clockStatus.textContent += ' Could not refresh timers: ' + error.message;
      });
    }, 30000);
  </script>
</body>
</html>
)rawliteral";

bool playAlarmTone(uint8_t volume) {
  int16_t samples[BUFFER_SAMPLES];
  size_t bytesWritten;
  const float amplitude = 12000.0f * volume / 100.0f;

  for (uint32_t offset = 0; offset < TONE_SAMPLES; offset += BUFFER_SAMPLES) {
    const size_t count = min(static_cast<size_t>(BUFFER_SAMPLES),
                             static_cast<size_t>(TONE_SAMPLES - offset));

    for (size_t i = 0; i < count; ++i) {
      const uint32_t sampleIndex = offset + i;
      const float phase = 2.0f * PI * TONE_HZ * sampleIndex / SAMPLE_RATE;
      samples[i] = static_cast<int16_t>(amplitude * sinf(phase));
    }

    if (i2s_write(I2S_NUM_0, samples, count * sizeof(int16_t), &bytesWritten,
                  portMAX_DELAY) != ESP_OK ||
        bytesWritten != count * sizeof(int16_t)) {
      Serial.println("I2S alarm tone write failed.");
      return false;
    }
  }

  memset(samples, 0, sizeof(samples));
  if (i2s_write(I2S_NUM_0, samples, sizeof(samples), &bytesWritten,
                portMAX_DELAY) != ESP_OK ||
      bytesWritten != sizeof(samples)) {
    Serial.println("I2S alarm silence write failed.");
    return false;
  }
  return true;
}

uint16_t readLittleEndian16(const uint8_t *bytes) {
  return static_cast<uint16_t>(bytes[0]) |
         static_cast<uint16_t>(bytes[1] << 8);
}

uint32_t readLittleEndian32(const uint8_t *bytes) {
  return static_cast<uint32_t>(bytes[0]) |
         (static_cast<uint32_t>(bytes[1]) << 8) |
         (static_cast<uint32_t>(bytes[2]) << 16) |
         (static_cast<uint32_t>(bytes[3]) << 24);
}

bool readWavInfo(File &file, WavInfo &info) {
  uint8_t header[12];
  if (!file.seek(0) || file.read(header, sizeof(header)) != sizeof(header) ||
      memcmp(header, "RIFF", 4) != 0 || memcmp(header + 8, "WAVE", 4) != 0) {
    return false;
  }

  const uint64_t riffEnd64 =
      static_cast<uint64_t>(8) + readLittleEndian32(header + 4);
  if (riffEnd64 > file.size() || riffEnd64 < sizeof(header)) {
    return false;
  }
  const uint32_t riffEnd = static_cast<uint32_t>(riffEnd64);
  uint32_t cursor = sizeof(header);
  bool foundFormat = false;
  bool foundData = false;
  uint32_t byteRate = 0;

  while (riffEnd - cursor >= 8 && (!foundFormat || !foundData)) {
    uint8_t chunkHeader[8];
    if (!file.seek(cursor) ||
        file.read(chunkHeader, sizeof(chunkHeader)) != sizeof(chunkHeader)) {
      return false;
    }
    const uint32_t chunkBytes = readLittleEndian32(chunkHeader + 4);
    const uint64_t chunkStart = static_cast<uint64_t>(cursor) + 8;
    const uint64_t chunkEnd = chunkStart + chunkBytes;
    const uint64_t nextChunk = chunkEnd + (chunkBytes & 1U);
    if (chunkEnd > riffEnd || nextChunk > riffEnd) {
      return false;
    }

    if (memcmp(chunkHeader, "fmt ", 4) == 0) {
      uint8_t format[16];
      if (foundFormat || chunkBytes < sizeof(format) ||
          !file.seek(static_cast<uint32_t>(chunkStart)) ||
          file.read(format, sizeof(format)) != sizeof(format)) {
        return false;
      }
      const uint16_t encoding = readLittleEndian16(format);
      info.channels = readLittleEndian16(format + 2);
      info.sampleRate = readLittleEndian32(format + 4);
      byteRate = readLittleEndian32(format + 8);
      info.blockAlign = readLittleEndian16(format + 12);
      info.bitsPerSample = readLittleEndian16(format + 14);
      const uint16_t expectedAlign =
          info.channels * (info.bitsPerSample / 8);
      if (encoding != 1 || (info.channels != 1 && info.channels != 2) ||
          (info.bitsPerSample != 8 && info.bitsPerSample != 16) ||
          info.sampleRate < 8000 || info.sampleRate > 48000 ||
          info.blockAlign != expectedAlign ||
          byteRate != info.sampleRate * info.blockAlign) {
        return false;
      }
      foundFormat = true;
    } else if (memcmp(chunkHeader, "data", 4) == 0) {
      if (foundData) return false;
      info.dataOffset = static_cast<uint32_t>(chunkStart);
      info.dataBytes = chunkBytes;
      foundData = true;
    }
    cursor = static_cast<uint32_t>(nextChunk);
  }

  return foundFormat && foundData && info.dataBytes > 0 &&
         info.dataBytes % info.blockAlign == 0;
}

bool playStoredAudio(const char *path, uint8_t volume, const char *label) {
  if (!fileSystemReady) {
    return false;
  }
  File audio = LittleFS.open(path, "r");
  if (!audio) {
    Serial.printf("Could not open %s WAV file.\n", label);
    return false;
  }

  WavInfo wav = {};
  if (!readWavInfo(audio, wav)) {
    audio.close();
    Serial.printf("%s WAV must be PCM, 8/16-bit mono/stereo at 8-48 kHz.\n", label);
    return false;
  }
  if (!audio.seek(wav.dataOffset) ||
      i2s_set_clk(I2S_NUM_0, wav.sampleRate, I2S_BITS_PER_SAMPLE_16BIT,
                  I2S_CHANNEL_MONO) != ESP_OK) {
    audio.close();
    Serial.printf("Could not prepare I2S for the %s WAV sample rate.\n", label);
    return false;
  }

  i2s_zero_dma_buffer(I2S_NUM_0);
  uint8_t input[BUFFER_SAMPLES * 4];
  int16_t output[BUFFER_SAMPLES];
  bool success = true;
  uint32_t remaining = wav.dataBytes;
  while (remaining > 0) {
    const size_t frameCount =
        min(static_cast<size_t>(BUFFER_SAMPLES),
            static_cast<size_t>(remaining / wav.blockAlign));
    const size_t inputBytes = frameCount * wav.blockAlign;
    if (frameCount == 0 || audio.read(input, inputBytes) != inputBytes) {
      success = false;
      break;
    }
    for (size_t i = 0; i < frameCount; ++i) {
      const size_t frameOffset = i * wav.blockAlign;
      int32_t mixedSample = 0;
      for (uint16_t channel = 0; channel < wav.channels; ++channel) {
        const size_t sampleOffset =
            frameOffset + channel * (wav.bitsPerSample / 8);
        int32_t sample;
        if (wav.bitsPerSample == 8) {
          sample = (static_cast<int32_t>(input[sampleOffset]) - 128) * 256;
        } else {
          const uint16_t raw = readLittleEndian16(input + sampleOffset);
          sample = (raw & 0x8000U) ? static_cast<int32_t>(raw) - 65536
                                   : static_cast<int32_t>(raw);
        }
        mixedSample += sample;
      }
      mixedSample /= wav.channels;
      output[i] = static_cast<int16_t>(mixedSample * volume / 100);
    }
    size_t bytesWritten = 0;
    const size_t outputBytes = frameCount * sizeof(int16_t);
    if (i2s_write(I2S_NUM_0, output, outputBytes, &bytesWritten,
                  portMAX_DELAY) != ESP_OK ||
        bytesWritten != outputBytes) {
      success = false;
      break;
    }
    remaining -= inputBytes;
  }
  memset(output, 0, sizeof(output));
  size_t silenceBytesWritten = 0;
  if (i2s_write(I2S_NUM_0, output, sizeof(output), &silenceBytesWritten,
                portMAX_DELAY) != ESP_OK ||
      silenceBytesWritten != sizeof(output)) {
    success = false;
  }
  delay(20);
  audio.close();
  if (i2s_set_clk(I2S_NUM_0, SAMPLE_RATE, I2S_BITS_PER_SAMPLE_16BIT,
                  I2S_CHANNEL_MONO) != ESP_OK) {
    Serial.printf("Could not restore the %s audio sample rate.\n", label);
    success = false;
  }
  return success;
}

bool playAlarmAudio(const Alarm &alarm) {
  char path[32];
  alarmAudioPath(alarm.id, ".wav", path, sizeof(path));
  return playStoredAudio(path, alarm.volume, "alarm");
}

bool playTimerAudio(const Timer &timer) {
  char path[32];
  timerAudioPath(timer.id, ".wav", path, sizeof(path));
  return playStoredAudio(path, timer.volume, "timer");
}

bool persistAlarmStorage() {
  return preferences.putBytes("alarms", &alarmStorage, sizeof(alarmStorage)) ==
         sizeof(alarmStorage);
}

bool persistTimerStorage() {
  return preferences.putBytes("timers", &timerStorage, sizeof(timerStorage)) ==
         sizeof(timerStorage);
}

void alarmAudioPath(uint32_t id, const char *suffix, char *path,
                    size_t pathSize) {
  snprintf(path, pathSize, "/alarm_%lu%s", static_cast<unsigned long>(id), suffix);
}

void timerAudioPath(uint32_t id, const char *suffix, char *path,
                    size_t pathSize) {
  snprintf(path, pathSize, "/timer_%lu%s", static_cast<unsigned long>(id), suffix);
}

bool hasTimerAudio(uint32_t id) {
  if (!fileSystemReady) return false;
  char path[32];
  timerAudioPath(id, ".wav", path, sizeof(path));
  return LittleFS.exists(path);
}

bool hasAlarmAudio(uint32_t id) {
  if (!fileSystemReady) {
    return false;
  }
  char path[32];
  alarmAudioPath(id, ".wav", path, sizeof(path));
  return LittleFS.exists(path);
}

int findAlarmIndex(uint32_t id) {
  for (uint8_t i = 0; i < alarmStorage.count; ++i) {
    if (alarmStorage.alarms[i].id == id) {
      return i;
    }
  }
  return -1;
}

bool loadAlarmStorage() {
  if (!preferences.begin("speakerAlarm", false)) {
    Serial.println("Could not open alarm preferences.");
    return false;
  }

  const size_t storedSize = preferences.getBytesLength("alarms");
  if (storedSize == 0) {
    alarmStorage.version = 3;
    alarmStorage.count = 0;
    alarmStorage.nextId = 1;
    if (!persistAlarmStorage()) {
      Serial.println("Could not initialize alarm preferences.");
      return false;
    }
    return true;
  }

  if (storedSize > sizeof(alarmStorage)) {
    Serial.println("Stored alarms use an unsupported format.");
    return false;
  }
  memset(&alarmStorage, 0, sizeof(alarmStorage));
  if (preferences.getBytes("alarms", &alarmStorage, sizeof(alarmStorage)) !=
      storedSize) {
    Serial.println("Could not read the stored alarms.");
    return false;
  }
  const uint8_t storedVersion = alarmStorage.version;

  if (storedVersion == 1 && storedSize == sizeof(LegacyAlarmStorage)) {
    LegacyAlarmStorage legacy = {};
    memcpy(&legacy, &alarmStorage, sizeof(legacy));
    if (legacy.version != 1 || legacy.count > MAX_ALARMS) {
      Serial.println("Stored alarms are invalid.");
      return false;
    }

    alarmStorage = {};
    alarmStorage.version = 3;
    alarmStorage.count = legacy.count;
    alarmStorage.nextId = 1;
    for (uint8_t i = 0; i < legacy.count; ++i) {
      memcpy(alarmStorage.alarms[i].name, legacy.alarms[i].name,
             sizeof(alarmStorage.alarms[i].name));
      alarmStorage.alarms[i].secondsOfDay =
          static_cast<uint32_t>(legacy.alarms[i].minuteOfDay) * 60;
      alarmStorage.alarms[i].volume = legacy.alarms[i].volume;
      alarmStorage.alarms[i].daysMask = legacy.alarms[i].daysMask;
      alarmStorage.alarms[i].id = alarmStorage.nextId++;
    }
    if (!persistAlarmStorage()) {
      Serial.println("Could not migrate stored alarms.");
      return false;
    }
    Serial.println("Migrated stored alarms to seconds-enabled format.");
    return true;
  }

  if (storedVersion == 2 && storedSize == sizeof(LegacyAlarmStorageV2)) {
    LegacyAlarmStorageV2 legacy = {};
    memcpy(&legacy, &alarmStorage, sizeof(legacy));
    if (legacy.version != 2 || legacy.count > MAX_ALARMS ||
        legacy.nextId == 0) {
      Serial.println("Stored alarms are invalid.");
      return false;
    }

    alarmStorage = {};
    alarmStorage.version = 3;
    alarmStorage.count = legacy.count;
    alarmStorage.nextId = legacy.nextId;
    for (uint8_t i = 0; i < legacy.count; ++i) {
      memcpy(alarmStorage.alarms[i].name, legacy.alarms[i].name,
             sizeof(alarmStorage.alarms[i].name));
      alarmStorage.alarms[i].secondsOfDay =
          static_cast<uint32_t>(legacy.alarms[i].minuteOfDay) * 60;
      alarmStorage.alarms[i].volume = legacy.alarms[i].volume;
      alarmStorage.alarms[i].daysMask = legacy.alarms[i].daysMask;
      alarmStorage.alarms[i].id = legacy.alarms[i].id;
    }
    if (!persistAlarmStorage()) {
      Serial.println("Could not migrate stored alarms.");
      return false;
    }
    Serial.println("Migrated stored alarms to seconds-enabled format.");
    return true;
  }

  if (storedVersion != 3 || storedSize != sizeof(alarmStorage) ||
      alarmStorage.version != 3 || alarmStorage.count > MAX_ALARMS ||
      alarmStorage.nextId == 0) {
    Serial.println("Stored alarms are invalid or use an unsupported format.");
    return false;
  }
  for (uint8_t i = 0; i < alarmStorage.count; ++i) {
    if (alarmStorage.alarms[i].secondsOfDay >= 24U * 60 * 60) {
      Serial.println("Stored alarm time is invalid.");
      return false;
    }
  }
  return true;
}

bool loadTimerStorage() {
  const size_t storedSize = preferences.getBytesLength("timers");
  if (storedSize == 0) {
    timerStorage.version = 1;
    timerStorage.count = 0;
    timerStorage.nextId = 1;
    return persistTimerStorage();
  }
  if (storedSize != sizeof(timerStorage)) {
    Serial.println("Stored timers use an unsupported format.");
    return false;
  }
  memset(&timerStorage, 0, sizeof(timerStorage));
  if (preferences.getBytes("timers", &timerStorage, sizeof(timerStorage)) !=
      storedSize) {
    Serial.println("Could not read the stored timers.");
    return false;
  }
  if (timerStorage.version != 1 || timerStorage.count > MAX_TIMERS ||
      timerStorage.nextId == 0) {
    Serial.println("Stored timers are invalid.");
    return false;
  }
  for (uint8_t i = 0; i < timerStorage.count; ++i) {
    Timer &timer = timerStorage.timers[i];
    if (timer.durationSeconds == 0 ||
        timer.durationSeconds > 24UL * 60 * 60 ||
        timer.remainingSeconds > timer.durationSeconds ||
        timer.volume < 1 || timer.volume > 100 ||
        timer.running > 1) {
      Serial.println("Stored timer data is invalid.");
      return false;
    }
    timer.remainingSeconds = timer.remainingSeconds > 0
                              ? timer.remainingSeconds
                              : timer.durationSeconds;
    timer.running = 1;
    timer.deadlineMillis = millis() + timer.remainingSeconds * 1000UL;
  }
  return persistTimerStorage();
}

String escapeJson(const char *text) {
  String escaped;
  for (const char *p = text; *p; ++p) {
    if (*p == '"' || *p == '\\') {
      escaped += '\\';
    }
    escaped += *p;
  }
  return escaped;
}

void sendAlarmList() {
  String response = "{\"clockReady\":";
  response += clockReady ? "true" : "false";
  response += ",\"alarms\":[";
  for (uint8_t i = 0; i < alarmStorage.count; ++i) {
    if (i > 0) {
      response += ',';
    }
    const Alarm &alarm = alarmStorage.alarms[i];
    char timeText[9];
    snprintf(timeText, sizeof(timeText), "%02u:%02u:%02u",
             alarm.secondsOfDay / 3600,
             (alarm.secondsOfDay / 60) % 60, alarm.secondsOfDay % 60);
    response += "{\"id\":";
    response += String(static_cast<unsigned long>(alarm.id));
    response += ",\"name\":\"";
    response += escapeJson(alarm.name);
    response += "\",\"time\":\"";
    response += timeText;
    response += "\",\"volume\":";
    response += String(alarm.volume);
    response += ",\"hasAudio\":";
    response += hasAlarmAudio(alarm.id) ? "true" : "false";
    response += ",\"days\":[";
    for (uint8_t day = 0; day < 7; ++day) {
      if (day > 0) {
        response += ',';
      }
      response += (alarm.daysMask & (1U << day)) ? "true" : "false";
    }
    response += "]}";
  }
  response += "]}";
  server.send(200, "application/json", response);
}

int findTimerIndex(uint32_t id) {
  for (uint8_t i = 0; i < timerStorage.count; ++i) {
    if (timerStorage.timers[i].id == id) return i;
  }
  return -1;
}

uint32_t timerRemaining(const Timer &timer) {
  if (!timer.running) return timer.remainingSeconds;
  const uint32_t now = millis();
  if ((int32_t)(timer.deadlineMillis - now) <= 0) return 0;
  const uint32_t leftMillis = timer.deadlineMillis - now;
  return (leftMillis + 999) / 1000;
}

String formatDuration(uint32_t seconds) {
  char text[9];
  snprintf(text, sizeof(text), "%02u:%02u:%02u",
           seconds / 3600, (seconds / 60) % 60, seconds % 60);
  return String(text);
}

void sendTimerList() {
  String response = "{\"timers\":[";
  for (uint8_t i = 0; i < timerStorage.count; ++i) {
    if (i > 0) response += ',';
    Timer &timer = timerStorage.timers[i];
    if (timer.running) {
      const uint32_t remaining = timerRemaining(timer);
      if (remaining == 0) timer.remainingSeconds = 0;
      else timer.remainingSeconds = remaining;
    }
    response += "{\"id\":";
    response += String(static_cast<unsigned long>(timer.id));
    response += ",\"name\":\"";
    response += escapeJson(timer.name);
    response += "\",\"duration\":\"";
    response += formatDuration(timer.durationSeconds);
    response += "\",\"remaining\":\"";
    response += formatDuration(timer.remainingSeconds);
    response += "\",\"volume\":";
    response += String(timer.volume);
    response += ",\"hasAudio\":";
    response += hasTimerAudio(timer.id) ? "true" : "false";
    response += ",\"running\":";
    response += timer.running ? "true" : "false";
    response += "}";
  }
  response += "]}";
  server.send(200, "application/json", response);
}

bool parseAlarmTime(const String &value, uint32_t &secondsOfDay) {
  const bool hasSeconds = value.length() == 8;
  if ((value.length() != 5 && !hasSeconds) || value[2] != ':' ||
      (hasSeconds && value[5] != ':') ||
      !isDigit(value[0]) || !isDigit(value[1]) ||
      !isDigit(value[3]) || !isDigit(value[4]) ||
      (hasSeconds && (!isDigit(value[6]) || !isDigit(value[7])))) {
    return false;
  }
  const uint8_t hours = (value[0] - '0') * 10 + value[1] - '0';
  const uint8_t minutes = (value[3] - '0') * 10 + value[4] - '0';
  const uint8_t seconds =
      hasSeconds ? (value[6] - '0') * 10 + value[7] - '0' : 0;
  if (hours > 23 || minutes > 59 || seconds > 59) {
    return false;
  }
  secondsOfDay = hours * 3600UL + minutes * 60UL + seconds;
  return true;
}

bool parseTimerDuration(const String &value, uint32_t &durationSeconds) {
  uint32_t hours;
  uint32_t minutes;
  uint32_t seconds;
  if (value.length() == 8 && value[2] == ':' && value[5] == ':' &&
      isDigit(value[0]) && isDigit(value[1]) && isDigit(value[3]) &&
      isDigit(value[4]) && isDigit(value[6]) && isDigit(value[7])) {
    hours = (value[0] - '0') * 10 + value[1] - '0';
    minutes = (value[3] - '0') * 10 + value[4] - '0';
    seconds = (value[6] - '0') * 10 + value[7] - '0';
  } else if (value.length() == 5 && value[2] == ':' &&
             isDigit(value[0]) && isDigit(value[1]) &&
             isDigit(value[3]) && isDigit(value[4])) {
    hours = 0;
    minutes = (value[0] - '0') * 10 + value[1] - '0';
    seconds = (value[3] - '0') * 10 + value[4] - '0';
  } else {
    return false;
  }
  if (hours > 24 || minutes > 59 || seconds > 59 ||
      (hours == 24 && (minutes != 0 || seconds != 0))) return false;
  durationSeconds = hours * 3600UL + minutes * 60UL + seconds;
  return durationSeconds > 0;
}

bool parseAlarmId(const String &value, uint32_t &id) {
  if (value.length() == 0) return false;
  for (size_t i = 0; i < value.length(); ++i) {
    if (!isDigit(value[i])) return false;
  }
  const unsigned long parsed = strtoul(value.c_str(), nullptr, 10);
  if (parsed == 0 || parsed > UINT32_MAX) return false;
  id = static_cast<uint32_t>(parsed);
  return true;
}

bool parseAlarmInput(Alarm &alarm, String &error) {
  String name = server.arg("name");
  name.trim();
  const String time = server.arg("time");
  const String volumeText = server.arg("volume");
  uint8_t daysMask = 0;
  bool foundDay = false;
  for (int i = 0; i < server.args(); ++i) {
    if (server.argName(i) == "day") {
      const String value = server.arg(i);
      if (value.length() != 1 || value[0] < '0' || value[0] > '6') {
        error = "Invalid selected day.";
        return false;
      }
      daysMask |= 1U << (value[0] - '0');
      foundDay = true;
    }
  }
  uint32_t secondsOfDay;
  if (name.length() == 0 ||
      name.length() >= sizeof(alarmStorage.alarms[0].name)) {
    error = "Enter a name with 1 to 31 characters.";
    return false;
  }
  for (size_t i = 0; i < name.length(); ++i) {
    if (static_cast<uint8_t>(name[i]) < 0x20) {
      error = "The alarm name contains an invalid character.";
      return false;
    }
  }
  if (!parseAlarmTime(time, secondsOfDay)) {
    error = "Enter a valid alarm time.";
    return false;
  }
  if (volumeText.length() == 0) {
    error = "Choose an alarm volume.";
    return false;
  }
  for (size_t i = 0; i < volumeText.length(); ++i) {
    if (!isDigit(volumeText[i])) {
      error = "Choose an alarm volume from 1 to 100.";
      return false;
    }
  }
  const long volume = volumeText.toInt();
  if (volume < 1 || volume > 100) {
    error = "Choose an alarm volume from 1 to 100.";
    return false;
  }
  if (!foundDay) {
    error = "Select at least one day.";
    return false;
  }

  memset(&alarm, 0, sizeof(alarm));
  memcpy(alarm.name, name.c_str(), name.length());
  alarm.secondsOfDay = secondsOfDay;
  alarm.volume = volume;
  alarm.daysMask = daysMask;
  return true;
}

void getAlarmsHandler() {
  if (!preferencesReady) {
    server.send(503, "text/plain", "Alarm storage is unavailable.");
    return;
  }
  sendAlarmList();
}

void createAlarmHandler() {
  if (!preferencesReady) {
    server.send(503, "text/plain", "Alarm storage is unavailable.");
    return;
  }
  if (alarmStorage.count >= MAX_ALARMS ||
      alarmStorage.nextId == UINT32_MAX) {
    server.send(400, "text/plain", "Alarm limit reached.");
    return;
  }

  Alarm alarm;
  String error;
  if (!parseAlarmInput(alarm, error)) {
    server.send(400, "text/plain", error);
    return;
  }

  const AlarmStorage previous = alarmStorage;
  alarm.id = alarmStorage.nextId++;
  alarmStorage.alarms[alarmStorage.count++] = alarm;
  if (!persistAlarmStorage()) {
    alarmStorage = previous;
    server.send(500, "text/plain", "Could not save the alarm.");
    return;
  }
  const String response =
      String("{\"id\":") + String(static_cast<unsigned long>(alarm.id)) + "}";
  server.send(201, "application/json", response);
}

void updateAlarmHandler() {
  if (!preferencesReady) {
    server.send(503, "text/plain", "Alarm storage is unavailable.");
    return;
  }
  uint32_t id;
  if (!parseAlarmId(server.arg("id"), id)) {
    server.send(400, "text/plain", "Missing or invalid alarm id.");
    return;
  }
  const int index = findAlarmIndex(id);
  if (index < 0) {
    server.send(404, "text/plain", "Alarm not found.");
    return;
  }
  Alarm updated;
  String error;
  if (!parseAlarmInput(updated, error)) {
    server.send(400, "text/plain", error);
    return;
  }
  updated.id = id;
  const Alarm previous = alarmStorage.alarms[index];
  alarmStorage.alarms[index] = updated;
  if (!persistAlarmStorage()) {
    alarmStorage.alarms[index] = previous;
    server.send(500, "text/plain", "Could not save the alarm.");
    return;
  }
  lastFiredDay[index] = -1;
  server.send(200, "text/plain", "Alarm updated.");
}

void deleteAlarmHandler() {
  if (!preferencesReady) {
    server.send(503, "text/plain", "Alarm storage is unavailable.");
    return;
  }
  uint32_t id;
  if (!parseAlarmId(server.arg("id"), id)) {
    server.send(400, "text/plain", "Missing or invalid alarm id.");
    return;
  }
  const int index = findAlarmIndex(id);
  if (index < 0) {
    server.send(404, "text/plain", "Alarm not found.");
    return;
  }

  const AlarmStorage previous = alarmStorage;
  for (uint8_t i = index; i + 1 < alarmStorage.count; ++i) {
    alarmStorage.alarms[i] = alarmStorage.alarms[i + 1];
  }
  memset(&alarmStorage.alarms[alarmStorage.count - 1], 0, sizeof(Alarm));
  --alarmStorage.count;
  if (!persistAlarmStorage()) {
    alarmStorage = previous;
    server.send(500, "text/plain", "Could not delete the alarm.");
    return;
  }
  for (size_t i = 0; i < MAX_ALARMS; ++i) lastFiredDay[i] = -1;

  if (fileSystemReady) {
    const char *suffixes[] = {".wav", ".pcm"};
    for (const char *suffix : suffixes) {
      char path[32];
      alarmAudioPath(id, suffix, path, sizeof(path));
      if (LittleFS.exists(path) && !LittleFS.remove(path)) {
        Serial.println("Could not remove deleted alarm audio from flash.");
        server.send(500, "text/plain",
                    "Alarm deleted, but its audio file could not be removed.");
        return;
      }
    }
  }
  server.send(200, "text/plain", "Alarm deleted.");
}

void getTimersHandler() {
  if (!preferencesReady) {
    server.send(503, "text/plain", "Timer storage is unavailable.");
    return;
  }
  sendTimerList();
}

bool parseTimerInput(Timer &timer, String &error) {
  String name = server.arg("name");
  name.trim();
  if (name.length() == 0 || name.length() >= sizeof(timer.name)) {
    error = "Enter a name with 1 to 31 characters.";
    return false;
  }
  for (size_t i = 0; i < name.length(); ++i) {
    if (static_cast<uint8_t>(name[i]) < 0x20) {
      error = "The timer name contains an invalid character.";
      return false;
    }
  }
  const String durationText = server.arg("duration");
  uint32_t durationSeconds;
  if (!parseTimerDuration(durationText, durationSeconds)) {
    error = "Enter a valid timer duration.";
    return false;
  }
  const String volumeText = server.arg("volume");
  for (size_t i = 0; i < volumeText.length(); ++i) {
    if (!isDigit(volumeText[i])) {
      error = "Choose a timer volume from 1 to 100.";
      return false;
    }
  }
  const long volume = volumeText.toInt();
  if (volumeText.length() == 0 || volume < 1 || volume > 100) {
    error = "Choose a timer volume from 1 to 100.";
    return false;
  }
  memset(&timer, 0, sizeof(timer));
  memcpy(timer.name, name.c_str(), name.length());
  timer.durationSeconds = durationSeconds;
  timer.remainingSeconds = durationSeconds;
  timer.volume = static_cast<uint8_t>(volume);
  return true;
}

void createTimerHandler() {
  if (!preferencesReady) {
    server.send(503, "text/plain", "Timer storage is unavailable.");
    return;
  }
  if (timerStorage.count >= MAX_TIMERS ||
      timerStorage.nextId == UINT32_MAX) {
    server.send(400, "text/plain", "Timer limit reached.");
    return;
  }
  Timer timer;
  String error;
  if (!parseTimerInput(timer, error)) {
    server.send(400, "text/plain", error);
    return;
  }
  const TimerStorage previous = timerStorage;
  timer.id = timerStorage.nextId++;
  timer.running = 1;
  timer.deadlineMillis = millis() + timer.durationSeconds * 1000UL;
  timerStorage.timers[timerStorage.count++] = timer;
  if (!persistTimerStorage()) {
    timerStorage = previous;
    server.send(500, "text/plain", "Could not save the timer.");
    return;
  }
  const String response =
      String("{\"id\":") + String(static_cast<unsigned long>(timer.id)) + "}";
  server.send(201, "application/json", response);
}

void updateTimerHandler() {
  if (!preferencesReady) {
    server.send(503, "text/plain", "Timer storage is unavailable.");
    return;
  }
  uint32_t id;
  if (!parseAlarmId(server.arg("id"), id)) {
    server.send(400, "text/plain", "Missing or invalid timer id.");
    return;
  }
  const int index = findTimerIndex(id);
  if (index < 0) {
    server.send(404, "text/plain", "Timer not found.");
    return;
  }
  const String action = server.arg("action");
  Timer &timer = timerStorage.timers[index];
  if (action.length() > 0) {
    Timer previous = timer;
    if (action == "start") {
      if (!timer.running) {
        if (timer.remainingSeconds == 0)
          timer.remainingSeconds = timer.durationSeconds;
        timer.deadlineMillis =
            millis() + timer.remainingSeconds * 1000UL;
        timer.running = 1;
      }
    } else if (action == "pause") {
      if (timer.running) {
        timer.remainingSeconds = timerRemaining(timer);
        timer.running = 0;
        timer.deadlineMillis = 0;
      }
    } else if (action == "reset") {
      timer.remainingSeconds = timer.durationSeconds;
      timer.running = 1;
      timer.deadlineMillis = millis() + timer.durationSeconds * 1000UL;
    } else {
      server.send(400, "text/plain", "Invalid timer action.");
      return;
    }
    if (!persistTimerStorage()) {
      timer = previous;
      server.send(500, "text/plain", "Could not update the timer.");
      return;
    }
    server.send(200, "text/plain", "Timer updated.");
    return;
  }
  Timer updated;
  String error;
  if (!parseTimerInput(updated, error)) {
    server.send(400, "text/plain", error);
    return;
  }
  updated.id = id;
  const Timer previous = timer;
  updated.running = 1;
  updated.remainingSeconds = updated.durationSeconds;
  updated.deadlineMillis = millis() + updated.durationSeconds * 1000UL;
  timer = updated;
  if (!persistTimerStorage()) {
    timer = previous;
    server.send(500, "text/plain", "Could not save the timer.");
    return;
  }
  server.send(200, "text/plain", "Timer updated.");
}

void deleteTimerHandler() {
  if (!preferencesReady) {
    server.send(503, "text/plain", "Timer storage is unavailable.");
    return;
  }
  uint32_t id;
  if (!parseAlarmId(server.arg("id"), id)) {
    server.send(400, "text/plain", "Missing or invalid timer id.");
    return;
  }
  const int index = findTimerIndex(id);
  if (index < 0) {
    server.send(404, "text/plain", "Timer not found.");
    return;
  }
  const TimerStorage previous = timerStorage;
  for (uint8_t i = index; i + 1 < timerStorage.count; ++i)
    timerStorage.timers[i] = timerStorage.timers[i + 1];
  memset(&timerStorage.timers[timerStorage.count - 1], 0, sizeof(Timer));
  --timerStorage.count;
  if (!persistTimerStorage()) {
    timerStorage = previous;
    server.send(500, "text/plain", "Could not delete the timer.");
    return;
  }
  if (fileSystemReady) {
    const char *suffixes[] = {".wav", ".tmp", ".bak"};
    for (const char *suffix : suffixes) {
      char path[32];
      timerAudioPath(id, suffix, path, sizeof(path));
      if (LittleFS.exists(path)) LittleFS.remove(path);
    }
  }
  server.send(200, "text/plain", "Timer deleted.");
}

void failAudioUpload(const String &message, int statusCode = 500) {
  audioUploadFailed = true;
  audioUploadComplete = false;
  audioUploadError = message;
  audioUploadStatus = statusCode;
  if (audioUploadFile) audioUploadFile.close();
  if (audioUploadId != 0) {
    char temporaryPath[32];
    if (audioUploadIsTimer)
      timerAudioPath(audioUploadId, ".tmp", temporaryPath, sizeof(temporaryPath));
    else
      alarmAudioPath(audioUploadId, ".tmp", temporaryPath, sizeof(temporaryPath));
    LittleFS.remove(temporaryPath);
  }
}

void handleAudioUpload() {
  HTTPUpload &upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    audioUploadStarted = true;
    audioUploadComplete = false;
    audioUploadFailed = false;
    audioUploadError = "";
    audioUploadStatus = 500;
    audioUploadId = 0;
    audioUploadIsTimer = server.arg("type") == "timer";
    audioUploadBytes = 0;
    if (!fileSystemReady) {
      failAudioUpload("Audio storage is unavailable.", 503);
      return;
    }
    uint32_t id;
    if (!parseAlarmId(server.arg("id"), id)) {
      failAudioUpload("Missing or invalid alarm id.", 400);
      return;
    }
    if (audioUploadIsTimer) {
      if (findTimerIndex(id) < 0) {
        failAudioUpload("Timer not found.", 404);
        return;
      }
    } else if (findAlarmIndex(id) < 0) {
      failAudioUpload("Alarm not found.", 404);
      return;
    }
    audioUploadId = id;
    char temporaryPath[32];
    if (audioUploadIsTimer)
      timerAudioPath(id, ".tmp", temporaryPath, sizeof(temporaryPath));
    else
      alarmAudioPath(id, ".tmp", temporaryPath, sizeof(temporaryPath));
    LittleFS.remove(temporaryPath);
    audioUploadFile = LittleFS.open(temporaryPath, "w");
    if (!audioUploadFile) {
      failAudioUpload("Could not create a temporary audio file.");
    }
    return;
  }

  if (upload.status == UPLOAD_FILE_WRITE) {
    if (audioUploadFailed || !audioUploadFile) return;
    const size_t freeBytes = LittleFS.totalBytes() - LittleFS.usedBytes();
    if (freeBytes < upload.currentSize + AUDIO_UPLOAD_RESERVE_BYTES) {
      failAudioUpload("Not enough flash space for this WAV file.", 507);
      return;
    }
    if (audioUploadFile.write(upload.buf, upload.currentSize) !=
        upload.currentSize) {
      failAudioUpload("Could not write the WAV file to flash.");
      return;
    }
    audioUploadBytes += upload.currentSize;
    return;
  }

  if (upload.status == UPLOAD_FILE_ABORTED) {
    failAudioUpload("WAV upload was interrupted.");
    return;
  }

  if (upload.status != UPLOAD_FILE_END) return;
  if (audioUploadFile) audioUploadFile.close();
  if (audioUploadFailed) return;

  char finalPath[32];
  char temporaryPath[32];
  char backupPath[32];
  if (audioUploadIsTimer) {
    timerAudioPath(audioUploadId, ".wav", finalPath, sizeof(finalPath));
    timerAudioPath(audioUploadId, ".tmp", temporaryPath, sizeof(temporaryPath));
    timerAudioPath(audioUploadId, ".bak", backupPath, sizeof(backupPath));
  } else {
    alarmAudioPath(audioUploadId, ".wav", finalPath, sizeof(finalPath));
    alarmAudioPath(audioUploadId, ".tmp", temporaryPath, sizeof(temporaryPath));
    alarmAudioPath(audioUploadId, ".bak", backupPath, sizeof(backupPath));
  }
  File uploadedAudio = LittleFS.open(temporaryPath, "r");
  WavInfo wav = {};
  const bool validWav =
      uploadedAudio && uploadedAudio.size() == audioUploadBytes &&
      readWavInfo(uploadedAudio, wav);
  if (uploadedAudio) uploadedAudio.close();
  if (!validWav) {
    failAudioUpload(
        "Unsupported WAV. Use PCM, 8/16-bit mono or stereo, 8-48 kHz.", 400);
    return;
  }

  const bool hadPrevious = LittleFS.exists(finalPath);
  if (LittleFS.exists(backupPath) && !LittleFS.remove(backupPath)) {
    failAudioUpload("Could not clear the previous audio backup.");
    return;
  }
  if (hadPrevious && !LittleFS.rename(finalPath, backupPath)) {
    failAudioUpload("Could not preserve the previous alarm audio.");
    return;
  }
  if (!LittleFS.rename(temporaryPath, finalPath)) {
    if (hadPrevious) LittleFS.rename(backupPath, finalPath);
    failAudioUpload("Could not save the new WAV file.");
    return;
  }
  if (hadPrevious && !LittleFS.remove(backupPath)) {
    Serial.println("Could not remove the previous alarm audio backup.");
  }
  audioUploadComplete = true;
}

void finishAudioUpload() {
  const bool successful = audioUploadStarted && audioUploadComplete &&
                          !audioUploadFailed;
  const int status = successful ? 201 : audioUploadStatus;
  const String message = successful ? "WAV file saved."
                                    : (audioUploadError.length()
                                           ? audioUploadError
                                           : "No WAV file was uploaded.");
  server.send(status, "text/plain", message);
  audioUploadStarted = false;
  audioUploadComplete = false;
  audioUploadFailed = false;
  audioUploadId = 0;
  audioUploadIsTimer = false;
  audioUploadBytes = 0;
  audioUploadError = "";
}

void checkTimers() {
  if (!preferencesReady || !audioReady) return;
  for (uint8_t i = 0; i < timerStorage.count; ++i) {
    Timer &timer = timerStorage.timers[i];
    if (!timer.running) continue;
    if (timerRemaining(timer) != 0) continue;
    timer.running = 0;
    timer.remainingSeconds = 0;
    timer.deadlineMillis = 0;
    persistTimerStorage();
    Serial.printf("Timer '%s' is finished.\n", timer.name);
    const bool played = hasTimerAudio(timer.id)
                            ? playTimerAudio(timer)
                            : playAlarmTone(timer.volume);
    if (!played) {
      Serial.println("Timer audio playback failed.");
    }
  }
}

void connectRoutes() {
  server.on("/", HTTP_GET, []() {
    server.send_P(200, "text/html; charset=utf-8", PAGE);
  });
  server.on("/api/alarms", HTTP_GET, getAlarmsHandler);
  server.on("/api/alarms", HTTP_POST, createAlarmHandler);
  server.on("/api/alarms", HTTP_PUT, updateAlarmHandler);
  server.on("/api/alarms", HTTP_DELETE, deleteAlarmHandler);
  server.on("/api/timers", HTTP_GET, getTimersHandler);
  server.on("/api/timers", HTTP_POST, createTimerHandler);
  server.on("/api/timers", HTTP_PUT, updateTimerHandler);
  server.on("/api/timers", HTTP_DELETE, deleteTimerHandler);
  server.on("/api/audio", HTTP_POST, finishAudioUpload, handleAudioUpload);
  server.onNotFound([]() {
    server.send(404, "text/plain", "Not found.");
  });
}

void checkAlarms() {
  if (!preferencesReady || !audioReady) {
    return;
  }

  const time_t now = time(nullptr);
  if (now < 1704067200) {
    clockReady = false;
    return;
  }
  clockReady = true;

  struct tm localTime;
  if (localtime_r(&now, &localTime) == nullptr) {
    Serial.println("Could not convert current time.");
    return;
  }

  const uint32_t secondsOfDay =
      localTime.tm_hour * 3600UL + localTime.tm_min * 60UL + localTime.tm_sec;
  const uint8_t today = localTime.tm_wday;
  const int dayKey = (localTime.tm_year + 1900) * 366 + localTime.tm_yday;
  for (uint8_t i = 0; i < alarmStorage.count; ++i) {
    Alarm &alarm = alarmStorage.alarms[i];
    if (alarm.secondsOfDay != secondsOfDay ||
        !(alarm.daysMask & (1U << today)) ||
        lastFiredDay[i] == dayKey) {
      continue;
    }

    lastFiredDay[i] = dayKey;
    Serial.printf("Alarm '%s' is playing.\n", alarm.name);
    const bool played = hasAlarmAudio(alarm.id)
                            ? playAlarmAudio(alarm)
                            : playAlarmTone(alarm.volume);
    if (!played) {
      Serial.println("Alarm audio playback failed.");
    }
  }
}

void setup() {
  Serial.begin(115200);
  for (size_t i = 0; i < MAX_ALARMS; ++i) {
    lastFiredDay[i] = -1;
  }
  preferencesReady = loadAlarmStorage();
  if (preferencesReady && !loadTimerStorage()) {
    Serial.println("Timer storage initialization failed.");
  }
  fileSystemReady = LittleFS.begin(false);
  if (!fileSystemReady) {
    Serial.println("LittleFS mount failed; formatting the audio partition.");
    fileSystemReady = LittleFS.begin(true);
  }
  if (!fileSystemReady) {
    Serial.println("Could not mount LittleFS; alarm audio is unavailable.");
  }
  configTzTime(TIME_ZONE, "pool.ntp.org", "time.nist.gov");

  pinMode(AMP_SD, OUTPUT);
  digitalWrite(AMP_SD, HIGH);
  delay(50);

  i2s_config_t config = {};
  config.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_TX);
  config.sample_rate = SAMPLE_RATE;
  config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  config.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
  config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  config.dma_buf_count = 8;
  config.dma_buf_len = 256;
  config.use_apll = false;
  config.tx_desc_auto_clear = true;

  i2s_pin_config_t pinConfig = {};
  pinConfig.bck_io_num = I2S_BCLK;
  pinConfig.ws_io_num = I2S_LRC;
  pinConfig.data_out_num = I2S_DIN;
  pinConfig.data_in_num = I2S_PIN_NO_CHANGE;

  if (i2s_driver_install(I2S_NUM_0, &config, 0, nullptr) != ESP_OK ||
      i2s_set_pin(I2S_NUM_0, &pinConfig) != ESP_OK) {
    Serial.println("I2S setup failed.");
    return;
  }
  audioReady = true;

  connectRoutes();
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("speakeralarm");
  WiFi.onEvent(handleWiFiEvent);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  lastReconnectAttempt = millis();
  Serial.println("Connecting to Wi-Fi...");
}

void loop() {
  if (WiFi.status() == WL_CONNECTED) {
    if (!serverStarted) {
      server.begin();
      serverStarted = true;
    }

    const IPAddress currentIp = WiFi.localIP();
    if (currentIp != reportedIp) {
      reportedIp = currentIp;
      Serial.print("Alarm page by IP: http://");
      Serial.print(currentIp);
      Serial.println("/");
      Serial.println("Use this IP if speakeralarm.local does not resolve.");
    }

    if (!mdnsStarted &&
        (lastMdnsAttempt == 0 || millis() - lastMdnsAttempt >= 10000)) {
      lastMdnsAttempt = millis();
      if (MDNS.begin("speakeralarm")) {
        if (MDNS.addService("http", "tcp", 80)) {
          mdnsStarted = true;
          Serial.println("mDNS ready: http://speakeralarm.local/");
        } else {
          MDNS.end();
          Serial.println("mDNS HTTP service announcement failed; will retry.");
        }
      } else {
        Serial.println("mDNS startup failed; will retry.");
      }
    }
    wasWifiConnected = true;
    server.handleClient();
  } else {
    if (wasWifiConnected) {
      Serial.println("Wi-Fi connection lost; restarting network services on reconnect.");
      if (serverStarted) {
        server.stop();
        serverStarted = false;
      }
      reportedIp = IPAddress();
      wasWifiConnected = false;
    }
    if (mdnsStarted) {
      MDNS.end();
      mdnsStarted = false;
      lastMdnsAttempt = 0;
    }

    const uint32_t now = millis();
    if (wifiAssociated && !wifiHasIp &&
        now - wifiIpWaitStartedAt >= 60000) {
      Serial.println("No DHCP address after 60 seconds; restarting Wi-Fi association.");
      wifiIpWaitStartedAt = now;
      lastReconnectAttempt = now;
      WiFi.reconnect();
    } else if (!wifiAssociated && now - lastReconnectAttempt >= 10000) {
      lastReconnectAttempt = now;
      Serial.println("Not associated with Wi-Fi; retrying connection.");
      if (!WiFi.reconnect()) {
        Serial.println("Wi-Fi reconnect request failed; will retry.");
      }
    }
  }
  checkAlarms();
  checkTimers();
}
