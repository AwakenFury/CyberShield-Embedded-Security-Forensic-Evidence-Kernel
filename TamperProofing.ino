#include <WiFi.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <esp_partition.h>
#include <esp_ota_ops.h>
#include <esp_heap_caps.h>

// --- Configuration ---
const char* AP_SSID = "ESP32_Secure_Node";
const char* AP_PASS = "FactorySecure123!"; 
const byte DNS_PORT = 53;
const int MAX_ALLOWED_DEVICES = 2;

// --- Security Passphrase for Rollback ---
const char* ROLLBACK_PASSWORD = "AdminSuperSecretKey2026"; 

// --- Security Baseline Blueprint ---
struct SystemSnapshot {
  uint8_t partition_hash[32];
  size_t stable_free_heap;
  volatile uint32_t magnetic_tamper_count;
  bool is_compromised;
  String attack_reason;
};

SystemSnapshot baseline;
DNSServer dnsServer;
WebServer server(80);

// --- Hardware Interrupt for Hall Effect / External Switch ---
const int TAMPER_PIN = 4; 
void IRAM_ATTR handle_magnetic_tamper() {
  baseline.magnetic_tamper_count++;
}

// --- HTML Dashboard UI Layout ---
const char DASHBOARD_HTML[] PROGMEM = R"=====(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>ESP32 Tamper Console</title>
    <style>
        body { font-family: -apple-system, sans-serif; background: #0f172a; color: #f8fafc; padding: 20px; margin: 0; }
        .container { max-width: 600px; margin: 0 auto; }
        h1 { color: #f1f5f9; border-bottom: 2px solid #334155; padding-bottom: 10px; font-size: 24px; }
        .card { background: #1e293b; padding: 20px; border-radius: 8px; margin-bottom: 15px; box-shadow: 0 4px 6px -1px rgba(0,0,0,0.1); }
        .status-box { padding: 12px; border-radius: 6px; font-weight: bold; text-align: center; font-size: 18px; }
        .status-good { background: #065f46; color: #34d399; }
        .status-bad { background: #991b1b; color: #f87171; animation: pulse 1.5s infinite; }
        .metric-row { display: flex; justify-content: space-between; padding: 10px 0; border-bottom: 1px solid #334155; }
        .metric-label { color: #94a3b8; }
        .metric-value { font-family: monospace; color: #e2e8f0; font-weight: bold; }
        .btn { display: block; width: 100%; background: #2563eb; color: white; border: none; padding: 12px; border-radius: 6px; font-weight: bold; cursor: pointer; text-align: center; text-decoration: none; margin-top: 15px; box-sizing: border-box; }
        .btn:hover { background: #1d4ed8; }
        .btn-warn { background: #dc2626; margin-top: 10px; }
        .btn-warn:hover { background: #b91c1c; }
        .input-field { width: 100%; padding: 10px; border-radius: 6px; border: 1px solid #334155; background: #0f172a; color: white; margin-top: 5px; margin-bottom: 10px; box-sizing: border-box; }
        @keyframes pulse { 0% { opacity: 1; } 50% { opacity: 0.5; } 100% { opacity: 1; } }
    </style>
</head>
<body>
    <div class="container">
        <h1>🛡️ Hardware Integrity Node</h1>
        
        <div class="card">
            <div id="statusAlert" class="status-box status-good">SYSTEM SECURE</div>
        </div>

        <div class="card">
            <h3>System Telemetry Baseline</h3>
            <div class="metric-row"><span class="metric-label">Firmware Hash (SHA-256)</span><span id="hashVal" class="metric-value">Loading...</span></div>
            <div class="metric-row"><span class="metric-label">Active Connections</span><span id="connVal" class="metric-value">0</span></div>
            <div class="metric-row"><span class="metric-label">Free Dynamic Memory</span><span id="ramVal" class="metric-value">0 bytes</span></div>
            <div class="metric-row"><span class="metric-label">Magnetic/Side-Channel Flags</span><span id="magVal" class="metric-value">0</span></div>
        </div>
        
        <div class="card">
            <h3>Emergency Recovery</h3>
            <label class="metric-label" for="passKey">Validation Passphrase</label>
            <input type="password" id="passKey" class="input-field" placeholder="Enter secure key...">
            <button class="btn btn-warn" onclick="triggerRollback()">Rollback to Stock Firmware</button>
        </div>
        
        <button class="btn" onclick="refreshData()">Query Live Baseline</button>
    </div>

    <script>
        function refreshData() {
            fetch('/api/status')
                .then(res => res.json())
                .then(data => {
                    document.getElementById('hashVal').innerText = data.hash.substring(0, 16) + "...";
                    document.getElementById('connVal').innerText = data.connections + " / " + data.max_connections;
                    document.getElementById('ramVal').innerText = data.free_ram.toLocaleString() + " bytes";
                    document.getElementById('magVal').innerText = data.magnetic_events;
                    
                    const alertBox = document.getElementById('statusAlert');
                    if (data.compromised) {
                        alertBox.innerText = "CRITICAL ALERT: " + data.reason;
                        alertBox.className = "status-box status-bad";
                    } else {
                        alertBox.innerText = "SYSTEM SECURE";
                        alertBox.className = "status-box status-good";
                    }
                });
        }

        function triggerRollback() {
            const passValue = document.getElementById('passKey').value;
            if (!passValue) {
                alert("Passphrase field cannot be blank.");
                return;
            }
            if (confirm("Are you sure you want to discard the active sketch and revert the hardware?")) {
                let formData = new FormData();
                formData.append("passphrase", passValue);

                fetch('/api/rollback', { 
                    method: 'POST',
                    body: formData
                })
                .then(res => {
                    if (res.status === 200) {
                        alert("Rollback initiated. System rebooting away from attack vector...");
                        window.location.reload();
                    } else if (res.status === 401) {
                        alert("ERROR: Invalid Passphrase. Tamper attempt logged.");
                    } else {
                        alert("ERROR: No recovery partition found on flash.");
                    }
                })
                .catch(err => alert("Network communication lost during processing. Check node connection."));
            }
        }
        
        setInterval(refreshData, 2000);
        window.onload = refreshData;
    </script>
</body>
</html>
)=====";

// --- Web Server Request Handlers ---
void handle_root() {
  server.send_P(200, "text/html", DASHBOARD_HTML);
}

void handle_status_api() {
  int current_stations = WiFi.softAPgetStationNum();
  size_t current_free_ram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  
  uint8_t current_hash[32];
  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_partition_get_sha256(running, current_hash);

  if (memcmp(current_hash, baseline.partition_hash, 32) != 0) {
    baseline.is_compromised = true;
    baseline.attack_reason = "SKETCH_MODIFICATION_DETECTED";
  }
  if (!heap_caps_check_integrity_all(true)) {
    baseline.is_compromised = true;
    baseline.attack_reason = "RAM_INJECTION_OVERFLOW";
  }
  if (current_stations > MAX_ALLOWED_DEVICES) {
    baseline.is_compromised = true;
    baseline.attack_reason = "UNAUTHORIZED_DEVICES_CONNECTED";
  }

  String hex_hash = "";
  for (int i = 0; i < 32; i++) {
    if (current_hash[i] < 0x10) hex_hash += "0";
    hex_hash += String(current_hash[i], HEX);
  }

  String json = "{";
  json += "\"hash\":\"" + hex_hash + "\",";
  json += "\"connections\":" + String(current_stations) + ",";
  json += "\"max_connections\":" + String(MAX_ALLOWED_DEVICES) + ",";
  json += "\"free_ram\":" + String(current_free_ram) + ",";
  json += "\"magnetic_events\":" + String(baseline.magnetic_tamper_count) + ",";
  json += "\"compromised\":" + String(baseline.is_compromised ? "true" : "false") + ",";
  json += "\"reason\":\"" + baseline.attack_reason + "\"";
  json += "}";
  
  server.send(200, "application/json", json);
}

void handle_hardware_rollback() {
  // 1. Enforce strict passphrase checking
  if (!server.hasArg("passphrase")) {
    server.send(400, "text/plain", "Missing payload context.");
    return;
  }
  
  String input_pass = server.arg("passphrase");
  
  // Protect against buffer overflows on the parameter input
  if (input_pass.length() > 64 || input_pass != ROLLBACK_PASSWORD) {
    baseline.is_compromised = true;
    baseline.attack_reason = "BRUTE_FORCE_ROLLBACK_ATTEMPT";
    server.send(401, "text/plain", "Unauthorized.");
    return;
  }

  // 2. Locate the alternate OTA flash partition space
  const esp_partition_t* rollback_partition = esp_ota_get_next_update_partition(NULL);
  if (rollback_partition == NULL) {
    server.send(500, "text/plain", "No secondary OTA partition slot configured.");
    return;
  }

  // 3. Command the bootloader state to pivot away from active sketch
  esp_err_t err = esp_ota_set_boot_partition(rollback_partition);
  if (err == ESP_OK) {
    server.send(200, "text/plain", "Reverting. Rebooting...");
    delay(1000);
    esp_restart(); // System Hard Reset
  } else {
    server.send(500, "text/plain", "Bootloader register update failed.");
  }
}

void handle_captive_redirect() {
  server.sendHeader("Location", "http://192.168.4", true);
  server.send(302, "text/plain", ""); 
}

// --- Setup Operations ---
void setup() {
  Serial.begin(115200);

  pinMode(TAMPER_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(TAMPER_PIN), handle_magnetic_tamper, FALLING);

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  delay(100); 

IPAddress apIP(192, 168, 4, 1);
dnsServer.start(DNS_PORT, "*", apIP);

  // Capture clean image footprint





  // Capture clean image footprint
  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_partition_get_sha256(running, baseline.partition_hash);
  baseline.stable_free_heap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  baseline.magnetic_tamper_count = 0;
  baseline.is_compromised = false;
  baseline.attack_reason = "";

  // Mount API Endpoints
  server.on("/", HTTP_GET, handle_root);
  server.on("/api/status", HTTP_GET, handle_status_api);
  server.on("/api/rollback", HTTP_POST, handle_hardware_rollback);
  
  // Captive Redirect Catchers
  server.on("/generate_204", handle_captive_redirect);  
  server.on("/fwlink", handle_captive_redirect);        
  server.onNotFound(handle_captive_redirect);           

  server.begin();
  Serial.println("Secure Core Online.");
}

void loop() {
  dnsServer.processNextRequest();
  server.handleClient();
  
  if (baseline.is_compromised) {
    // Drop execution speeds down during alert states to choke off dynamic fuzzing tools
    delay(500); 
  }
}
