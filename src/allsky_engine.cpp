#include "allsky_engine.h"
#include <WiFi.h>
#include <LittleFS.h>
#include <mqtt_client.h>
#include <time.h>

static esp_mqtt_client_handle_t s_mqttClient = nullptr;
static uint8_t *s_incomingBuffer = nullptr;
static uint32_t s_totalBytesExpected = 0;
static uint8_t *s_activeImage = nullptr;
static size_t s_activeImageLen = 0;
static bool s_hasNewImage = false;
static uint32_t s_imageCount = 0;
static bool s_isConnected = false;
static AllskyTelemetry s_telemetry;

extern volatile bool isOtaUpdating;

bool AllskyEngine::parseJpgDimensions(const uint8_t *data, size_t len, uint16_t *width, uint16_t *height) {
    if (!data || len < 4) return false;
    if (data[0] != 0xFF || data[1] != 0xD8) return false;
    
    size_t offset = 2;
    while (offset + 4 < len) {
        if (data[offset] != 0xFF) {
            offset++;
            continue;
        }
        while (offset < len && data[offset] == 0xFF) {
            offset++;
        }
        if (offset >= len) break;
        
        uint8_t marker = data[offset++];
        if ((marker >= 0xC0 && marker <= 0xC3) || (marker >= 0xC9 && marker <= 0xCB)) {
            if (offset + 7 <= len) {
                *height = (data[offset + 3] << 8) | data[offset + 4];
                *width  = (data[offset + 5] << 8) | data[offset + 6];
                return true;
            }
            break;
        } else if (marker == 0xD9 || marker == 0xDA) {
            break;
        } else {
            if (offset + 2 > len) break;
            uint16_t segLen = (data[offset] << 8) | data[offset + 1];
            if (segLen < 2) break;
            offset += segLen;
        }
    }
    return false;
}

float AllskyEngine::calculateDctScale(uint16_t w, uint16_t h, uint16_t targetDim) {
    uint16_t minDim = min(w, h);
    if (minDim <= targetDim * 1.35f) {
        return 1.0f;
    } else if (minDim <= targetDim * 2.7f) {
        return 0.5f;
    } else if (minDim <= targetDim * 5.4f) {
        return 0.25f;
    } else {
        return 0.125f;
    }
}

static void allsky_mqtt_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    if (isOtaUpdating) return;

    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            Serial.println("[ALLSKY MQTT] Connected to MQTT broker!");
            s_isConnected = true;
            if (handler_args) {
                const AppConfig *cfg = (const AppConfig*)handler_args;
                esp_mqtt_client_subscribe(s_mqttClient, cfg->mqtt_topic, 0);
                Serial.printf("[ALLSKY MQTT] Subscribed to topic: %s\n", cfg->mqtt_topic);
            }
            break;

        case MQTT_EVENT_DISCONNECTED:
            Serial.println("[ALLSKY MQTT] Disconnected from broker.");
            s_isConnected = false;
            if (s_incomingBuffer) {
                free(s_incomingBuffer);
                s_incomingBuffer = nullptr;
            }
            break;

        case MQTT_EVENT_DATA:
            if (isOtaUpdating) return;

            if (event->current_data_offset == 0) {
                s_totalBytesExpected = event->total_data_len;
                Serial.printf("[ALLSKY MQTT] Incoming thumbnail: %u bytes\n", s_totalBytesExpected);

                if (s_incomingBuffer) {
                    free(s_incomingBuffer);
                    s_incomingBuffer = nullptr;
                }
                s_incomingBuffer = (uint8_t*)malloc(s_totalBytesExpected);
                if (!s_incomingBuffer) {
                    Serial.println("[ALLSKY MQTT] Out of RAM for incoming thumbnail!");
                    return;
                }
            }

            if (s_incomingBuffer && event->data_len > 0) {
                memcpy(s_incomingBuffer + event->current_data_offset, event->data, event->data_len);
            }

            if (event->current_data_offset + event->data_len == event->total_data_len) {
                if (isOtaUpdating || !s_incomingBuffer) return;

                Serial.printf("[ALLSKY MQTT] Complete frame received in RAM (%u bytes). Free heap: %u\n", 
                    s_totalBytesExpected, (unsigned int)ESP.getFreeHeap());

                if (s_activeImage) {
                    free(s_activeImage);
                }
                s_activeImage = s_incomingBuffer;
                s_activeImageLen = s_totalBytesExpected;
                s_incomingBuffer = nullptr;

                s_hasNewImage = true;
                s_imageCount++;

                // Format timestamp
                time_t now = time(nullptr);
                struct tm timeinfo;
                if (localtime_r(&now, &timeinfo) && timeinfo.tm_year > (2020 - 1900)) {
                    strftime(s_telemetry.timestamp, sizeof(s_telemetry.timestamp), "%H:%M:%S", &timeinfo);
                    s_telemetry.hasTelemetry = true;
                }
            }
            break;

        default:
            break;
    }
}

void AllskyEngine::init(const AppConfig &cfg) {
    if (strlen(cfg.mqtt_host) == 0) {
        Serial.println("[ALLSKY] MQTT host empty, skipping Allsky client init.");
        return;
    }

    if (s_mqttClient) {
        stop();
    }

    char uri[128];
    snprintf(uri, sizeof(uri), "mqtt://%s:%u", cfg.mqtt_host, cfg.mqtt_port);

    esp_mqtt_client_config_t mqtt_cfg = {};
    mqtt_cfg.uri = uri;
    if (strlen(cfg.mqtt_user) > 0) mqtt_cfg.username = cfg.mqtt_user;
    if (strlen(cfg.mqtt_pass) > 0) mqtt_cfg.password = cfg.mqtt_pass;
    mqtt_cfg.user_context = (void*)&cfg;
    mqtt_cfg.buffer_size = 4096;

    Serial.printf("[ALLSKY] Connecting to MQTT broker: %s\n", uri);
    s_mqttClient = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(s_mqttClient, (esp_mqtt_event_id_t)ESP_EVENT_ANY_ID, allsky_mqtt_handler, (void*)&cfg);
    esp_mqtt_client_start(s_mqttClient);
}

void AllskyEngine::stop() {
    if (s_mqttClient) {
        Serial.println("[ALLSKY] Stopping MQTT client...");
        esp_mqtt_client_stop(s_mqttClient);
        esp_mqtt_client_destroy(s_mqttClient);
        s_mqttClient = nullptr;
        s_isConnected = false;
    }
    if (s_incomingBuffer) {
        free(s_incomingBuffer);
        s_incomingBuffer = nullptr;
    }
    if (s_activeImage) {
        free(s_activeImage);
        s_activeImage = nullptr;
        s_activeImageLen = 0;
    }
}

bool AllskyEngine::hasNewImage() {
    return s_hasNewImage;
}

void AllskyEngine::clearNewImageFlag() {
    s_hasNewImage = false;
}

uint32_t AllskyEngine::getImageCount() {
    return s_imageCount;
}

const AllskyTelemetry& AllskyEngine::getTelemetry() {
    return s_telemetry;
}

bool AllskyEngine::isConnected() {
    return s_isConnected;
}

bool AllskyEngine::render(LovyanGFX &gfx, const AppConfig &cfg) {
    if (!s_activeImage || s_activeImageLen < 100) {
        // Render sleek AllskyView connected/waiting screen
        gfx.startWrite();
        gfx.fillScreen(gfx.color565(8, 14, 24));
        gfx.drawCircle(180, 180, 174, 0x1A8F);

        gfx.setTextDatum(textdatum_t::middle_center);

        // Header
        gfx.setFont(&fonts::Font2);
        gfx.setTextColor(s_isConnected ? TFT_GREEN : gfx.color565(0, 210, 255));
        gfx.drawString(s_isConnected ? "Allsky Connected" : "Connecting to MQTT...", 180, 95);

        // Topic Pill Badge
        {
            gfx.setFont(&fonts::Font2);
            int tw = gfx.textWidth(cfg.mqtt_topic);
            int th = gfx.fontHeight();
            int badgeY = 145;
            gfx.fillRoundRect(180 - (tw / 2) - 12, badgeY - (th / 2) - 5, tw + 24, th + 10, 8, 0x10A2);
            gfx.drawRoundRect(180 - (tw / 2) - 12, badgeY - (th / 2) - 5, tw + 24, th + 10, 8, 0x39E7);
            gfx.setTextColor(TFT_CYAN);
            gfx.drawString(cfg.mqtt_topic, 180, badgeY);
        }

        // IP Address Badge
        {
            String ipStr = "IP: " + WiFi.localIP().toString();
            gfx.setFont(&fonts::Font2);
            int ipW = gfx.textWidth(ipStr.c_str());
            int ipH = gfx.fontHeight();
            int ipY = 195;
            gfx.fillRoundRect(180 - (ipW / 2) - 10, ipY - (ipH / 2) - 4, ipW + 20, ipH + 8, 6, 0x0841);
            gfx.drawRoundRect(180 - (ipW / 2) - 10, ipY - (ipH / 2) - 4, ipW + 20, ipH + 8, 6, 0x2945);
            gfx.setTextColor(0x9E79);
            gfx.drawString(ipStr.c_str(), 180, ipY);
        }

        // Subtitle
        gfx.setFont(&fonts::Font2);
        gfx.setTextColor(TFT_WHITE);
        gfx.drawString("Waiting for capture...", 180, 255);
        gfx.endWrite();
        return true;
    }

    uint16_t origW = 0, origH = 0;
    float scale = 1.0f;
    int drawX = 0, drawY = 0;

    if (parseJpgDimensions(s_activeImage, s_activeImageLen, &origW, &origH) && origW > 0 && origH > 0) {
        scale = calculateDctScale(origW, origH, 360);
        int renderedW = (int)(origW * scale);
        int renderedH = (int)(origH * scale);
        drawX = (360 - renderedW) / 2;
        drawY = (360 - renderedH) / 2;
    }

    // Format local time fallback if needed
    char timeBuf[32] = "";
    if (s_telemetry.hasTelemetry && s_telemetry.timestamp[0] != '\0') {
        strncpy(timeBuf, s_telemetry.timestamp, sizeof(timeBuf) - 1);
    } else {
        time_t now = time(nullptr);
        struct tm timeinfo;
        if (localtime_r(&now, &timeinfo) && timeinfo.tm_year > (2020 - 1900)) {
            strftime(timeBuf, sizeof(timeBuf), "%H:%M:%S", &timeinfo);
        }
    }

    gfx.startWrite();
    gfx.fillScreen(TFT_BLACK);

    // Decode directly from RAM buffer (zero storage)
    bool ok = gfx.drawJpg(s_activeImage, s_activeImageLen, drawX, drawY, 0, 0, 0, 0, scale, scale);

    if (ok) {
        // Top subtle label
        gfx.setTextDatum(textdatum_t::middle_center);
        gfx.setTextColor(0x7BEF);
        gfx.setFont(&fonts::Font2);
        gfx.drawString("ALLSKY", 180, 18);

        // Bottom timestamp badge
        if (timeBuf[0] != '\0') {
            gfx.setFont(&fonts::Font2);
            int textW = gfx.textWidth(timeBuf);
            int textH = gfx.fontHeight();

            int badgeY = 328;
            int badgeX = 180 - (textW / 2) - 14;
            int badgeW = textW + 28;
            int badgeH = textH + 8;

            gfx.fillRoundRect(badgeX, badgeY - (textH / 2) - 4, badgeW, badgeH, 6, TFT_BLACK);
            gfx.drawRoundRect(badgeX, badgeY - (textH / 2) - 4, badgeW, badgeH, 6, 0x5AEB);

            gfx.setTextColor(TFT_CYAN);
            gfx.drawString(timeBuf, 180, badgeY);
        }
    }

    gfx.endWrite();
    return ok;
}

