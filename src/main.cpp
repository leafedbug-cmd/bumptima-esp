#include <Arduino.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
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
#define WIFI_SCAN_MS    15000UL
#define BT_INQ_DURATION 10      // × 1.28 s ≈ 12.8 s inquiry window

// ── Upstream WiFi for internet passthrough.
// Set STA_SSID to your phone's hotspot SSID, STA_PASS to its password.
// Leave STA_SSID empty to run AP-only (no internet sharing).
static const char STA_SSID[] = "Dis1Mom";
static const char STA_PASS[] = "wasgoodlol";

// ─── Device types ─────────────────────────────────────────────────────────────
struct WiFiDev {
    char    ssid[33];
    char    bssid[18];
    int32_t rssi;
    uint8_t channel;
    uint8_t auth;   // wifi_auth_mode_t cast to uint8
};

struct BtDev {
    char     name[64];
    char     addr[18];
    int8_t   rssi;
    uint32_t cod;   // Class of Device
};

// ─── Globals ──────────────────────────────────────────────────────────────────
static WiFiDev          wifiList[MAX_WIFI_DEV];
static int              wifiCount = 0;
static BtDev            btList[MAX_BT_DEV];
static int              btCount   = 0;
static SemaphoreHandle_t scanMtx;

static DNSServer        dns;
static AsyncWebServer   srv(WEB_PORT);

// ─── Bluetooth GAP callback ───────────────────────────────────────────────────
static void btGapCb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *p) {
    if (event == ESP_BT_GAP_DISC_RES_EVT) {
        char addr[18];
        auto *b = p->disc_res.bda;
        snprintf(addr, sizeof(addr), "%02X:%02X:%02X:%02X:%02X:%02X",
                 b[0], b[1], b[2], b[3], b[4], b[5]);

        if (xSemaphoreTake(scanMtx, pdMS_TO_TICKS(50)) != pdTRUE) return;

        // Find existing slot (de-duplicate by address) or claim a new one
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
                default:
                    break;
                }
            }
        }
        xSemaphoreGive(scanMtx);

    } else if (event == ESP_BT_GAP_DISC_STATE_CHANGED_EVT) {
        if (p->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED) {
            // Auto-restart inquiry
            esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY,
                                       BT_INQ_DURATION, 0);
        }
    }
}

static void initBT() {
    // Release BLE memory — we only need Classic BT
    esp_bt_controller_mem_release(ESP_BT_MODE_BLE);

    esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    if (esp_bt_controller_init(&cfg) != ESP_OK) {
        Serial.println("[BT] controller init failed"); return;
    }
    if (esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT) != ESP_OK) {
        Serial.println("[BT] enable failed"); return;
    }
    if (esp_bluedroid_init() != ESP_OK || esp_bluedroid_enable() != ESP_OK) {
        Serial.println("[BT] bluedroid failed"); return;
    }
    esp_bt_gap_register_callback(btGapCb);
    esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY,
                               BT_INQ_DURATION, 0);
    Serial.println("[BT] Classic inquiry started");
}

// ─── WiFi scan ────────────────────────────────────────────────────────────────
static void doWifiScan() {
    int n = WiFi.scanNetworks(false, true);   // blocking, show hidden
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
    // Captive portal detection — all OS flavors
    auto toPortal = [](AsyncWebServerRequest *r) {
        r->redirect("http://192.168.4.1/");
    };
    srv.on("/generate_204",         HTTP_GET, toPortal);  // Android
    srv.on("/hotspot-detect.html",  HTTP_GET, toPortal);  // iOS / macOS
    srv.on("/connecttest.txt",      HTTP_GET, toPortal);  // Windows
    srv.on("/ncsi.txt",             HTTP_GET, toPortal);  // Windows
    srv.on("/redirect",             HTTP_GET, toPortal);
    srv.on("/canonical.html",       HTTP_GET, toPortal);
    srv.on("/success.txt",          HTTP_GET, toPortal);
    srv.on("/library/test/success.html", HTTP_GET, toPortal);

    // JSON scan API
    srv.on("/api/scan", HTTP_GET, [](AsyncWebServerRequest *r) {
        JsonDocument doc;
        JsonArray    wifi = doc["wifi"].to<JsonArray>();
        JsonArray    bt   = doc["bt"].to<JsonArray>();

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

        String json;
        serializeJson(doc, json);
        r->send(200, "application/json", json);
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

    if (!LittleFS.begin(true)) {
        Serial.println("[FS] LittleFS mount failed — run 'pio run -t uploadfs'");
    }

    // AP + STA mode
    WiFi.mode(WIFI_AP_STA);
    // Open network: pass nullptr for password
    WiFi.softAP(AP_SSID, nullptr, AP_CHANNEL, 0, AP_MAX_CONN);
    delay(500);
    Serial.printf("[WiFi] AP '%s' up — IP: %s\n",
                  AP_SSID, WiFi.softAPIP().toString().c_str());

    // Optional upstream connection for internet passthrough
    if (strlen(STA_SSID) > 0) {
        WiFi.begin(STA_SSID, STA_PASS);
        Serial.print("[WiFi] Connecting to upstream");
        for (int i = 0; i < 20 && WiFi.status() != WL_CONNECTED; i++) {
            delay(500); Serial.print('.');
        }
        if (WiFi.status() == WL_CONNECTED) {
            Serial.printf("\n[WiFi] STA connected — IP: %s\n",
                          WiFi.localIP().toString().c_str());
#ifdef CONFIG_LWIP_IPV4_NAPT
            // Enable NAT so AP clients route through the STA internet connection
            ip_napt_enable((uint32_t)WiFi.softAPIP(), 1);
            Serial.println("[NAT] enabled — AP clients have internet");
#else
            Serial.println("[NAT] not compiled in (needs CONFIG_LWIP_IPV4_NAPT)");
#endif
        } else {
            Serial.println("\n[WiFi] upstream connection failed");
        }
    }

    // DNS — respond with our IP for every hostname (captive portal trigger)
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
