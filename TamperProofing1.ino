#include <WiFi.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <esp_partition.h>
#include <esp_ota_ops.h>
#include <esp_heap_caps.h>

// --- Core Internal ESP32 Temp Engine ---
#ifdef __cplusplus
extern "C" {
#endif
uint8_t temprature_sens_read(); 
#ifdef __cplusplus
}
#endif

// --- Configuration Settings ---
const char* AP_SSID = "ESP32_Secure_Node";
const char* AP_PASS = "FactorySecure123!"; 
const byte DNS_PORT = 53;
const int MAX_ALLOWED_DEVICES = 2;
const char* ROLLBACK_PASSWORD = "AdminRollback";
const float MAX_SAFE_TEMP = 80.0; 

// --- Security Baseline Blueprint ---
struct SystemSnapshot {
  uint8_t partition_hash[32];
  volatile uint32_t magnetic_tamper_count;
  bool is_compromised;
  String attack_reason;
};

SystemSnapshot baseline;
DNSServer dnsServer;
WebServer server(80);

WiFiClient streamClient; // Holds our active live streaming connection
bool isStreamActive = false;

// --- HTML Dashboard UI Layout (Stored in Flash) ---
const char DASHBOARD_HTML[] PROGMEM = R"=====(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8"><meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>ESP32 Real-Time Monitoring Panel</title>
    <style>
        :root { --bg-main: #0f172a; --bg-card: #1e293b; --text-main: #f8fafc; --text-muted: #94a3b8; --border: #334155; --bg-input: #0f172a; --accent: #3b82f6; }
        body { font-family: -apple-system, sans-serif; background: var(--bg-main); color: var(--text-main); padding: 20px; margin: 0; }
        .container { max-width: 800px; margin: 0 auto; }
        .grid { display: grid; grid-template-columns: 1fr 1fr; gap: 15px; }
        @media(max-width:768px){ .grid { grid-template-columns: 1fr; } }
        .card { background: var(--bg-card); padding: 20px; border-radius: 8px; margin-bottom: 15px; border: 1px solid var(--border); }
        .status-box { padding: 12px; border-radius: 6px; font-weight: bold; text-align: center; font-size: 18px; }
        .status-good { background: #065f46; color: #34d399; }
        .status-bad { background: #991b1b; color: #f87171; animation: pulse 1.5s infinite; }
        .metric-row { display: flex; justify-content: space-between; padding: 10px 0; border-bottom: 1px solid var(--border); }
        .metric-label { color: var(--text-muted); }
        .metric-value { font-family: monospace; font-weight: bold; }
        .btn { display: block; width: 100%; background: var(--accent); color: white; border: none; padding: 12px; border-radius: 6px; font-weight: bold; cursor: pointer; text-align: center; margin-top: 15px; box-sizing: border-box; }
        .btn-warn { background: #dc2626; margin-top: 10px; }
        .input-field { width: 100%; padding: 10px; border-radius: 6px; border: 1px solid var(--border); background: var(--bg-input); color: var(--text-main); margin-top: 5px; margin-bottom: 10px; box-sizing: border-box; }
        .terminal { background: #020617; border: 1px solid var(--border); border-radius: 6px; padding: 10px; font-family: monospace; font-size: 12px; height: 220px; overflow-y: auto; color: #38bdf8; white-space: pre-wrap; box-sizing: border-box; }
        @keyframes pulse { 0% { opacity: 1; } 50% { opacity: 0.5; } 100% { opacity: 1; } }
    </style>
</head>
<body>
    <div class="container">
        <div class="card"><div id="statusAlert" class="status-box status-good">AWAITING LIVE DATA STREAM...</div></div>
        <div class="grid">
            <div class="card">
                <h3>🌡️ Core Diagnostics</h3>
                <div class="metric-row"><span class="metric-label">Internal Core Temp</span><span id="tempVal" class="metric-value" style="color:#ef4444;">-- °C</span></div>
                <div class="metric-row"><span class="metric-label">Free Dynamic RAM</span><span id="ramVal" class="metric-value">-- bytes</span></div>
                <div class="metric-row"><span class="metric-label">Active Connections</span><span id="connVal" class="metric-value">--</span></div>
                <div class="metric-row"><span class="metric-label">Magnetic Flags</span><span id="magVal" class="metric-value">0</span></div>
            </div>
            <div class="card">
                <h3>🔍 Firmware Matrix</h3>
                <div class="metric-row"><span class="metric-label">Active Flash Partition</span><span id="partVal" class="metric-value">--</span></div>
                <div class="metric-row"><span class="metric-label">Running Hash Signature</span><span id="hashVal" class="metric-value">--</span></div>
                <label class="metric-label" for="passKey">Emergency Recovery Pin</label>
                <input type="password" id="passKey" class="input-field" placeholder="Verification code...">
                <button class="btn btn-warn" style="padding:8px;" onclick="triggerRollback()">Rollback Firmware</button>
            </div>
        </div>
        <div class="card">
            <h3>📡 Live Native Event & Network Packet Stream</h3>
            <div id="logTerminal" class="terminal">Connecting to native ESP32 stream channel...</div>
        </div>
    </div>
    <script>
        // Use native Server-Sent Events (SSE) to listen to the ESP32 data stream
        const eventSource = new EventSource('/stream');

        eventSource.onmessage = function(event) {
            let data = JSON.parse(event.data);
            
            // 1. Update UI Elements
            document.getElementById('tempVal').innerText = data.temp.toFixed(1) + " °C";
            document.getElementById('ramVal').innerText = data.free_ram.toLocaleString() + " bytes";
            document.getElementById('connVal').innerText = data.connections;
            document.getElementById('magVal').innerText = data.magnetic_events;
            document.getElementById('partVal').innerText = data.partition;
            document.getElementById('hashVal').innerText = data.hash.substring(0, 16) + "...";
            
            // 2. Handle Alarm Interface
            const alertBox = document.getElementById('statusAlert');
            if (data.compromised) {
                alertBox.innerText = "🚨 CRITICAL STATE: " + data.reason;
                alertBox.className = "status-box status-bad";
            } else {
                alertBox.innerText = "SYSTEM ACTIVE - TELEMETRY LOCK GOOD";
                alertBox.className = "status-box status-good";
            }

            // 3. Print Logs to Terminal
            if(data.log_msg) {
                let term = document.getElementById('logTerminal');
                term.innerText += "\n" + data.log_msg;
                term.scrollTop = term.scrollHeight;
            }
        };

        eventSource.onerror = function() {
            document.getElementById('logTerminal').innerText += "\n[System] Stream disconnected. Reconnecting...";
        };

        function triggerRollback() {
            const passValue = document.getElementById('passKey').value;
            if (!passValue) return;
            
            let formData = new FormData();
            formData.append("passphrase", passValue);
            
            fetch('/api/rollback', { method: 'POST', body: formData })
            .then(res => {
                if(res.status !== 200) alert("Access Denied or Rollback Failure.");
            });
        }
    </script>
</body>
</html>
)=====";

// --- Native Data Stream Pipe Function ---
void push_stream_frame(String logMessage) {
  if (!isStreamActive || !streamClient.connected()) {
    isStreamActive = false;
    return;
  }

  // Calculate live security parameters
  float raw_temp = (temprature_sens_read() - 32) / 1.8; 
  int current_stations = WiFi.softAPgetStationNum();
  size_t current_free_ram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  
  uint8_t current_hash[32];
  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_partition_get_sha256(running, current_hash);

  // Check Core Thermal Tripwire
  if (raw_temp > MAX_SAFE_TEMP && !baseline.is_compromised) {
    baseline.is_compromised = true;
    baseline.attack_reason = "THERMAL_OVERHEAT_LOCKDOWN";
    logMessage = "CRITICAL: Overheat protection triggered!";
  }

  // Check Firmware Code Injection Changes
  if (memcmp(current_hash, baseline.partition_hash, 32) != 0 && !baseline.is_compromised) {
    baseline.is_compromised = true;
    baseline.attack_reason = "FIRMWARE_TAMPER_FLAGGED";
    logMessage = "CRITICAL: Firmware sketch footprint modified!";
  }

  String hex_hash = "";
  for (int i = 0; i < 32; i++) {
    if (current_hash[i] < 0x10) hex_hash += "0";
    hex_hash += String(current_hash[i], HEX);
  }

  // Format data as a clean JSON layout inside an SSE data block
  String json = "data: {";
  json += "\"temp\":" + String(raw_temp) + ",";
  json += "\"free_ram\":" + String(current_free_ram) + ",";
  json += "\"connections\":" + String(current_stations) + ",";
  json += "\"magnetic_events\":" + String(baseline.magnetic_tamper_count) + ",";
  json += "\"partition\":\"" + String(running->label) + "\",";
  json += "\"hash\":\"" + hex_hash + "\",";
  json += "\"compromised\":" + String(baseline.is_compromised ? "true" : "false") + ",";
  json += "\"reason\":\"" + baseline.attack_reason + "\"";
  if (logMessage.length() > 0) {
    json += ",\"log_msg\":\"[" + String(millis()/1000) + "s] " + logMessage + "\"";
  }
  json += "}\n\n"; // SSE streams require two newlines to separate packets

  streamClient.print(json);
  streamClient.flush();

  if (baseline.attack_reason == "THERMAL_OVERHEAT_LOCKDOWN") {
    delay(500); // Give the packet time to leave the hardware
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    setCpuFrequencyMhz(10);
    while(true) { delay(1000); }
  }
}

// --- Standard HTTP Handlers ---
void handle_root() {
  push_stream_frame("INBOUND [HTTP] -> Root dashboard interface loaded.");
  server.send_P(200, "text/html", DASHBOARD_HTML);
}

void handle_stream_init() {


  // Capture the client request context to keep the socket connection open
  streamClient = server.client();
  
  // Send the specific HTTP headers required to set up an endless text stream
  streamClient.println("HTTP/1.1 200 OK");
  streamClient.println("Content-Type: text/event-stream");
  streamClient.println("Cache-Control: no-cache");
  streamClient.println("Connection: keep-alive");
  streamClient.println();
  streamClient.flush();
  
  isStreamActive = true;
  push_stream_frame("System live network stream channel opened successfully.");
}

void handle_hardware_rollback() {
  if (!server.hasArg("passphrase")) {
    server.send(400, "text/plain", "Bad Request");
    return;
  }

  String input_pass = server.arg("passphrase");
  if (input_pass == ROLLBACK_PASSWORD) {
    push_stream_frame("Valid signature authorization verified. Swapping boot slots...");
    const esp_partition_t* rollback_part = esp_ota_get_next_update_partition(NULL);
    if (rollback_part != NULL) {
      esp_ota_set_boot_partition(rollback_part);
      server.send(200, "text/plain", "Success");
      delay(1000);
      esp_restart();
    }
  } else {
    baseline.is_compromised = true;
    baseline.attack_reason = "BRUTE_FORCE_ATTEMPT";
    push_stream_frame("SECURITY ALERT: Invalid recovery passphrase input attempt detected!");
    server.send(401, "text/plain", "Denied");
  }
}

void handle_captive_redirect() {
  push_stream_frame("CAPTIVE PROBE -> Blocked probe route, redirecting client to security portal.");
  server.sendHeader("Location", "http://192.168.4", true);
  server.send(302, "text/plain", ""); 
}

void setup() {
  Serial.begin(115200);

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  dnsServer.start(DNS_PORT, "*", IPAddress(192, 168, 4, 1));

  // Take initial clean system snapshot ("Screenshot")
  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_partition_get_sha256(running, baseline.partition_hash);
  baseline.magnetic_tamper_count = 0;
  baseline.is_compromised = false;
  baseline.attack_reason = "";

  // Map Server Paths
  server.on("/", HTTP_GET, handle_root);
  server.on("/stream", HTTP_GET, handle_stream_init); // Stream source endpoint
  server.on("/api/rollback", HTTP_POST, handle_hardware_rollback);
  server.onNotFound(handle_captive_redirect);

  server.begin();
  Serial.println("Zero-Library Secure Matrix Operating.");
}

void loop() {
  dnsServer.processNextRequest();
  server.handleClient();

  // Stream data frame telemetry updates every 1 second smoothly
  static unsigned long last_telemetry_time = 0;
  unsigned long now = millis();
  if (now - last_telemetry_time >= 1000) {
    last_telemetry_time = now;
    if (isStreamActive) {
      push_stream_frame(""); // Updates data parameters on-screen
    }
  }
}
