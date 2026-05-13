# DOSPANEL v5.0

A retro-style DOS system monitor running on the **JC3248W535** display module (ESP32-S3, 480×320, capacitive touch). Displays real-time data sent by a TSR program running on a vintage DOS PC, with built-in games, audio player, WiFi web interface, and microSD support.

![DOSPANEL Screenshot](/screenshot.jpg)

---

## Features

- **5 touch-navigable pages**
  - Page 1 — Clock, RAM, DOS version, drive activity + **Space Invaders** (playable)
  - Page 2 — CPU, BIOS, drive usage, volume label + **Snake** (playable)
  - Page 3 — Keyboard LEDs, mouse, COM port, open files + **Pong** (playable)
  - Page 4 — Simulated stereo VU meter (real input when INMP441 is connected)
  - Page 5 — MP3/WAV audio player with waveform visualizer
- **WiFi + WebSocket** — live data mirrored in a browser, identical canvas rendering
- **MicroSD support** — splash screen, CSV data logging, file manager via browser
- **Audio player** — MP3 and WAV playback from `/AUDIO` folder on SD card
- **Web file manager** — upload, download, delete files on the SD card from any browser
- **VUMONITOR.EXE** — TSR DOS program that sends system data via COM1 serial port

---

## Hardware

| Component | Notes |
|-----------|-------|
| JC3248W535 | ESP32-S3, 480×320 IPS, capacitive touch, WiFi, I2S audio amp |
| MAX3232 module | RS-232 to 3.3V TTL converter, DB9 female connector |
| MicroSD card | Class 10, FAT32 formatted |
| Speaker | 4–8 ohm, 1–3W, connected to P6/P7 |
| INMP441 *(optional)* | I2S digital microphone for real VU meter |

---

## Wiring

### Power — Molex PC connector → P1

| Molex Pin | Color | Connect to |
|-----------|-------|-----------|
| 4 | Red | P1 +5V |
| 2 or 3 | Black | P1 GND |
| 1 | Yellow | **Do not connect** |

### Serial RS-232 — MAX3232 → P3 or P4

| MAX3232 | Connect to |
|---------|-----------|
| VCC | P3/P4 Pin 2 (3.3V) |
| GND | P3/P4 Pin 1 (GND) |
| TX | P3/P4 Pin 4 (GPIO 18) |
| DB9 female | COM1 on DOS PC |

### Microphone INMP441 *(optional)*

| INMP441 Pin | GPIO |
|-------------|------|
| VDD | 3.3V (from P3/P4 pin 2) |
| GND | GND (from P3/P4 pin 1) |
| SD | GPIO 17 |
| SCK | GPIO 15 |
| WS | GPIO 16 |
| L/R | GND (left channel) |

### MicroSD — internal, no external wiring needed

The SD card slot is wired internally to the ESP32-S3 via HSPI:

| Signal | GPIO |
|--------|------|
| CS | 10 |
| MOSI | 11 |
| CLK | 12 |
| MISO | 13 |

### Speaker

Connect directly to **P6 or P7** — the onboard I2S amplifier handles everything. GPIO 41/2/42 are wired internally.

---

## Software Requirements

### Arduino Libraries

Install these via the Arduino IDE Library Manager:

| Library | Author |
|---------|--------|
| JC3248W535 | me-processware (GitHub) |
| GFX Library for Arduino | moononournation |
| WebSockets | Markus Sattler |
| ESP8266Audio | Earle F. Philhower III |

> **Important:** Use the ESP32 built-in `SD` library. If you have a standalone `SD` library installed under `Arduino/libraries`, remove it to avoid conflicts.

### Board Settings (Arduino IDE)

| Setting | Value |
|---------|-------|
| Board | ESP32S3 Dev Module |
| Flash Size | 8MB |
| Partition Scheme | 8M with spiffs |
| PSRAM | OPI PSRAM |
| Upload Speed | 921600 |

---

## MicroSD Card Structure

Format the card as **FAT32** and place these files in the root:

```
/
├── SPLASH.BMP       ← 480×320 24-bit BMP splash screen (optional)
├── INDEX.HTM        ← Web interface (served at http://[IP]/)
├── DATALOG.CSV      ← Created automatically on first boot
└── AUDIO/
    ├── SONG01.MP3
    ├── SONG02.WAV
    └── ...
```

---

## WiFi Configuration

Edit these lines at the top of `DOSPANEL.ino` before uploading:

```cpp
#define WIFI_SSID  "YourNetworkName"
#define WIFI_PASS  "YourPassword"
```

Once connected, the display shows the IP address. Open it in any browser on the same network.

---

## Web Interface

Access `http://[display IP]/` for the full web interface. Available endpoints:

| Endpoint | Description |
|----------|-------------|
| `/` | Web interface (served from SD) |
| `/files` | File manager — upload, download, delete |
| `/datalog.csv` | Download the CSV data log |
| `/playlist` | JSON list of audio tracks |
| `/playerstatus` | JSON player state |
| `/player?cmd=...` | Player control (play, pause, stop, next, prev, vol, reload) |

---

## VUMONITOR.EXE — DOS TSR

`VUMONITOR.C` is a Terminate-and-Stay-Resident program for MS-DOS that hooks INT 8h (18.2 Hz timer) and sends a data packet via COM1 every ~1 second.

### Compile

```
wcl -ms -os VUMONITOR.C
```

Requires **Watcom C** compiler.

### Usage

```
VUMONITOR        Install TSR
VUMONITOR /U     Uninstall TSR
VUMONITOR /Q     Query status
```

### Packet Format

```
$RAM:512;14:32:07;09/04/2026;DOS:7.1;DRV:0;CPU:486;BIOS:AMI-010192;DSK:C:204800;VOL:MSDOS;KBD:010;MOUSE:1,160,120,0;COM:9600;FILES:15
```

---

## Touch Controls

### Page navigation
- Touch **right edge** → next page
- Touch **left edge** → previous page

### Games (bottom area of pages 1–3)
| Page | Game | Controls |
|------|------|----------|
| 1 | Space Invaders | Left/right = move ship, center = fire |
| 2 | Snake | Tap in the direction you want to go |
| 3 | Pong | Drag finger up/down to control left paddle |

### Audio Player (page 5)
Touch the buttons drawn on screen: `<< PREV`, `>> PLAY`, `|| PAUSE`, `[] STOP`, `NEXT >>`, `V-`, `V+`

---

## Project Structure

```
DOSPANEL/
├── DOSPANEL.ino        ← Main Arduino firmware
├── VUMONITOR.C         ← DOS TSR source code
├── INDEX.HTM           ← Web interface (copy to SD card)
├── SPLASH.BMP          ← Splash screen (copy to SD card)
└── README.md
```

---

## License

MIT License — feel free to use, modify, and share.

---

## Credits

Developed by Francesco Paolo Patti 2026.  
Hardware: JC3248W535 by Shenzhen Jingcai Intelligent Co., Ltd.
