# AllskyRadarView 🌌🌧️

[![Build & Release](https://github.com/oldjiberjaber/AllskyRadarView/actions/workflows/release.yml/badge.svg)](https://github.com/oldjiberjaber/AllskyRadarView/actions/workflows/release.yml)
[![Release](https://img.shields.io/github/v/release/oldjiberjaber/AllskyRadarView)](https://github.com/oldjiberjaber/AllskyRadarView/releases)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

**AllskyRadarView** is a unified, high-performance ESP32-C3 firmware capable of driving **Dual GC9B72 360×360 round LCD displays** (or a single display in radar, allsky, or timed carousel mode).

It bridges your local **Allsky Camera** (via MQTT) and real-time **Weather Radar & Satellite Motion Loops** (via RainViewer & Open-Meteo) into a tactical round observatory HUD.

---

## 📷 Display Preview

| Tactical Weather Radar Scope | Allsky Night Sky Camera View |
|:---:|:---:|
| ![Radar Scope](assets/radar_view.jpg) | ![Allsky Camera](assets/allsky_view.jpg) |
| *Live RainViewer radar, Open-Meteo telemetry & wind vector HUD* | *MQTT Allsky camera feed with capture timestamp* |

---

## 🌟 Key Features

- 🖥️ **Flexible Multi-Display Modes**:
  - **Dual Display**: Screen 1 (CS1) shows live Allsky Camera stream; Screen 2 (CS2) runs the live RainViewer weather radar motion loop & telemetry.
  - **Single Radar**: Single display dedicated to full radar motion loop and weather scope.
  - **Single Allsky**: Single display dedicated to the MQTT Allsky Camera feed.
  - **Timed Carousel**: Single display smoothly alternating between Allsky and Radar every *N* seconds (configurable from 5s to 300s).
- 🌧️ **Live Radar & Satellite Motion Loop**:
  - Multi-frame history animation (3, 5, or 8 frames covering 20–80 minutes of storm motion).
  - Stream-decoded via LittleFS with **zero heap fragmentation** and full DMA acceleration.
  - Switch on-the-fly between **Precipitation Radar** and **Infrared Satellite Cloud Cover**.
- 🧭 **Tactical Weather Telemetry**:
  - Real-time temperature, humidity, and 16-point cardinal wind speed & direction (`SW`, `ENE`, `NNE`) from Open-Meteo.
  - Compass needle vector on the radar scope.
- ⚡ **Zero-Lag Shared SPI Bus**:
  - Both GC9B72 360×360 panels operate on the ESP32-C3 FSPI bus clocked at **80 MHz** with hardware DMA.
- 🌐 **Comprehensive Web Configuration Portal & Captive AP**:
  - Interactive **Leaflet.js map picker** for instant GPS geolocation and scope radius visualization.
  - Wi-Fi network scanner.
  - MQTT broker configuration for indi-allsky / Allsky camera servers.
  - Live color palette selection (Universal Blue, NEXRAD, Rainbow, TITAN, Dark Sky, etc.).
- 📶 **Over-The-Air (OTA) Updates**:
  - Fast OTA reflashing with a live tactical circular progress HUD.

---

## 📐 Hardware Wiring Pinout

AllskyRadarView uses an **ESP32-C3 SuperMini** driving one or two **GC9B72 360×360 round SPI LCDs**.

| ESP32-C3 Pin | Screen 1 (Allsky / Main) | Screen 2 (Radar Scope) | Description |
|---|---|---|---|
| **GPIO 4** | `SCL` / `SCLK` | `SCL` / `SCLK` | Shared 80 MHz SPI Clock |
| **GPIO 3** | `SDA` / `MOSI` | `SDA` / `MOSI` | Shared SPI MOSI Data |
| **GPIO 10** | `DC` | `DC` | Shared Data / Command |
| **GPIO 0** | `RST` | `RST` | Shared Hardware Reset |
| **GPIO 5** | `BLK` / `BL` | `BLK` / `BL` | Shared Backlight PWM (44.1 kHz LEDC) |
| **GPIO 1** | **`CS` (Chip Select 1)** | — | Primary Screen Chip Select |
| **GPIO 2** | — | **`CS` (Chip Select 2)** | Secondary Screen Chip Select |
| **GPIO 9** | Button | — | Boot Button (Short press: Refresh / Long press: AP mode) |
| **3V3 / 5V** | `VCC` | `VCC` | Power Supply |
| **GND** | `GND` | `GND` | Common Ground |

---

## 🚀 Getting Started

### 1. Build & Flash via PlatformIO

```bash
# Clone the repository
git clone https://github.com/oldjiberjaber/AllskyRadarView.git
cd AllskyRadarView

# Flash via USB COM Port
pio run -e esp32-c3-usb -t upload

# Or Flash wirelessly via Over-The-Air (OTA)
pio run -e esp32-c3-ota -t upload
```

### 2. First Boot & Setup
1. If no Wi-Fi credentials are stored, the device starts an Access Point: `AllskyRadar-Setup`.
2. Connect to the Wi-Fi network and open `http://192.168.4.1` (or click the captive portal prompt).
3. Select your **Operating Mode** (Dual Screen, Single Radar, Single Allsky, or Carousel).
4. Configure your **Wi-Fi**, **Allsky MQTT topic**, and **Radar Geolocation** using the interactive map.
5. Hit **Save & Apply**!

---

## 🛠️ Architecture & Memory Strategy

The ESP32-C3 features ~320 KB SRAM. Decoding full PNG and JPEG images simultaneously requires strict memory architecture:
- **LittleFS Stream Decoding**: Incoming PNG radar tiles and JPEG Allsky images are written sequentially to LittleFS and decoded via file stream callbacks (`drawPng(&file)` and `drawJpg(&file)`), preventing RAM fragmentation and leaving $>180\text{ KB}$ contiguous free memory.
- **DCT Scaling**: Incoming Allsky JPEG images are prescaled in hardware DCT down to the target 360×360 panel size.

---

## 📄 License

Distributed under the MIT License. See `LICENSE` for details.

