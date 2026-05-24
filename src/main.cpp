#include <Arduino.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <time.h>
#include <freertos/semphr.h>

// Bluetooth Classic (ESP-IDF GAP API)
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"

// NAT/IP forwarding between STA and AP interfaces
extern "C" {
#include "lwip/lwip_napt.h"
}

// ─── Configuration ────────────────────────────────────────────────────────────
#define AP_SSID         "Bumptima"
#define AP_CHANNEL      6
#define AP_MAX_CONN     10
#define DNS_PORT        53
#define WEB_PORT        80
#define MAX_WIFI_DEV    30
#define MAX_BT_DEV      50
#define MAX_LOG         500
#define LOG_FILE        "/connections.csv"
#define WIFI_SCAN_MS    15000UL
#define BT_INQ_DURATION 10

static const char STA_SSID[] = "Dis1Mom";
static const char STA_PASS[] = "wasgoodlol";

// ─── Types ────────────────────────────────────────────────────────────────────
struct WiFiDev {
    char    ssid[33];
    char    bssid[18];
    int32_t rssi;
    uint8_t channel;
    uint8_t auth;
};

struct BtDev {
    char     name[64];
    char     addr[18];
    int8_t   rssi;
    uint32_t cod;
};

struct ConnEntry {
    char   mac[18];
    time_t connAt;
    time_t discAt;   // 0 = still connected
};

// ─── Globals ──────────────────────────────────────────────────────────────────
static WiFiDev           wifiList[MAX_WIFI_DEV];
static int               wifiCount = 0;
static BtDev             btList[MAX_BT_DEV];
static int               btCount   = 0;
static SemaphoreHandle_t scanMtx;

static ConnEntry         connLog[MAX_LOG];
static int               connLogCount = 0;
static SemaphoreHandle_t logMtx;
static bool              ntpReady = false;

static DNSServer         dns;
static AsyncWebServer    srv(WEB_PORT);

// ─── Time helpers ─────────────────────────────────────────────────────────────
static void fmtTime(time_t t, char *buf, size_t len) {
    if (t < 1000000000UL) {   // NTP not synced yet
        snprintf(buf, len, "T+%lus", (unsigned long)t);
        return;
    }
    struct tm *tm = localtime(&t);
    strftime(buf, len, "%Y-%m-%d %H:%M:%S", tm);
}

static time_t nowTime() {
    time_t t; time(&t);
    // Before NTP sync, use millis-based offset so relative order is preserved
    if (t < 1000000000UL) t = (time_t)(millis() / 1000);
    return t;
}

// ─── Connection log ───────────────────────────────────────────────────────────
// Ensure CSV header exists on first boot
static void initLogFile() {
    if (!LittleFS.exists(LOG_FILE)) {
        File f = LittleFS.open(LOG_FILE, "w");
        if (f) { f.println("MAC,Connected,Disconnected,Duration_s"); f.close(); }
    }
}

// Append a completed entry to the persistent CSV
static void appendLogFile(const ConnEntry &e) {
    File f = LittleFS.open(LOG_FILE, "a");
    if (!f) return;
    char tc[24], td[24];
    fmtTime(e.connAt, tc, sizeof(tc));
    fmtTime(e.discAt, td, sizeof(td));
    long dur = (e.discAt > 0 && e.connAt < e.discAt) ? (long)(e.discAt - e.connAt) : 0;
    f.printf("%s,%s,%s,%ld\n", e.mac, tc, td, dur);
    f.close();
}

// ─── WiFi AP client events ────────────────────────────────────────────────────
static void onAPConnect(WiFiEvent_t, WiFiEventInfo_t info) {
    char mac[18];
    auto *m = info.wifi_ap_staconnected.mac;
    snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
             m[0], m[1], m[2], m[3], m[4], m[5]);

    Serial.printf("[LOG] Client connected: %s\n", mac);

    if (xSemaphoreTake(logMtx, pdMS_TO_TICKS(200)) != pdTRUE) return;
    if (connLogCount < MAX_LOG) {
        strncpy(connLog[connLogCount].mac, mac, sizeof(connLog[0].mac));
        connLog[connLogCount].connAt = nowTime();
        connLog[connLogCount].discAt = 0;
        connLogCount++;
    }
    xSemaphoreGive(logMtx);
}

static void onAPDisconnect(WiFiEvent_t, WiFiEventInfo_t info) {
    char mac[18];
    auto *m = info.wifi_ap_stadisconnected.mac;
    snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
             m[0], m[1], m[2], m[3], m[4], m[5]);

    Serial.printf("[LOG] Client disconnected: %s\n", mac);

    if (xSemaphoreTake(logMtx, pdMS_TO_TICKS(200)) != pdTRUE) return;
    // Find the most recent open entry for this MAC and close it
    for (int i = connLogCount - 1; i >= 0; i--) {
        if (strcmp(connLog[i].mac, mac) == 0 && connLog[i].discAt == 0) {
            connLog[i].discAt = nowTime();
            appendLogFile(connLog[i]);   // persist to CSV
            break;
        }
    }
    xSemaphoreGive(logMtx);
}

// ─── Bluetooth GAP callback ───────────────────────────────────────────────────
static void btGapCb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *p) {
    if (event == ESP_BT_GAP_DISC_RES_EVT) {
        char addr[18];
        auto *b = p->disc_res.bda;
        snprintf(addr, sizeof(addr), "%02X:%02X:%02X:%02X:%02X:%02X",
                 b[0], b[1], b[2], b[3], b[4], b[5]);

        if (xSemaphoreTake(scanMtx, pdMS_TO_TICKS(50)) != pdTRUE) return;

        BtDev *slot = nullptr;
        for (int i = 0; i < btCount; i++) {
            if (strcmp(btList[i].addr, addr) == 0) { slot = &btList[i]; break; }
        }
        if (!slot && btCount < MAX_BT_DEV) slot = &btList[btCount++];

        if (slot) {
            strncpy(slot->addr, addr, sizeof(slot->addr));
            slot->name[0] = '\0';
            slot->rssi    = -100;
            slot->cod     = 0;

            for (int i = 0; i < p->disc_res.num_prop; i++) {
                auto &prop = p->disc_res.prop[i];
                switch (prop.type) {
                case ESP_BT_GAP_DEV_PROP_BDNAME:
                    if (prop.len > 0) {
                        int len = min((int)prop.len, (int)sizeof(slot->name) - 1);
                        memcpy(slot->name, prop.val, len);
                        slot->name[len] = '\0';
                    }
                    break;
                case ESP_BT_GAP_DEV_PROP_RSSI:
                    if (prop.len >= 1) slot->rssi = *(int8_t *)prop.val;
                    break;
                case ESP_BT_GAP_DEV_PROP_COD:
                    if (prop.len >= 3) memcpy(&slot->cod, prop.val, min((int)prop.len, 4));
                    break;
                default: break;
                }
            }
        }
        xSemaphoreGive(scanMtx);

    } else if (event == ESP_BT_GAP_DISC_STATE_CHANGED_EVT) {
        if (p->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED) {
            esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY,
                                       BT_INQ_DURATION, 0);
        }
    }
}

static void initBT() {
    esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
    esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    if (esp_bt_controller_init(&cfg) != ESP_OK) { Serial.println("[BT] init failed"); return; }
    if (esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT) != ESP_OK) { Serial.println("[BT] enable failed"); return; }
    if (esp_bluedroid_init() != ESP_OK || esp_bluedroid_enable() != ESP_OK) { Serial.println("[BT] bluedroid failed"); return; }
    esp_bt_gap_register_callback(btGapCb);
    esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, BT_INQ_DURATION, 0);
    Serial.println("[BT] Classic inquiry started");
}

// ─── WiFi scan ────────────────────────────────────────────────────────────────
static void doWifiScan() {
    int n = WiFi.scanNetworks(false, true);
    if (n < 0) { Serial.println("[WiFi] scan error"); return; }
    Serial.printf("[WiFi] found %d networks\n", n);

    if (xSemaphoreTake(scanMtx, pdMS_TO_TICKS(1000)) != pdTRUE) {
        WiFi.scanDelete(); return;
    }
    wifiCount = min(n, MAX_WIFI_DEV);
    for (int i = 0; i < wifiCount; i++) {
        strncpy(wifiList[i].ssid,  WiFi.SSID(i).c_str(), 32);
        wifiList[i].ssid[32] = '\0';
        strncpy(wifiList[i].bssid, WiFi.BSSIDstr(i).c_str(), 17);
        wifiList[i].bssid[17] = '\0';
        wifiList[i].rssi    = WiFi.RSSI(i);
        wifiList[i].channel = (uint8_t)WiFi.channel(i);
        wifiList[i].auth    = (uint8_t)WiFi.encryptionType(i);
    }
    xSemaphoreGive(scanMtx);
    WiFi.scanDelete();
}

// ─── Web server ───────────────────────────────────────────────────────────────
static void setupServer() {
    auto toPortal = [](AsyncWebServerRequest *r) {
        r->redirect("http://192.168.4.1/");
    };
    srv.on("/generate_204",              HTTP_GET, toPortal);
    srv.on("/hotspot-detect.html",       HTTP_GET, toPortal);
    srv.on("/connecttest.txt",           HTTP_GET, toPortal);
    srv.on("/ncsi.txt",                  HTTP_GET, toPortal);
    srv.on("/redirect",                  HTTP_GET, toPortal);
    srv.on("/canonical.html",            HTTP_GET, toPortal);
    srv.on("/success.txt",               HTTP_GET, toPortal);
    srv.on("/library/test/success.html", HTTP_GET, toPortal);

    // ── Scan API ──
    srv.on("/api/scan", HTTP_GET, [](AsyncWebServerRequest *r) {
        JsonDocument doc;
        JsonArray wifi = doc["wifi"].to<JsonArray>();
        JsonArray bt   = doc["bt"].to<JsonArray>();

        if (xSemaphoreTake(scanMtx, pdMS_TO_TICKS(500)) == pdTRUE) {
            for (int i = 0; i < wifiCount; i++) {
                auto w = wifi.add<JsonObject>();
                w["ssid"]    = wifiList[i].ssid;
                w["bssid"]   = wifiList[i].bssid;
                w["rssi"]    = wifiList[i].rssi;
                w["channel"] = wifiList[i].channel;
                w["auth"]    = wifiList[i].auth;
            }
            for (int i = 0; i < btCount; i++) {
                auto b = bt.add<JsonObject>();
                b["name"] = btList[i].name[0] ? btList[i].name : btList[i].addr;
                b["addr"] = btList[i].addr;
                b["rssi"] = btList[i].rssi;
                b["cod"]  = btList[i].cod;
            }
            xSemaphoreGive(scanMtx);
        }
        String json; serializeJson(doc, json);
        r->send(200, "application/json", json);
    });

    // ── Connection log API (JSON) ──
    srv.on("/api/log", HTTP_GET, [](AsyncWebServerRequest *r) {
        JsonDocument doc;
        JsonArray arr = doc.to<JsonArray>();

        if (xSemaphoreTake(logMtx, pdMS_TO_TICKS(500)) == pdTRUE) {
            for (int i = connLogCount - 1; i >= 0; i--) {   // newest first
                auto e = arr.add<JsonObject>();
                char tc[24], td[24];
                fmtTime(connLog[i].connAt, tc, sizeof(tc));
                e["mac"]  = connLog[i].mac;
                e["conn"] = tc;
                if (connLog[i].discAt > 0) {
                    fmtTime(connLog[i].discAt, td, sizeof(td));
                    e["disc"] = td;
                    e["dur"]  = (long)(connLog[i].discAt - connLog[i].connAt);
                } else {
                    e["disc"] = "";
                    e["dur"]  = -1;   // still connected
                }
            }
            xSemaphoreGive(logMtx);
        }
        String json; serializeJson(doc, json);
        r->send(200, "application/json", json);
    });

    // ── CSV download — combines in-memory active sessions + persisted file ──
    srv.on("/log/download", HTTP_GET, [](AsyncWebServerRequest *r) {
        // Build a fresh CSV from both sources
        String csv = "MAC,Connected,Disconnected,Duration_s,Status\n";

        // Active (not yet disconnected) entries from memory
        if (xSemaphoreTake(logMtx, pdMS_TO_TICKS(500)) == pdTRUE) {
            for (int i = 0; i < connLogCount; i++) {
                if (connLog[i].discAt == 0) {
                    char tc[24];
                    fmtTime(connLog[i].connAt, tc, sizeof(tc));
                    csv += String(connLog[i].mac) + "," + tc + ",,0,ACTIVE\n";
                }
            }
            xSemaphoreGive(logMtx);
        }

        // Completed entries from LittleFS
        File f = LittleFS.open(LOG_FILE, "r");
        if (f) {
            f.readStringUntil('\n');   // skip header
            while (f.available()) {
                String line = f.readStringUntil('\n');
                line.trim();
                if (line.length() > 0) csv += line + ",COMPLETED\n";
            }
            f.close();
        }

        AsyncWebServerResponse *resp =
            r->beginResponse(200, "text/csv", csv);
        resp->addHeader("Content-Disposition",
                        "attachment; filename=\"bumptima_log.csv\"");
        r->send(resp);
    });

    // ── Clear log ──
    srv.on("/log/clear", HTTP_POST, [](AsyncWebServerRequest *r) {
        if (xSemaphoreTake(logMtx, pdMS_TO_TICKS(500)) == pdTRUE) {
            connLogCount = 0;
            xSemaphoreGive(logMtx);
        }
        LittleFS.remove(LOG_FILE);
        initLogFile();
        r->send(200, "text/plain", "ok");
    });

    srv.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");
    srv.onNotFound(toPortal);
    srv.begin();
    Serial.println("[Web] server started on port 80");
}

// ─── Setup ────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(100);
    Serial.println("\n\n======== BUMPTIMA ========");

    scanMtx = xSemaphoreCreateMutex();
    logMtx  = xSemaphoreCreateMutex();

    if (!LittleFS.begin(true)) {
        Serial.println("[FS] LittleFS mount failed");
    }
    initLogFile();

    // Register AP client events
    WiFi.onEvent(onAPConnect,    ARDUINO_EVENT_WIFI_AP_STACONNECTED);
    WiFi.onEvent(onAPDisconnect, ARDUINO_EVENT_WIFI_AP_STADISCONNECTED);

    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(AP_SSID, nullptr, AP_CHANNEL, 0, AP_MAX_CONN);
    delay(500);
    Serial.printf("[WiFi] AP '%s' up — IP: %s\n",
                  AP_SSID, WiFi.softAPIP().toString().c_str());

    if (strlen(STA_SSID) > 0) {
        WiFi.begin(STA_SSID, STA_PASS);
        Serial.print("[WiFi] Connecting to upstream");
        for (int i = 0; i < 20 && WiFi.status() != WL_CONNECTED; i++) {
            delay(500); Serial.print('.');
        }
        if (WiFi.status() == WL_CONNECTED) {
            Serial.printf("\n[WiFi] STA connected — IP: %s\n",
                          WiFi.localIP().toString().c_str());
            // Sync time via NTP now that we have internet
            configTime(0, 0, "pool.ntp.org", "time.google.com");
            Serial.println("[NTP] time sync requested");
#ifdef CONFIG_LWIP_IPV4_NAPT
            ip_napt_enable((uint32_t)WiFi.softAPIP(), 1);
            Serial.println("[NAT] enabled");
#endif
        } else {
            Serial.println("\n[WiFi] upstream connection failed");
        }
    }

    dns.start(DNS_PORT, "*", WiFi.softAPIP());
    setupServer();
    initBT();
    doWifiScan();

    Serial.println("[Bumptima] ready");
}

// ─── Loop ─────────────────────────────────────────────────────────────────────
static unsigned long lastScan = 0;

void loop() {
    dns.processNextRequest();

    if (millis() - lastScan > WIFI_SCAN_MS) {
        lastScan = millis();
        doWifiScan();
    }

    delay(5);
}
