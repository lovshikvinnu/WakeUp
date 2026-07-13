/*
 * PROJECT: WakeLight v1.0
 * HARDWARE: ESP32 DevKit V1, Active LOW Relay (GPIO 5)
 * BLYNK: Template "Wake Up"
 */

#define BLYNK_TEMPLATE_ID "YOUR_TEMPLATE"
#define BLYNK_TEMPLATE_NAME "YOUR_TEMPLATE_NAME"
#define BLYNK_AUTH_TOKEN "YOUR_TOKEN"
#define BLYNK_PRINT Serial

#include <WiFi.h>
#include <WiFiClient.h>
#include <BlynkSimpleEsp32.h>
#include <time.h>
#include <esp_task_wdt.h>
#include <Preferences.h>

// --- Constants ---
const int RELAY_PIN = 5;
const int ARM_CUTOFF_HOUR = 15;
const int ALARM_WINDOW_MINUTES = 60;
const int WDT_TIMEOUT_SEC = 10;
const long TIMEZONE_OFFSET_SEC = 19800; // GMT +5:30
const uint32_t NTP_RETRY_INTERVAL_MS = 60000;
const uint32_t WIFI_RETRY_INTERVAL_MS = 10000;
const uint32_t BLYNK_RETRY_INTERVAL_MS = 10000;
const uint32_t LOOP_DELAY_MS = 1000;
const char* NTP_SERVER = "time.google.com";

// --- WiFi Credentials ---
char ssid[] = "YOUR_SSID";
char pass[] = "PASSWORD";

// --- Global Variables ---
int wakeHour = 6;
int wakeMinute = 0;
bool relayState = false; // Note: false means RELAY_PIN is HIGH (Active LOW relay)
int lastTriggerDay = -1;
bool ntpSynced = false;

unsigned long lastNtpRetry = 0;
unsigned long lastWifiRetry = 0;
unsigned long lastBlynkRetry = 0;
int wifiRetryCount = 0;

WidgetTerminal terminal(V4);
Preferences preferences;

// --- Function Prototypes ---
void printLog(String msg);
void updateBlynkState();
void relayOn();
void relayOff();
void connectWiFi(bool blocking);
void connectBlynk(bool blocking);
void syncNTP();
void rearmAlarm(int newHour, int newMinute);
bool shouldTriggerScheduledAlarm(int currentHour, int currentMinute, int currentDay);
void checkBootRecovery();
void processScheduler();
void printStatus();

// --- Core Helper Functions ---

void printLog(String msg) {
    Serial.println(msg);
    if (Blynk.connected()) {
        terminal.println(msg);
        terminal.flush();
    }
}

void updateBlynkState() {
    if (!Blynk.connected()) return;
    Blynk.virtualWrite(V0, relayState ? 1 : 0);
    Blynk.virtualWrite(V3, relayState ? 1 : 0);
}

void relayOn() {
    if (relayState) return;
    
    digitalWrite(RELAY_PIN, LOW); // Active LOW
    relayState = true;
    
    updateBlynkState();
    printLog("[ACTION] Relay ON");
}

void relayOff() {
    if (!relayState) return;
    
    digitalWrite(RELAY_PIN, HIGH); // Active LOW
    relayState = false;
    
    updateBlynkState();
    printLog("[ACTION] Relay OFF");
}

// --- Alarm Re-arming Logic ---

void rearmAlarm(int newHour, int newMinute) {
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo, 10)) return;

    int currentMinutes = timeinfo.tm_hour * 60 + timeinfo.tm_min;
    int newAlarmMinutes = newHour * 60 + newMinute;

    if (newAlarmMinutes > currentMinutes) {
        lastTriggerDay = -1;
        printLog("[INFO] Alarm Re-Armed");
    }
}

// --- Blynk Callbacks ---

BLYNK_CONNECTED() {
    printLog("[INFO] Blynk Connected");
    Blynk.syncVirtual(V1);
    Blynk.syncVirtual(V2);
    updateBlynkState();
}

BLYNK_WRITE(V0) {
    if (param.asInt() == 1) {
        relayOn();
    } else {
        relayOff();
    }
}

BLYNK_WRITE(V1) {
    int newHour = constrain(param.asInt(), 0, 23);
    rearmAlarm(newHour, wakeMinute);
    wakeHour = newHour;
    preferences.putInt("hour", wakeHour);
    printLog("[INFO] Wake Hour Updated: " + String(wakeHour));
}

BLYNK_WRITE(V2) {
    int newMinute = constrain(param.asInt(), 0, 59);
    rearmAlarm(wakeHour, newMinute);
    wakeMinute = newMinute;
    preferences.putInt("minute", wakeMinute);
    printLog("[INFO] Wake Minute Updated: " + String(wakeMinute));
}

// --- Network & Time Functions ---

void connectWiFi(bool blocking) {
    if (WiFi.status() == WL_CONNECTED) return;
    
    Serial.println("[INFO] Connecting to WiFi...");
    WiFi.begin(ssid, pass);
    
    if (blocking) {
        unsigned long startAttempt = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < WIFI_RETRY_INTERVAL_MS) {
            delay(500);
            Serial.print(".");
            esp_task_wdt_reset();
        }
        Serial.println();
        if (WiFi.status() == WL_CONNECTED) {
            Serial.println("[INFO] WiFi Connected");
        } else {
            Serial.println("[WARNING] WiFi Connection Timeout");
        }
    }
}

void connectBlynk(bool blocking) {
    if (Blynk.connected()) return;
    
    Serial.println("[INFO] Connecting to Blynk...");
    Blynk.config(BLYNK_AUTH_TOKEN);
    
    if (blocking) {
        Blynk.connect(BLYNK_RETRY_INTERVAL_MS);
    }
}

void syncNTP() {
    Serial.println("[INFO] Syncing NTP...");
    
    unsigned long start = millis();
    struct tm timeinfo;
    
    while (millis() - start < 10000) {
        if (getLocalTime(&timeinfo, 0)) {
            ntpSynced = true;
            printLog("[INFO] NTP Synced");
            return;
        }
        delay(500);
        esp_task_wdt_reset();
    }
    
    printLog("[WARNING] NTP Sync Failed");
    ntpSynced = false;
}

// --- Scheduler Logic ---

bool shouldTriggerScheduledAlarm(int currentHour, int currentMinute, int currentDay) {
    int currentMinutes = currentHour * 60 + currentMinute;
    int wakeMinutes = wakeHour * 60 + wakeMinute;
    int alarmWindowEnd = min(wakeMinutes + ALARM_WINDOW_MINUTES, 24 * 60);

    if (currentMinutes >= wakeMinutes && currentMinutes < alarmWindowEnd) {
        if (lastTriggerDay != currentDay) {
            return true;
        }
    }
    
    return false;
}

void checkBootRecovery() {
    if (!ntpSynced) return;
    
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo, 10)) return;
    
    int currentDay = timeinfo.tm_yday;
    int currentMinutes = timeinfo.tm_hour * 60 + timeinfo.tm_min;
    int wakeMinutes = wakeHour * 60 + wakeMinute;
    int cutoffMinutes = ARM_CUTOFF_HOUR * 60;
    
    if (currentMinutes >= wakeMinutes && currentMinutes < cutoffMinutes) {
        if (lastTriggerDay != currentDay) {
            relayOn();
            lastTriggerDay = currentDay;
            printLog("[EVENT] Boot Recovery Alarm");
        }
    } else if (currentMinutes >= cutoffMinutes && wakeMinutes < cutoffMinutes) {
        if (lastTriggerDay != currentDay) {
            lastTriggerDay = currentDay;
            printLog("[INFO] Booted after cutoff. Ignored today's morning alarm.");
        }
    }
}

void processScheduler() {
    if (!ntpSynced) return;
    
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo, 10)) return; 

    if (shouldTriggerScheduledAlarm(timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_yday)) {
        printLog("[EVENT] Alarm Triggered");
        relayOn();
        lastTriggerDay = timeinfo.tm_yday;
    }
}

// --- Output & Logging ---

void printStatus() {
    struct tm timeinfo;
    bool hasTime = getLocalTime(&timeinfo, 10);
    
    int rssi = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : 0;
    String wifiStat = (WiFi.status() == WL_CONNECTED) ? "Connected" : "Disconnected";
    String blynkStat = Blynk.connected() ? "Connected" : "Disconnected";
    
    char buf[256];
    if (hasTime) {
        snprintf(buf, sizeof(buf), "Time: %02d:%02d:%02d | Wake: %02d:%02d | Day: %d | LastDay: %d | Relay: %s | WiFi: %s (%d dBm) | Blynk: %s",
                 timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec,
                 wakeHour, wakeMinute,
                 timeinfo.tm_yday, lastTriggerDay,
                 relayState ? "ON" : "OFF",
                 wifiStat.c_str(), rssi,
                 blynkStat.c_str());
    } else {
        snprintf(buf, sizeof(buf), "Time: --:--:-- | Wake: %02d:%02d | Day: -- | LastDay: %d | Relay: %s | WiFi: %s (%d dBm) | Blynk: %s",
                 wakeHour, wakeMinute, lastTriggerDay,
                 relayState ? "ON" : "OFF",
                 wifiStat.c_str(), rssi,
                 blynkStat.c_str());
    }
    Serial.println(buf);
}

// --- Setup ---

void setup() {
    Serial.begin(115200);
    Serial.println();

    // Watchdog Init
    #if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
        esp_task_wdt_config_t wdt_config = {
            .timeout_ms = WDT_TIMEOUT_SEC * 1000,
            .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,
            .trigger_panic = true
        };
        esp_task_wdt_init(&wdt_config);
    #else
        esp_task_wdt_init(WDT_TIMEOUT_SEC, true);
    #endif
    esp_task_wdt_add(NULL);

    // Hardware Init
    pinMode(RELAY_PIN, OUTPUT);
    digitalWrite(RELAY_PIN, HIGH); // Force HIGH immediately
    relayState = false;            // Always initialize in predictable OFF state

    // Preferences Init & State Restore
    preferences.begin("WakeLight", false);
    wakeHour = constrain(preferences.getInt("hour", 6), 0, 23);
    wakeMinute = constrain(preferences.getInt("minute", 0), 0, 59);

    // Network & NTP Init
    connectWiFi(true);
    connectBlynk(true);
    
    printLog("--------------------------------------------------");
    printLog("WakeLight Firmware v1.0");
    printLog(String("Build Date: ") + __DATE__);
    printLog(String("Build Time: ") + __TIME__);
    printLog("--------------------------------------------------");

    configTime(TIMEZONE_OFFSET_SEC, 0, NTP_SERVER);
    syncNTP();
    
    // Process boot recovery once after time synchronization
    checkBootRecovery();
}

// --- Main Loop ---

void loop() {
    unsigned long currentMillis = millis();

    // 1. Reconnect WiFi (Non-blocking)
    if (WiFi.status() != WL_CONNECTED) {
        if (currentMillis - lastWifiRetry >= WIFI_RETRY_INTERVAL_MS) {
            Serial.println("[WARNING] WiFi Disconnected. Reconnecting...");
            if (wifiRetryCount < 5) {
                WiFi.reconnect();
                wifiRetryCount++;
            } else {
                WiFi.disconnect();
                WiFi.begin(ssid, pass);
                wifiRetryCount = 0;
            }
            lastWifiRetry = currentMillis;
        }
    } else {
        wifiRetryCount = 0;
        // 2. Reconnect Blynk (Non-blocking)
        if (!Blynk.connected()) {
            if (currentMillis - lastBlynkRetry >= BLYNK_RETRY_INTERVAL_MS) {
                Serial.println("[WARNING] Blynk Disconnected. Reconnecting...");
                Blynk.connect(2000); 
                lastBlynkRetry = currentMillis;
            }
        }
    }

    // Always run Blynk to process internal state and handle timeouts
    Blynk.run();

    // 3. Retry NTP if failed (Non-blocking)
    if (!ntpSynced && WiFi.status() == WL_CONNECTED) {
        if (currentMillis - lastNtpRetry >= NTP_RETRY_INTERVAL_MS) {
            syncNTP();
            lastNtpRetry = currentMillis;
        }
    }

    // 4. Reset Watchdog
    esp_task_wdt_reset();

    // 5. Scheduler & Status (Executed every second based on delay)
    printStatus();
    processScheduler();

    // 6. Scheduler Delay
    delay(LOOP_DELAY_MS);
}
