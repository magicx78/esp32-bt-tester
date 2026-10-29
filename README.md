# ESP32 Bluetooth-Tester (Scan · Connect · Piepton)

Eigenständiges ESP-IDF-Firmware-Projekt für den **Original-ESP32** (WROOM/WROVER,
Dual-Mode BR/EDR + BLE). Stellt eine **Weboberfläche über einen eigenen WLAN-Accesspoint**
bereit, mit der man:

1. **BT-Geräte scannt** (Classic **und** BLE) und als Liste sieht,
2. sich mit einem gewählten Gerät **verbindet** (nur verbinden),
3. einen **1-kHz-Piepton mit wählbarer Dauer** an ein gewähltes Audio-Gerät sendet.

> ⚠️ **Nur für autorisierte Tests an eigenen Geräten.** Das Verbinden mit fremden
> Bluetooth-Geräten oder das Senden von Tönen an sie ohne Erlaubnis kann rechtswidrig
> sein. Benutze dieses Werkzeug ausschließlich für Geräte, die dir gehören oder für die
> du eine ausdrückliche Testfreigabe hast.

## Funktionsweise (kurz)

- **Ton** = der ESP32 verbindet sich als **A2DP-Source** (die „Quelle"/„Zuspieler") mit
  dem Lautsprecher/Kopfhörer (A2DP-Sink) und streamt einen erzeugten 1-kHz-Sinus.
- **Scan** = Classic-Inquiry + BLE-Scan werden gemergt. In der Liste sind
  **verbindbare Audio-Geräte grün hervorgehoben**, sonstige Classic-Geräte blau,
  **BLE-Geräte grau (nur zur Info — sie können keinen Ton empfangen)**.
- **Pairing** nutzt SSP mit IO-Capability *NoInputNoOutput* (**Just Works**) und
  bestätigt automatisch, damit das Verbinden möglichst ohne Nachfrage klappt.

## Hardware

- **Original-ESP32** (WROOM-32 oder WROVER). **WROVER (mit PSRAM) empfohlen**, da
  Dual-Mode-BT + WLAN + A2DP speicherintensiv sind.
- **Nicht möglich** auf ESP32-S3 / C3 / C6 / H2 — diese haben **kein Bluetooth Classic**.

## Build & Flash (ESP-IDF v5.x)

```bash
idf.py set-target esp32
idf.py build
idf.py -p <PORT> flash monitor
```

Wichtige Optionen stehen bereits in `sdkconfig.defaults` (Dual-Mode-BT, A2DP,
WLAN/BT-Koexistenz, Partitionstabelle). Für ein **WROVER-Board** zusätzlich in
`menuconfig` bzw. `sdkconfig.defaults` PSRAM aktivieren:

```
CONFIG_SPIRAM=y
CONFIG_SPIRAM_USE_MALLOC=y
```

## Benutzung

1. ESP32 flashen und starten.
2. Mit dem WLAN **`ESP32-BT-Tester`** (Passwort **`bttester123`**) verbinden.
3. Im Browser **http://192.168.4.1/** öffnen.
4. **Scan starten** → Gerät in der Liste anklicken.
5. **Verbinden** oder Dauer eingeben und **Piepton senden**.

SSID/Passwort stehen oben in `main/app_main.c` (`AP_SSID` / `AP_PASS`).

## Projektstruktur

| Datei | Zweck |
|-------|-------|
| `main/app_main.c`   | Init: NVS, WLAN-SoftAP, BT Dual-Mode, Module; Adress-Helfer |
| `main/bt_scan.c`    | Classic-Inquiry + BLE-Scan, Geräte-Tabelle, SSP-Auto-Accept |
| `main/a2dp_src.c`   | A2DP-Source: Verbinden/Trennen/Piepton (Zustandsautomat) |
| `main/tone_gen.c`   | 1-kHz-Sinus (44.1 kHz / 16 bit / Stereo) |
| `main/web_server.c` | HTTP-Server + JSON-API |
| `main/index.html`   | Weboberfläche (farbcodierte Liste) |
| `main/bt_tester.h`  | gemeinsame Typen/Schnittstellen |

## Grenzen / bekannte Punkte

- Der Scan findet nur **sichtbare (discoverable)** Classic-Geräte. Ein Lautsprecher/
  Kopfhörer muss dafür meist im **Pairing-Modus** sein. Bereits mit etwas anderem
  verbundene oder nicht sichtbare Geräte tauchen nicht auf.
- Der **Piepton braucht eine A2DP-Sink** (Lautsprecher/Kopfhörer). Reine
  Nicht-A2DP-Geräte und BLE-Geräte können ihn nicht abspielen.
- Manche Geräte verlangen trotz *Just Works* eine Bestätigung am Gerät und lassen sich
  dann nicht völlig ohne Interaktion verbinden.
- WLAN und BT teilen sich einen Funk-Core → bei schwachem Signal sind kurze
  Aussetzer möglich. WROVER + kurze Distanz helfen.
- Es ist immer **nur eine aktive Verbindung** gleichzeitig vorgesehen.

## Getestet gegen

ESP-IDF v5.1–v5.4. Bei abweichenden Versionen können einzelne API-/Enum-Namen
(A2DP-Audiozustände, BLE-Adv-Resolver) leicht abweichen — siehe Kommentare im Code.
