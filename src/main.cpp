#include <Arduino.h>
#include <WiFi.h>
#include <LittleFS.h>
#include <ArduinoOTA.h>
#include <time.h>
#include "config_manager.h"
#include "portal.h"
#include "radar_fetcher.h"
#include "allsky_engine.h"

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

#define BOOT_BUTTON_PIN 9
#define APP_VERSION "v1.0.0"
#define GITHUB_URL  "github.com/oldjiberjaber/AllskyRadarView"

// ==========================================
// 1. Global App Configuration
// ==========================================
AppConfig appConfig;

// ==========================================
// 2. LovyanGFX Configuration for Dual GC9B72 360x360
// ==========================================
class LGFX_Screen : public lgfx::LGFX_Device {
    lgfx::Panel_GC9B72 _panel_instance;
    lgfx::Bus_SPI      _bus_instance;
    lgfx::Light_PWM    _light_instance;

public:
    LGFX_Screen(int cs_pin, int rst_pin = -1, int bl_pin = -1, int pwm_chan = 0) {
        {
            auto cfg = _bus_instance.config();
            cfg.spi_host   = SPI2_HOST;       // ESP32-C3 FSPI
            cfg.spi_mode   = 0;               // SPI mode 0
            cfg.freq_write = 80000000;        // 80 MHz high-speed SPI clock
            cfg.freq_read  = 16000000;
            cfg.pin_sclk   = 4;               // SCLK = GPIO 4
            cfg.pin_mosi   = 3;               // MOSI = GPIO 3
            cfg.pin_miso   = -1;              // Not connected
            cfg.pin_dc     = 10;              // DC = GPIO 10
            cfg.dma_channel = SPI_DMA_CH_AUTO; // Hardware DMA channel
            _bus_instance.config(cfg);
            _panel_instance.setBus(&_bus_instance);
        }

        {
            auto cfg = _panel_instance.config();
            cfg.pin_cs           = cs_pin;    // Screen 1: GPIO 1, Screen 2: GPIO 2
            cfg.pin_rst          = rst_pin;   // RST: GPIO 0 (or -1)
            cfg.pin_busy         = -1;
            cfg.panel_width      = 360;
            cfg.panel_height     = 360;
            cfg.memory_width     = 360;
            cfg.memory_height    = 360;
            cfg.offset_x         = 0;
            cfg.offset_y         = 0;
            cfg.offset_rotation  = 0;
            cfg.dummy_read_pixel = 8;
            cfg.dummy_read_bits  = 1;
            cfg.readable         = false;
            cfg.invert           = false;
            cfg.rgb_order        = true;
            cfg.dlen_16bit       = false;
            cfg.bus_shared       = true;      // Bus shared for dual panel arbitration
            _panel_instance.config(cfg);
        }

        if (bl_pin >= 0) {
            auto cfg = _light_instance.config();
            cfg.pin_bl      = bl_pin;         // BLK = GPIO 5
            cfg.invert      = false;
            cfg.freq        = 44100;
            cfg.pwm_channel = pwm_chan;
            _light_instance.config(cfg);
            _panel_instance.setLight(&_light_instance);
        }

        setPanel(&_panel_instance);
    }
};

// Screen 1 (CS = GPIO 1, RST = GPIO 0, BL = GPIO 5)
LGFX_Screen gfx_screen1(1, 0, 5, 0);
// Screen 2 (CS = GPIO 2, shared reset/backlight handled by Screen 1)
LGFX_Screen gfx_screen2(2, -1, -1, 1);

// State management
static unsigned long lastRadarRefresh = 0;
static bool needRadarRefresh = true;
static bool needAllskyRefresh = true;

static unsigned long buttonPressStart = 0;
static bool buttonHeld = false;
volatile bool isOtaUpdating = false;

// Carousel State (For single display timed alternation)
enum CarouselPhase { PHASE_RADAR, PHASE_ALLSKY };
static CarouselPhase currentCarouselPhase = PHASE_RADAR;
static unsigned long lastCarouselSwitch = 0;

// UI Boot helper
void drawBootScreen(LovyanGFX &gfx, const char* title, const char* status, int progress = -1, const char* subStatus = nullptr) {
    gfx.startWrite();
    gfx.fillScreen(gfx.color565(8, 14, 24));

    int cx = 180;
    int cy = 180;

    // Scope rings
    gfx.drawCircle(cx, cy, 160, gfx.color565(20, 60, 80));
    gfx.drawCircle(cx, cy, 110, gfx.color565(15, 45, 60));
    gfx.drawCircle(cx, cy, 55, gfx.color565(15, 45, 60));

    // Crosshairs
    gfx.drawFastHLine(cx - 165, cy, 330, gfx.color565(15, 40, 55));
    gfx.drawFastVLine(cx, cy - 165, 330, gfx.color565(15, 40, 55));

    // Title & Version
    gfx.setFont(&fonts::Font0);
    gfx.setTextDatum(textdatum_t::middle_center);
    gfx.setTextColor(gfx.color565(0, 210, 255));
    gfx.drawString(title, cx, 88);

    gfx.setTextColor(gfx.color565(0, 255, 200));
    gfx.drawString(APP_VERSION, cx, 105);

    gfx.setTextColor(gfx.color565(120, 160, 180));
    gfx.drawString("Unified Dual Display", cx, 122);

    // Status Message
    gfx.setTextColor(gfx.color565(255, 255, 255));
    gfx.drawString(status, cx, 195);

    if (subStatus) {
        gfx.setTextColor(gfx.color565(0, 255, 200));
        gfx.drawString(subStatus, cx, 215);
    }

    if (progress >= 0) {
        int barW = 160;
        int barH = 6;
        int fillW = (barW * progress) / 100;
        gfx.drawRoundRect(cx - (barW / 2) - 1, 235, barW + 2, barH + 2, 3, gfx.color565(20, 60, 80));
        gfx.fillRoundRect(cx - (barW / 2), 236, fillW, barH, 2, gfx.color565(0, 255, 200));
    }

    gfx.setTextColor(gfx.color565(60, 130, 160));
    gfx.drawString(GITHUB_URL, cx, 290);
    gfx.endWrite();
}

static int lastOtaPct = -1;

void drawOtaScreen(LovyanGFX &gfx) {
    gfx.startWrite();
    gfx.fillScreen(gfx.color565(8, 14, 24));

    int cx = 180;
    int cy = 180;

    gfx.drawCircle(cx, cy, 160, gfx.color565(20, 60, 80));
    gfx.drawCircle(cx, cy, 110, gfx.color565(15, 45, 60));

    gfx.setFont(&fonts::Font0);
    gfx.setTextDatum(textdatum_t::middle_center);
    gfx.setTextColor(gfx.color565(0, 210, 255));
    gfx.drawString("ALLSKY & RADAR", cx, 110);

    gfx.setTextColor(gfx.color565(120, 160, 180));
    gfx.drawString("WIRELESS FIRMWARE UPDATE", cx, 135);

    int barW = 160;
    int barH = 10;
    gfx.drawRoundRect(cx - (barW / 2) - 1, 219, barW + 2, barH + 2, 4, gfx.color565(25, 70, 95));

    gfx.setTextColor(gfx.color565(255, 255, 255));
    gfx.drawString("STARTING UPDATE...", cx, 195);
    gfx.endWrite();
}

void updateOtaProgress(LovyanGFX &gfx, int pct) {
    int cx = 180;
    int barW = 160;
    int barH = 10;
    int fillW = (barW * pct) / 100;

    gfx.startWrite();
    gfx.fillRect(cx - 70, 185, 140, 20, gfx.color565(8, 14, 24));
    gfx.setFont(&fonts::Font0);
    gfx.setTextDatum(textdatum_t::middle_center);
    gfx.setTextColor(gfx.color565(0, 255, 200));
    char buf[32];
    snprintf(buf, sizeof(buf), "FLASHING: %d%%", pct);
    gfx.drawString(buf, cx, 195);

    if (fillW > 0) {
        gfx.fillRoundRect(cx - (barW / 2), 220, fillW, barH, 3, gfx.color565(0, 255, 200));
    }
    gfx.endWrite();
}

void setupOTA() {
    ArduinoOTA.setPort(3232);
    ArduinoOTA.setHostname("AllskyRadarView");

    ArduinoOTA.onStart([]() {
        isOtaUpdating = true;
        lastOtaPct = -1;
        drawOtaScreen(gfx_screen1);
        if (appConfig.display_mode == MODE_DUAL_DISPLAY) {
            drawOtaScreen(gfx_screen2);
        }
        Serial.println("[OTA] Wireless firmware update started...");
    });

    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        if (total == 0) return;
        int currentPct = (int)((progress * 100ULL) / total);
        if (currentPct != lastOtaPct && (currentPct % 10 == 0 || currentPct == 100)) {
            lastOtaPct = currentPct;
            updateOtaProgress(gfx_screen1, currentPct);
            if (appConfig.display_mode == MODE_DUAL_DISPLAY) {
                updateOtaProgress(gfx_screen2, currentPct);
            }
        }
    });

    ArduinoOTA.onEnd([]() {
        gfx_screen1.startWrite();
        gfx_screen1.fillRect(180 - 80, 185, 160, 20, gfx_screen1.color565(8, 14, 24));
        gfx_screen1.setTextColor(gfx_screen1.color565(0, 255, 200));
        gfx_screen1.drawString("COMPLETE! REBOOTING...", 180, 195);
        gfx_screen1.endWrite();
        Serial.println("[OTA] Firmware update complete");
    });

    ArduinoOTA.onError([](ota_error_t error) {
        Serial.printf("[OTA] Error[%u]\n", error);
        isOtaUpdating = false;
    });

    ArduinoOTA.begin();
    Serial.println("[OTA] Wireless OTA service active on port 3232");
}

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n\n========================================");
    Serial.println("  AllskyRadarView - Dual/Single Display ");
    Serial.println("========================================");

    pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);

    // Initialize Displays
    gfx_screen1.init();
    gfx_screen1.setRotation(0);
    gfx_screen1.setColorDepth(16);

    gfx_screen2.init();
    gfx_screen2.setRotation(0);
    gfx_screen2.setColorDepth(16);

    // Load saved settings
    ConfigManager::load(appConfig);
    gfx_screen1.setBrightness(appConfig.brightness);

    // Set local timezone
    setenv("TZ", appConfig.timezone, 1);
    tzset();

    // Initialize LittleFS
    RadarFetcher::initFS();

    drawBootScreen(gfx_screen1, "ALLSKY RADAR", "CONNECTING WI-FI...", 25);
    if (appConfig.display_mode == MODE_DUAL_DISPLAY) {
        drawBootScreen(gfx_screen2, "RADAR SCOPE", "CONNECTING WI-FI...", 25);
    }

    // Connect to Wi-Fi
    if (ConfigManager::hasValidWiFi(appConfig)) {
        Serial.printf("[WIFI] Connecting to '%s'...\n", appConfig.wifi_ssid);
        WiFi.mode(WIFI_STA);
        WiFi.begin(appConfig.wifi_ssid, appConfig.wifi_pass);

        unsigned long startMs = millis();
        int attempt = 0;
        while (WiFi.status() != WL_CONNECTED && millis() - startMs < 12000) {
            delay(400);
            attempt++;
            int p = 25 + (attempt * 4) % 65;
            drawBootScreen(gfx_screen1, "ALLSKY RADAR", "CONNECTING WI-FI...", p);
            if (appConfig.display_mode == MODE_DUAL_DISPLAY) {
                drawBootScreen(gfx_screen2, "RADAR SCOPE", "CONNECTING WI-FI...", p);
            }
            Serial.print(".");
        }
        Serial.println();
    }

    if (WiFi.status() == WL_CONNECTED) {
        String ipStr = "IP: " + WiFi.localIP().toString();
        Serial.printf("[WIFI] Connected! IP: %s, RSSI: %d dBm\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
        drawBootScreen(gfx_screen1, "ALLSKY RADAR", "WI-FI CONNECTED", 80, ipStr.c_str());
        if (appConfig.display_mode == MODE_DUAL_DISPLAY) {
            drawBootScreen(gfx_screen2, "RADAR SCOPE", "WI-FI CONNECTED", 80, ipStr.c_str());
        }
        delay(1200);

        // Sync Time with NTP
        configTzTime(appConfig.timezone, "pool.ntp.org", "time.nist.gov", "time.google.com");

        // Start OTA & Local Web Portal
        setupOTA();
        Portal::startLocalServer(appConfig);

        // Initialize Engines based on Display Mode
        if (appConfig.display_mode == MODE_DUAL_DISPLAY || 
            appConfig.display_mode == MODE_SINGLE_ALLSKY || 
            appConfig.display_mode == MODE_SINGLE_CAROUSEL) {
            AllskyEngine::init(appConfig);
        }

        needRadarRefresh = true;
        needAllskyRefresh = true;
    } else {
        Serial.println("[WIFI] Failed to connect or no Wi-Fi credentials set. Starting AP mode.");
        Portal::startAP(appConfig);

        gfx_screen1.startWrite();
        gfx_screen1.fillScreen(gfx_screen1.color565(8, 14, 24));
        gfx_screen1.setFont(&fonts::Font0);
        gfx_screen1.setTextDatum(textdatum_t::middle_center);
        gfx_screen1.setTextColor(gfx_screen1.color565(0, 210, 255));
        gfx_screen1.drawString("AP SETUP MODE", 180, 100);

        gfx_screen1.setTextColor(gfx_screen1.color565(255, 255, 255));
        gfx_screen1.drawString("Connect to Wi-Fi:", 180, 140);
        gfx_screen1.setTextColor(gfx_screen1.color565(0, 255, 200));
        gfx_screen1.drawString("AllskyRadar-Setup", 180, 165);

        gfx_screen1.setTextColor(gfx_screen1.color565(255, 255, 255));
        gfx_screen1.drawString("Open browser to:", 180, 205);
        gfx_screen1.setTextColor(gfx_screen1.color565(0, 210, 255));
        gfx_screen1.drawString("http://192.168.4.1", 180, 230);
        gfx_screen1.endWrite();
    }
}

static uint8_t currentAnimFrame = 0;
static unsigned long lastFrameAdvanceTime = 0;

void loop() {
    // Handle OTA
    ArduinoOTA.handle();
    if (isOtaUpdating) return;

    // Handle Web Config Portal
    Portal::handle();

    // Check for live configuration updates from Web Portal
    if (Portal::isLiveRefreshRequested()) {
        const char* pName = (appConfig.data_product == 1) ? "IR SATELLITE" : "PRECIP RADAR";
        Serial.printf("[PORTAL] Configuration updated! Switching live to: %s\n", pName);
        gfx_screen1.setBrightness(appConfig.brightness);

        // Reconnect MQTT if needed
        if (appConfig.display_mode != MODE_SINGLE_RADAR) {
            AllskyEngine::init(appConfig);
        } else {
            AllskyEngine::stop();
        }

        needRadarRefresh = true;
        needAllskyRefresh = true;
    }

    // Check Hardware Button (GPIO 9)
    int btnState = digitalRead(BOOT_BUTTON_PIN);
    if (btnState == LOW) {
        if (!buttonHeld) {
            buttonHeld = true;
            buttonPressStart = millis();
        } else {
            if (millis() - buttonPressStart > 3000) {
                // Long Press (>3s) -> Launch Captive AP Portal
                Serial.println("[BUTTON] Long press detected! Switching to AP setup mode...");
                Portal::startAP(appConfig);
                buttonHeld = false;
                delay(500);
            }
        }
    } else {
        if (buttonHeld) {
            unsigned long duration = millis() - buttonPressStart;
            buttonHeld = false;
            if (duration < 2500) {
                // Short click -> Force instant refresh / advance carousel
                Serial.println("[BUTTON] Click detected!");
                needRadarRefresh = true;
                needAllskyRefresh = true;
                if (appConfig.display_mode == MODE_SINGLE_CAROUSEL) {
                    currentCarouselPhase = (currentCarouselPhase == PHASE_RADAR) ? PHASE_ALLSKY : PHASE_RADAR;
                    lastCarouselSwitch = millis();
                }
            }
        }
    }

    if (WiFi.status() != WL_CONNECTED || Portal::isAPMode()) {
        delay(10);
        return;
    }

    unsigned long now = millis();

    // =========================================================================
    // MODE ROUTING & RENDERING
    // =========================================================================

    // Reference displays for radar and allsky
    LovyanGFX *pRadarGfx = nullptr;
    LovyanGFX *pAllskyGfx = nullptr;

    switch (appConfig.display_mode) {
        case MODE_DUAL_DISPLAY:
            pAllskyGfx = &gfx_screen1; // Screen 1 (CS1): Allsky
            pRadarGfx  = &gfx_screen2; // Screen 2 (CS2): Radar
            break;

        case MODE_SINGLE_RADAR:
            pRadarGfx  = &gfx_screen1; // Screen 1: Radar only
            pAllskyGfx = nullptr;
            break;

        case MODE_SINGLE_ALLSKY:
            pRadarGfx  = nullptr;
            pAllskyGfx = &gfx_screen1; // Screen 1: Allsky only
            break;

        case MODE_SINGLE_CAROUSEL:
            // Check carousel timer
            if (now - lastCarouselSwitch >= ((unsigned long)appConfig.carousel_interval_sec * 1000UL)) {
                lastCarouselSwitch = now;
                currentCarouselPhase = (currentCarouselPhase == PHASE_RADAR) ? PHASE_ALLSKY : PHASE_RADAR;
                Serial.printf("[CAROUSEL] Switched phase to: %s\n", (currentCarouselPhase == PHASE_RADAR) ? "RADAR" : "ALLSKY");
                if (currentCarouselPhase == PHASE_ALLSKY) needAllskyRefresh = true;
            }

            if (currentCarouselPhase == PHASE_RADAR) {
                pRadarGfx = &gfx_screen1;
                pAllskyGfx = nullptr;
            } else {
                pRadarGfx = nullptr;
                pAllskyGfx = &gfx_screen1;
            }
            break;
    }

    // 1. Allsky Rendering
    if (pAllskyGfx) {
        if (AllskyEngine::hasNewImage() || needAllskyRefresh) {
            AllskyEngine::clearNewImageFlag();
            needAllskyRefresh = false;
            AllskyEngine::render(*pAllskyGfx, appConfig);
        }
    }

    // 2. Radar Fetching & Animation
    if (pRadarGfx) {
        unsigned long refreshIntervalMs = (unsigned long)appConfig.refresh_interval_min * 60000UL;

        if (needRadarRefresh || (now - lastRadarRefresh >= refreshIntervalMs)) {
            needRadarRefresh = false;
            lastRadarRefresh = now;

            String statusMsg;
            Serial.println("[RADAR] Fetching updated radar scope from RainViewer...");
            bool ok = RadarFetcher::fetchAllFrames(*pRadarGfx, appConfig, statusMsg);
            if (!ok) {
                Serial.printf("[RADAR] Update failed: %s\n", statusMsg.c_str());
            } else {
                Serial.println("[RADAR] Multi-frame download complete!");
                uint8_t totalF = RadarFetcher::getFrameCount();
                currentAnimFrame = (totalF > 0) ? (totalF - 1) : 0;
                lastFrameAdvanceTime = now;
            }
        } else {
            // Animate cached frames if enabled
            uint8_t totalF = RadarFetcher::getFrameCount();
            if (appConfig.anim_frames > 0 && totalF > 1) {
                bool isLiveFrame = (currentAnimFrame == totalF - 1);
                unsigned long dwellLimit = isLiveFrame ? appConfig.anim_dwell_ms : appConfig.anim_speed_ms;

                if (now - lastFrameAdvanceTime >= dwellLimit) {
                    lastFrameAdvanceTime = now;
                    currentAnimFrame = (currentAnimFrame + 1) % totalF;
                    RadarFetcher::renderFrameIndex(*pRadarGfx, appConfig, currentAnimFrame);
                }
            }
        }
    }

    delay(10);
}

