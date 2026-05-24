# bumptima-esp

ESP32-WROOM-32UE firmware — Lonely Binary Gold Edition board.

WiFi access point named **Bumptima** (open/no password) with a captive-portal web UI showing a live radar map of nearby WiFi networks and Bluetooth Classic devices.

See [SPECSHEET.md](SPECSHEET.md) for the full pinout reference.

---

## Features

| Feature | Details |
|---|---|
| WiFi AP | SSID: `Bumptima`, open (no password) |
| Captive portal | Auto-redirects to web UI on connect (Android, iOS, Windows) |
| Internet passthrough | Optional: connect ESP32 to upstream WiFi (e.g. phone hotspot) via STA+AP NAT |
| Web UI | Dark radar display with live Leaflet map + device list |
| WiFi scan | All nearby networks, refreshes every 15 s |
| BT Classic scan | Continuous BT Classic inquiry, accumulates devices |
| Onboard LED | WS2812B on **GPIO2** ("RGB 102" silkscreen) — available for status use |

---

## Quick start

### 1. Prerequisites

- [PlatformIO](https://platformio.org/) (VS Code extension or CLI)
- ESP32 board plugged in via USB-C

### 2. Clone / open project

```bash
cd bumptima-esp
```

### 3. Flash firmware

```bash
pio run --target upload
```

### 4. Upload web UI to LittleFS

```bash
pio run --target uploadfs
```

> Both steps are required on first flash. After that you only need to re-run `uploadfs` if you edit `data/index.html`.

### 5. Connect

- Join the **Bumptima** WiFi network (no password)
- Your phone/laptop should auto-redirect to the web UI
- If not, open a browser to `http://192.168.4.1`

---

## Internet passthrough (optional)

The ESP32-WROOM-32UE uses USB-C only for power and serial — it is not a USB host and cannot receive internet over the cable itself.

To share your phone's internet with Bumptima clients:

1. Enable your phone's **mobile hotspot** (creates a separate WiFi network)
2. Edit `src/main.cpp`:
   ```cpp
   static const char STA_SSID[] = "YourHotspotName";
   static const char STA_PASS[] = "YourHotspotPassword";
   ```
3. Re-flash firmware (`pio run --target upload`)

The ESP32 connects to your hotspot as a station, then NAT-routes all traffic from Bumptima clients through that connection. The web UI's live map tiles (CartoDB/OpenStreetMap) will load once clients have internet this way.

---

## Web UI

```
┌──────────────────────────────┐
│  BUMPTIMA         WIFI:12  BT:4  ●  │  ← header
├──────────────────────────────┤
│                              │
│     ┌─────────────┐          │
│     │  OSM MAP    │          │  ← Leaflet dark map inside circle
│     │  + RADAR    │          │
│     │  SWEEP      │          │  ← canvas: rotating sweep + device dots
│     └─────────────┘          │
│         ● 12 WIFI  ◆ 4 BT   │
├──────────────────────────────┤
│ [◉ WIFI]  [◈ BLUETOOTH]      │  ← tabs
│  ──────────────────────────  │
│  ● HomeNetwork   ████  -52dBm│  ← live device list, sorted by signal
│  ● CafeWiFi      ███   -67dBm│     auto-refreshes every 5 s
│  ...                         │
└──────────────────────────────┘
```

- Dots on radar: **green** = WiFi, **blue** = Bluetooth Classic
- Dot distance from centre = signal strength (closer = stronger)
- Dot angle = stable hash of BSSID/BT address (consistent across refreshes)
- Map uses browser Geolocation API; tiles require internet

---

## Board

- **Module:** ESP32-WROOM-32UE (external U.FL antenna — attach antenna before enabling radio)
- **USB-C:** power + serial only (CP210x/CH340)
- **Onboard RGB LED:** WS2812B on **GPIO2**
- **Buttons:** EN (reset), BOOT (GPIO0)
