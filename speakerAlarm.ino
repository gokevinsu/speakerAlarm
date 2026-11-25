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
AlarmStorage alarmStorage = {};
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
    #alarmList { display:grid; grid-template-columns:1fr; gap:.75rem; }
    .alarm { background:var(--card); border:1px solid var(--line); border-radius:.7rem; overflow:hidden; }
    .alarm summary { display:flex; align-items:center; justify-content:space-between; gap:1rem; padding:1rem; cursor:pointer; list-style:none; }
    .alarm summary::-webkit-details-marker { display:none; }
    .alarm summary::after { content:""; width:.55rem; height:.55rem; border-right:2px solid var(--muted); border-bottom:2px solid var(--muted); transform:rotate(45deg); transition:transform .15s; flex:none; margin-right:.25rem; }
    .alarm[open] summary::after { transform:rotate(225deg); }
    .alarm-summary-main { display:grid; gap:.2rem; }
    .alarm h2 { margin:0; font-size:1.1rem; }
    .alarm-time { font-size:1.6rem; font-variant-numeric:tabular-nums; }
    .alarm-info { color:var(--muted); font-size:.9rem; }
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
    .alarm-editor { display:grid; gap:1rem; padding:1rem; border-top:1px solid var(--line); }
    .alarm-editor > label { display:grid; gap:.35rem; }
    .volume-line { display:flex; align-items:center; gap:.7rem; }
    .volume-line input { flex:1; accent-color:var(--control-accent); }
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
  <script>
    const root = document.documentElement;
    const list = document.getElementById('alarmList');
    const dialog = document.getElementById('alarmDialog');
    const form = document.getElementById('alarmForm');
    const clockStatus = document.getElementById('clockStatus');
    const dayNames = ['Sun','Mon','Tue','Wed','Thu','Fri','Sat'];
    const themeSwitch = document.getElementById('darkMode');
    let openAlarmId = null;

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

    loadAlarms().catch(error => { clockStatus.textContent = 'Could not load alarms: ' + error.message; });
    setInterval(() => {
      if (dialog.open || list.querySelector('.alarm[open]')) return;
      loadAlarms().catch(error => {
        clockStatus.textContent = 'Could not refresh alarms: ' + error.message;
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

bool playAlarmAudio(const Alarm &alarm) {
  if (!fileSystemReady) {
    return false;
  }
  char path[32];
  alarmAudioPath(alarm.id, ".wav", path, sizeof(path));
  File audio = LittleFS.open(path, "r");
  if (!audio) {
    Serial.println("Could not open alarm WAV file.");
    return false;
  }

  WavInfo wav = {};
  if (!readWavInfo(audio, wav)) {
    audio.close();
    Serial.println("Alarm WAV must be PCM, 8/16-bit mono/stereo at 8-48 kHz.");
    return false;
  }
  if (!audio.seek(wav.dataOffset) ||
      i2s_set_clk(I2S_NUM_0, wav.sampleRate, I2S_BITS_PER_SAMPLE_16BIT,
                  I2S_CHANNEL_MONO) != ESP_OK) {
    audio.close();
    Serial.println("Could not prepare I2S for the alarm WAV sample rate.");
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
      output[i] = static_cast<int16_t>(mixedSample * alarm.volume / 100);
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
    Serial.println("Could not restore the alarm audio sample rate.");
    success = false;
  }
  return success;
}

bool persistAlarmStorage() {
  return preferences.putBytes("alarms", &alarmStorage, sizeof(alarmStorage)) ==
         sizeof(alarmStorage);
}

void alarmAudioPath(uint32_t id, const char *suffix, char *path,
                    size_t pathSize) {
  snprintf(path, pathSize, "/alarm_%lu%s", static_cast<unsigned long>(id), suffix);
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

void failAudioUpload(const String &message, int statusCode = 500) {
  audioUploadFailed = true;
  audioUploadComplete = false;
  audioUploadError = message;
  audioUploadStatus = statusCode;
  if (audioUploadFile) audioUploadFile.close();
  if (audioUploadId != 0) {
    char temporaryPath[32];
    alarmAudioPath(audioUploadId, ".tmp", temporaryPath,
                   sizeof(temporaryPath));
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
    if (findAlarmIndex(id) < 0) {
      failAudioUpload("Alarm not found.", 404);
      return;
    }
    audioUploadId = id;
    char temporaryPath[32];
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
  alarmAudioPath(audioUploadId, ".wav", finalPath, sizeof(finalPath));
  alarmAudioPath(audioUploadId, ".tmp", temporaryPath, sizeof(temporaryPath));
  alarmAudioPath(audioUploadId, ".bak", backupPath, sizeof(backupPath));
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
  audioUploadBytes = 0;
  audioUploadError = "";
}

void connectRoutes() {
  server.on("/", HTTP_GET, []() {
    server.send_P(200, "text/html; charset=utf-8", PAGE);
  });
  server.on("/api/alarms", HTTP_GET, getAlarmsHandler);
  server.on("/api/alarms", HTTP_POST, createAlarmHandler);
  server.on("/api/alarms", HTTP_PUT, updateAlarmHandler);
  server.on("/api/alarms", HTTP_DELETE, deleteAlarmHandler);
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
}
