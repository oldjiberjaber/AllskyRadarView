#include "allsky_engine.h"
#include <WiFi.h>
#include <LittleFS.h>
#include <mqtt_client.h>
#include <time.h>

static esp_mqtt_client_handle_t s_mqttClient = nullptr;
static uint8_t *s_incomingBuffer = nullptr;
static uint32_t s_totalBytesExpected = 0;
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

                Serial.printf("[ALLSKY MQTT] Complete frame received (%u bytes). Saving to LittleFS...\n", s_totalBytesExpected);
                
                // Save JPEG to LittleFS file to keep heap completely free
                File f = LittleFS.open("/allsky_latest.jpg", "w");
                if (f) {
                    f.write(s_incomingBuffer, s_totalBytesExpected);
                    f.flush();
                    f.close();
                    s_hasNewImage = true;
                    s_imageCount++;
                }

                free(s_incomingBuffer);
                s_incomingBuffer = nullptr;

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
    if (!LittleFS.exists("/allsky_latest.jpg")) {
        // Render waiting screen
        gfx.startWrite();
        gfx.fillScreen(gfx.color565(8, 14, 24));
        gfx.drawCircle(180, 180, 174, gfx.color565(0, 180, 255));

        gfx.setFont(&fonts::Font2);
        gfx.setTextDatum(textdatum_t::middle_center);
        gfx.setTextColor(gfx.color565(0, 255, 200));
        gfx.drawString("ALLSKY CAMERA", 180, 100);

        gfx.fillRoundRect(180 - 100, 140, 200, 26, 6, gfx.color565(12, 24, 38));
        gfx.drawRoundRect(180 - 100, 140, 200, 26, 6, gfx.color565(0, 180, 255));
        gfx.setTextColor(gfx.color565(0, 220, 255));
        gfx.drawString(cfg.mqtt_topic, 180, 153);

        gfx.setTextColor(gfx.color565(180, 200, 220));
        gfx.drawString(s_isConnected ? "Connected: Waiting for frame..." : "Connecting to MQTT...", 180, 220);
        gfx.endWrite();
        return true;
    }

    File f = LittleFS.open("/allsky_latest.jpg", "r");
    if (!f) return false;

    size_t fileSize = f.size();
    if (fileSize < 100) {
        f.close();
        return false;
    }

    // Read header to parse dimensions
    uint8_t hdr[1024];
    size_t hdrRead = f.read(hdr, sizeof(hdr));
    f.seek(0);

    uint16_t origW = 0, origH = 0;
    float scale = 1.0f;
    int drawX = 0, drawY = 0;

    if (parseJpgDimensions(hdr, hdrRead, &origW, &origH) && origW > 0 && origH > 0) {
        scale = calculateDctScale(origW, origH, 360);
        int renderedW = (int)(origW * scale);
        int renderedH = (int)(origH * scale);
        drawX = (360 - renderedW) / 2;
        drawY = (360 - renderedH) / 2;
    }

    gfx.startWrite();
    gfx.fillScreen(gfx.color565(4, 8, 14));

    // Decode directly from LittleFS stream
    bool ok = gfx.drawJpg(&f, drawX, drawY, 0, 0, 0, 0, scale, scale);
    f.close();

    if (ok) {
        // Top Header Badge
        gfx.setFont(&fonts::Font2);
        gfx.setTextDatum(textdatum_t::middle_center);
        gfx.fillRoundRect(180 - 65, 18, 130, 22, 11, gfx.color565(10, 18, 28));
        gfx.drawRoundRect(180 - 65, 18, 130, 22, 11, gfx.color565(0, 200, 255));
        gfx.setTextColor(gfx.color565(0, 230, 255));
        gfx.drawString("ALLSKY CAM", 180, 29);

        // Bottom Capture Time Badge
        if (s_telemetry.hasTelemetry && s_telemetry.timestamp[0] != '\0') {
            gfx.fillRoundRect(180 - 65, 320, 130, 22, 11, gfx.color565(10, 18, 28));
            gfx.drawRoundRect(180 - 65, 320, 130, 22, 11, gfx.color565(0, 255, 200));
            gfx.setTextColor(gfx.color565(0, 255, 200));
            gfx.drawString(s_telemetry.timestamp, 180, 331);
        }
    }

    gfx.endWrite();
    return ok;
}

