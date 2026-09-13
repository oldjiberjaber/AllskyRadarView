#include "radar_fetcher.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <math.h>

static RadarFrameInfo s_frameMeta[MAX_RADAR_FRAMES];
static uint8_t s_totalFrames = 0;
static WeatherTelemetry s_latestTelemetry;
static bool s_fsInitialized = false;

bool RadarFetcher::initFS() {
    if (s_fsInitialized) return true;
    if (!LittleFS.begin(true)) {
        Serial.println("[FS] Failed to mount LittleFS!");
        return false;
    }
    s_fsInitialized = true;
    Serial.printf("[FS] LittleFS mounted successfully. Total: %u KB, Used: %u KB\n",
        (unsigned int)(LittleFS.totalBytes() / 1024), (unsigned int)(LittleFS.usedBytes() / 1024));
    return true;
}

uint8_t RadarFetcher::getFrameCount() {
    return s_totalFrames;
}

const WeatherTelemetry& RadarFetcher::getLatestTelemetry() {
    return s_latestTelemetry;
}

bool RadarFetcher::queryMetadataMulti(const AppConfig &cfg, String &hostUrl, RadarFrameInfo *outFrames, uint8_t maxFrames, uint8_t &outCount) {
    outCount = 0;
    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;
    http.setConnectTimeout(5000);
    http.setTimeout(8000);
    http.setReuse(false);

    const char* apiUrl = "https://api.rainviewer.com/public/weather-maps.json";
    Serial.printf("[RADAR] Querying RainViewer API: %s (Free Heap: %u bytes)\n", apiUrl, (unsigned int)ESP.getFreeHeap());

    if (!http.begin(client, apiUrl)) {
        Serial.println("[RADAR] Failed to begin HTTP connection to RainViewer API");
        return false;
    }

    int httpCode = http.GET();
    if (httpCode != HTTP_CODE_OK) {
        Serial.printf("[RADAR] HTTP GET error: %d (%s)\n", httpCode, http.errorToString(httpCode).c_str());
        http.end();
        return false;
    }

    WiFiClient *stream = http.getStreamPtr();

    JsonDocument filter;
    filter["host"] = true;
    filter["radar"]["past"][0]["time"] = true;
    filter["radar"]["past"][0]["path"] = true;
    filter["satellite"]["infrared"][0]["time"] = true;
    filter["satellite"]["infrared"][0]["path"] = true;

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, *stream, DeserializationOption::Filter(filter));
    http.end();

    if (error) {
        Serial.printf("[RADAR] JSON stream parsing failed: %s\n", error.c_str());
        return false;
    }

    hostUrl = doc["host"] | "https://tilecache.rainviewer.com";
    JsonArray frames;
    bool isSatellite = (cfg.data_product == 1);
    bool isFallback = false;

    if (isSatellite) {
        frames = doc["satellite"]["infrared"].as<JsonArray>();
        if (frames.isNull() || frames.size() == 0) {
            Serial.println("[RADAR] RainViewer satellite IR empty; falling back to precipitation radar");
            frames = doc["radar"]["past"].as<JsonArray>();
            isFallback = true;
        }
    } else {
        frames = doc["radar"]["past"].as<JsonArray>();
    }

    if (frames.isNull() || frames.size() == 0) {
        Serial.println("[RADAR] No imagery frames found in API response");
        return false;
    }

    size_t totalAvail = frames.size();
    uint8_t countToFetch = (maxFrames == 0) ? 1 : ((maxFrames > totalAvail) ? totalAvail : maxFrames);
    if (countToFetch > MAX_RADAR_FRAMES) countToFetch = MAX_RADAR_FRAMES;

    size_t startOffset = totalAvail - countToFetch;
    for (uint8_t i = 0; i < countToFetch; i++) {
        JsonObject obj = frames[startOffset + i];
        outFrames[i].timestamp = obj["time"] | 0;
        const char* p = obj["path"] | "";
        strncpy(outFrames[i].path, p, sizeof(outFrames[i].path) - 1);
        outFrames[i].path[sizeof(outFrames[i].path) - 1] = '\0';
        outFrames[i].valid = true;
        outFrames[i].is_satellite = isSatellite;
        outFrames[i].is_fallback = isFallback;

        time_t rawtime = (time_t)outFrames[i].timestamp;
        struct tm frame_tm;
        localtime_r(&rawtime, &frame_tm);
        snprintf(outFrames[i].formatted_time, sizeof(outFrames[i].formatted_time), "%02d:%02d", frame_tm.tm_hour, frame_tm.tm_min);
    }

    outCount = countToFetch;
    Serial.printf("[RADAR] Metadata loaded: %u frames queued (Range: %s to %s)\n", 
        outCount, outFrames[0].formatted_time, outFrames[outCount - 1].formatted_time);
    return true;
}

bool RadarFetcher::queryTelemetry(const AppConfig &cfg, WeatherTelemetry &telemetry) {
    if (!cfg.show_telemetry) return false;

    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;
    http.setConnectTimeout(4000);
    http.setTimeout(6000);
    http.setReuse(false);

    char url[256];
    snprintf(url, sizeof(url), "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f&current=temperature_2m,relative_humidity_2m,wind_speed_10m,wind_direction_10m&timezone=auto", cfg.latitude, cfg.longitude);

    Serial.printf("[TELEM] Querying Open-Meteo: %s\n", url);
    if (!http.begin(client, url)) return false;

    int httpCode = http.GET();
    if (httpCode != HTTP_CODE_OK) {
        Serial.printf("[TELEM] HTTP GET error: %d (%s)\n", httpCode, http.errorToString(httpCode).c_str());
        http.end();
        return false;
    }

    String payload = http.getString();
    http.end();

    if (payload.length() == 0) {
        Serial.println("[TELEM] Empty response from Open-Meteo");
        return false;
    }

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, payload);
    if (error) {
        Serial.printf("[TELEM] JSON parse error: %s\n", error.c_str());
        return false;
    }

    telemetry.temperature = doc["current"]["temperature_2m"] | 0.0f;
    telemetry.humidity = doc["current"]["relative_humidity_2m"] | 0;
    telemetry.wind_speed = doc["current"]["wind_speed_10m"] | 0.0f;
    telemetry.wind_direction = doc["current"]["wind_direction_10m"] | 0;
    telemetry.valid = true;

    Serial.printf("[TELEM] Temp: %.1f°C, Humidity: %d%%, Wind: %.1f km/h @ %d°\n",
        telemetry.temperature, telemetry.humidity, telemetry.wind_speed, telemetry.wind_direction);
    return true;
}

bool RadarFetcher::downloadTileToFile(const String &url, const char *filePath) {
    initFS();

    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;
    http.setConnectTimeout(6000);
    http.setTimeout(10000);
    http.setReuse(false);

    Serial.printf("[RADAR] Downloading Tile to %s: %s (Free Heap: %u bytes)\n", filePath, url.c_str(), (unsigned int)ESP.getFreeHeap());
    if (!http.begin(client, url)) {
        Serial.println("[RADAR] Failed to begin HTTP connection for tile");
        return false;
    }

    int httpCode = http.GET();
    if (httpCode != HTTP_CODE_OK) {
        Serial.printf("[RADAR] Tile download error: %d (%s)\n", httpCode, http.errorToString(httpCode).c_str());
        http.end();
        return false;
    }

    File f = LittleFS.open(filePath, "w");
    if (!f) {
        Serial.printf("[RADAR] Failed to open %s for writing\n", filePath);
        http.end();
        return false;
    }

    int len = http.getSize();
    WiFiClient *stream = http.getStreamPtr();
    uint8_t tempBuf[1024];
    size_t totalBytes = 0;
    unsigned long startMs = millis();

    while ((http.connected() || stream->available()) && (millis() - startMs < 10000)) {
        size_t avail = stream->available();
        if (avail > 0) {
            size_t toRead = (avail > sizeof(tempBuf)) ? sizeof(tempBuf) : avail;
            int r = stream->read(tempBuf, toRead);
            if (r > 0) {
                f.write(tempBuf, r);
                totalBytes += r;
                startMs = millis();
            }
        } else {
            delay(5);
        }
        if (len > 0 && totalBytes >= (size_t)len) break;
    }

    f.flush();
    f.close();
    http.end();

    if (totalBytes < 500) {
        Serial.printf("[RADAR] Download failed or too small: %u bytes\n", (unsigned int)totalBytes);
        LittleFS.remove(filePath);
        return false;
    }

    Serial.printf("[RADAR] Download complete & saved to %s (%u bytes)\n", filePath, (unsigned int)totalBytes);
    return true;
}

bool RadarFetcher::fetchAllFrames(LovyanGFX &gfx, const AppConfig &cfg, String &statusMsg) {
    initFS();

    // 1. Query telemetry
    memset(&s_latestTelemetry, 0, sizeof(s_latestTelemetry));
    queryTelemetry(cfg, s_latestTelemetry);

    // 2. Query metadata
    String hostUrl;
    memset(s_frameMeta, 0, sizeof(s_frameMeta));
    uint8_t targetCount = cfg.anim_frames;
    if (targetCount == 0) targetCount = 1;
    uint8_t metaCount = 0;

    if (!queryMetadataMulti(cfg, hostUrl, s_frameMeta, targetCount, metaCount)) {
        statusMsg = "API Query Failed";
        return false;
    }

    // 3. Download each frame directly to LittleFS flash
    uint8_t downloaded = 0;
    for (uint8_t i = 0; i < metaCount; i++) {
        char tileUrl[384];
        bool isActualSat = (s_frameMeta[i].is_satellite && !s_frameMeta[i].is_fallback);
        uint8_t colorId = isActualSat ? 0 : cfg.color_scheme;
        uint8_t snowVal = isActualSat ? 0 : (cfg.snow ? 1 : 0);

        snprintf(tileUrl, sizeof(tileUrl), "%s%s/512/%u/%.4f/%.4f/%u/%u_%u.png",
            hostUrl.c_str(),
            s_frameMeta[i].path,
            cfg.zoom,
            cfg.latitude,
            cfg.longitude,
            colorId,
            cfg.smooth ? 1 : 0,
            snowVal
        );

        char filePath[32];
        snprintf(filePath, sizeof(filePath), "/radar_%u.png", downloaded);

        if (downloadTileToFile(String(tileUrl), filePath)) {
            downloaded++;
        } else {
            Serial.printf("[RADAR] Warning: Frame %u download failed\n", i);
        }
    }

    if (downloaded == 0) {
        statusMsg = "Tile Download Failed";
        return false;
    }

    s_totalFrames = downloaded;
    Serial.printf("[RADAR] Caching complete: %u frames saved to LittleFS (Free Heap: %u bytes)\n", 
        s_totalFrames, (unsigned int)ESP.getFreeHeap());

    // 4. Render latest frame immediately
    renderFrameIndex(gfx, cfg, s_totalFrames - 1);

    statusMsg = "OK";
    return true;
}

bool RadarFetcher::renderFrameIndex(LovyanGFX &gfx, const AppConfig &cfg, uint8_t frameIdx) {
    if (s_totalFrames == 0 || frameIdx >= s_totalFrames) return false;

    char filePath[32];
    snprintf(filePath, sizeof(filePath), "/radar_%u.png", frameIdx);

    File f = LittleFS.open(filePath, "r");
    if (!f) {
        Serial.printf("[RADAR] Frame file %s not found\n", filePath);
        return false;
    }

    gfx.startWrite();
    gfx.fillScreen(gfx.color565(8, 14, 24));

    // Decode coordinate-centered 512x512 PNG at (-76, -76) directly from file stream
    bool decoded = gfx.drawPng(&f, -76, -76);
    f.close();

    if (!decoded) {
        Serial.printf("[RADAR] Warning: PNG decode error on frame %u\n", frameIdx);
    }

    // Overlay tactical HUD, telemetry, and frame progress indicators
    drawTacticalOverlay(gfx, cfg, s_frameMeta[frameIdx], s_latestTelemetry, frameIdx, s_totalFrames);

    gfx.endWrite();
    return decoded;
}

static const char* getCardinalDirection(int deg) {
    static const char* cardinals[] = {
        "N", "NNE", "NE", "ENE",
        "E", "ESE", "SE", "SSE",
        "S", "SSW", "SW", "WSW",
        "W", "WNW", "NW", "NNW"
    };
    int idx = (int)((deg + 11.25f) / 22.5f) % 16;
    return cardinals[idx];
}

void RadarFetcher::drawWindVector(LovyanGFX &gfx, int cx, int cy, int radius, int angleDeg, float speedKmh, bool isFahrenheit) {
    int flowDeg = (angleDeg + 180) % 360;
    float flowRad = (float)(flowDeg - 90) * 0.0174533f;

    float podRad = (float)(angleDeg - 90) * 0.0174533f;
    int px = cx + (int)(cos(podRad) * radius);
    int py = cy + (int)(sin(podRad) * radius);

    if (py < 95) py = 95;
    if (py > 245) py = 245;

    gfx.fillCircle(px, py, 16, gfx.color565(8, 16, 26));
    gfx.drawCircle(px, py, 16, gfx.color565(0, 220, 255));

    uint16_t arrowColor = gfx.color565(0, 255, 180);
    int tipX = px + (int)(cos(flowRad) * 11);
    int tipY = py + (int)(sin(flowRad) * 11);
    int tailX = px - (int)(cos(flowRad) * 9);
    int tailY = py - (int)(sin(flowRad) * 9);

    float backAngle = flowRad + 3.14159f;
    int w1x = tipX + (int)(cos(backAngle + 0.55f) * 8);
    int w1y = tipY + (int)(sin(backAngle + 0.55f) * 8);
    int w2x = tipX + (int)(cos(backAngle - 0.55f) * 8);
    int w2y = tipY + (int)(sin(backAngle - 0.55f) * 8);

    gfx.drawLine(tailX, tailY, tipX, tipY, arrowColor);
    gfx.fillTriangle(tipX, tipY, w1x, w1y, w2x, w2y, arrowColor);
    gfx.fillCircle(px, py, 2, gfx.color565(0, 220, 255));
}

void RadarFetcher::drawTacticalOverlay(LovyanGFX &gfx, const AppConfig &cfg, const RadarFrameInfo &frameInfo, const WeatherTelemetry &telemetry, uint8_t frameIdx, uint8_t totalFrames) {
    const int cx = 180;
    const int cy = 180;

    // 1. Tactical Radar Range Rings
    if (cfg.show_range_rings) {
        uint16_t ringColor = gfx.color565(20, 60, 80);
        uint16_t gridColor = gfx.color565(15, 45, 60);

        gfx.drawCircle(cx, cy, 55, ringColor);
        gfx.drawCircle(cx, cy, 110, ringColor);
        gfx.drawCircle(cx, cy, 160, ringColor);

        gfx.drawFastHLine(cx - 165, cy, 140, gridColor);
        gfx.drawFastHLine(cx + 25, cy, 140, gridColor);
        gfx.drawFastVLine(cx, cy - 165, 140, gridColor);
        gfx.drawFastVLine(cx, cy + 25, 140, gridColor);

        gfx.setTextColor(gfx.color565(64, 210, 255));
        gfx.setFont(&fonts::Font2);
        gfx.setTextDatum(textdatum_t::middle_center);
        gfx.drawString("N", cx, 18);
        gfx.drawString("S", cx, 342);
        gfx.drawString("W", 18, cy);
        gfx.drawString("E", 342, cy);

        const char* distLabel = "50km";
        if (cfg.zoom <= 4) distLabel = "200km";
        else if (cfg.zoom == 5) distLabel = "100km";
        else if (cfg.zoom == 6) distLabel = "50km";
        else if (cfg.zoom >= 7) distLabel = "25km";

        gfx.fillRoundRect(cx + 96, cy - 18, 38, 14, 3, gfx.color565(8, 16, 26));
        gfx.drawRoundRect(cx + 96, cy - 18, 38, 14, 3, gfx.color565(20, 50, 70));
        gfx.setFont(&fonts::Font0);
        gfx.setTextColor(gfx.color565(80, 180, 220));
        gfx.drawString(distLabel, cx + 115, cy - 11);
    }

    // 2. Wind Vector Telemetry Overlay
    if (telemetry.valid && cfg.show_telemetry && telemetry.wind_speed > 0.5f) {
        drawWindVector(gfx, cx, cy, 135, telemetry.wind_direction, telemetry.wind_speed, cfg.temp_units == 1);
    }

    // 3. Center Reticle
    uint16_t reticleColor = gfx.color565(0, 255, 200);
    gfx.drawCircle(cx, cy, 5, reticleColor);
    gfx.fillCircle(cx, cy, 2, reticleColor);

    // 4. Top Header Badge: Location Name & Product Type
    gfx.setFont(&fonts::Font2);
    gfx.setTextDatum(textdatum_t::middle_center);

    char topBadge[64];
    uint16_t badgeBg = gfx.color565(10, 20, 32);
    uint16_t badgeBorder = gfx.color565(0, 200, 255);

    if (frameInfo.is_satellite) {
        if (frameInfo.is_fallback) {
            badgeBg = gfx.color565(26, 20, 10);
            badgeBorder = gfx.color565(255, 180, 0);
            snprintf(topBadge, sizeof(topBadge), "%.10s [SAT N/A]", cfg.location_name);
        } else {
            badgeBg = gfx.color565(24, 12, 34);
            badgeBorder = gfx.color565(200, 100, 255);
            snprintf(topBadge, sizeof(topBadge), "%.10s [Z%u SAT]", cfg.location_name, cfg.zoom);
        }
    } else {
        snprintf(topBadge, sizeof(topBadge), "%.10s [Z%u RADAR]", cfg.location_name, cfg.zoom);
    }

    gfx.fillRoundRect(cx - 110, 32, 220, 26, 13, badgeBg);
    gfx.drawRoundRect(cx - 110, 32, 220, 26, 13, badgeBorder);
    gfx.setTextColor(gfx.color565(255, 255, 255));
    gfx.drawString(topBadge, cx, 45);

    // 5. Ambient Weather Telemetry Pill
    if (telemetry.valid && cfg.show_telemetry) {
        char envBadge[64];
        float tempVal = telemetry.temperature;
        char tempUnit = 'C';
        float speedVal = telemetry.wind_speed;
        const char* speedUnit = "km/h";
        if (cfg.temp_units == 1) {
            tempVal = (tempVal * 1.8f) + 32.0f;
            tempUnit = 'F';
            speedVal = speedVal * 0.621371f;
            speedUnit = "mph";
        }
        
        const char* cardDir = getCardinalDirection(telemetry.wind_direction);
        if (telemetry.wind_speed >= 0.5f) {
            snprintf(envBadge, sizeof(envBadge), "%.1f %c | %d%% | %s %.0f%s", 
                tempVal, tempUnit, telemetry.humidity, cardDir, speedVal, speedUnit);
        } else {
            snprintf(envBadge, sizeof(envBadge), "%.1f %c | %d%% | CALM", 
                tempVal, tempUnit, telemetry.humidity);
        }

        gfx.setFont(&fonts::Font2);
        gfx.setTextDatum(textdatum_t::middle_center);
        gfx.fillRoundRect(cx - 120, 62, 240, 24, 12, gfx.color565(12, 24, 38));
        gfx.drawRoundRect(cx - 120, 62, 240, 24, 12, gfx.color565(25, 75, 105));
        gfx.setTextColor(gfx.color565(0, 240, 255));
        gfx.drawString(envBadge, cx, 74);
    }

    // 6. Color Legend
    drawRadarLegend(gfx, (frameInfo.is_satellite && !frameInfo.is_fallback) ? 1 : 0);

    // 7. Bottom Footer Badge
    if (cfg.show_clock) {
        gfx.setFont(&fonts::Font2);
        gfx.setTextDatum(textdatum_t::middle_center);

        char bottomBadge[64];
        const char* prodLabel = frameInfo.is_satellite ? "SAT" : "RADAR";

        if (totalFrames > 1) {
            if (frameIdx == totalFrames - 1) {
                snprintf(bottomBadge, sizeof(bottomBadge), "%s: %s [LIVE]", prodLabel, frameInfo.formatted_time);
            } else {
                snprintf(bottomBadge, sizeof(bottomBadge), "%s: %s [%u/%u]", 
                    prodLabel, frameInfo.formatted_time, frameIdx + 1, totalFrames);
            }
        } else {
            snprintf(bottomBadge, sizeof(bottomBadge), "%s: %s", prodLabel, frameInfo.formatted_time);
        }

        gfx.fillRoundRect(cx - 95, 304, 190, 24, 12, gfx.color565(10, 20, 32));
        gfx.drawRoundRect(cx - 95, 304, 190, 24, 12, gfx.color565(30, 70, 95));

        if (totalFrames > 1 && frameIdx == totalFrames - 1) {
            gfx.setTextColor(gfx.color565(0, 255, 200));
        } else {
            gfx.setTextColor(gfx.color565(0, 220, 255));
        }
        gfx.drawString(bottomBadge, cx, 316);
    }
}

void RadarFetcher::drawRadarLegend(LovyanGFX &gfx, uint8_t product) {
    const int cx = 180;
    const int cardW = 210;
    const int cardH = 26;
    const int cardY = 266;

    gfx.fillRoundRect(cx - (cardW / 2), cardY, cardW, cardH, 6, gfx.color565(10, 18, 30));
    gfx.drawRoundRect(cx - (cardW / 2), cardY, cardW, cardH, 6, gfx.color565(25, 60, 85));

    const int barW = 190;
    const int barH = 5;
    const int barX = cx - (barW / 2);
    const int barY = cardY + 16;

    if (product == 1) {
        const uint16_t satColors[] = {
            gfx.color565(25, 35, 45),
            gfx.color565(60, 80, 100),
            gfx.color565(110, 140, 170),
            gfx.color565(160, 190, 220),
            gfx.color565(210, 235, 250),
            gfx.color565(255, 255, 255)
        };
        int numColors = sizeof(satColors) / sizeof(satColors[0]);
        int stepW = barW / numColors;

        for (int i = 0; i < numColors; i++) {
            gfx.fillRect(barX + i * stepW, barY, stepW, barH, satColors[i]);
        }
        gfx.setFont(&fonts::Font0);
        gfx.setTextColor(gfx.color565(160, 200, 230));
        gfx.setTextDatum(textdatum_t::middle_left);
        gfx.drawString("CLEAR", barX, cardY + 8);
        gfx.setTextDatum(textdatum_t::middle_right);
        gfx.drawString("DENSE CLOUDS", barX + barW, cardY + 8);
    } else {
        const uint16_t colors[] = {
            gfx.color565(0, 120, 255),
            gfx.color565(0, 230, 120),
            gfx.color565(120, 255, 0),
            gfx.color565(255, 220, 0),
            gfx.color565(255, 90, 0),
            gfx.color565(255, 0, 70),
            gfx.color565(200, 0, 255)
        };
        int numColors = sizeof(colors) / sizeof(colors[0]);
        int stepW = barW / numColors;

        for (int i = 0; i < numColors; i++) {
            gfx.fillRect(barX + i * stepW, barY, stepW, barH, colors[i]);
        }

        gfx.setFont(&fonts::Font0);
        gfx.setTextColor(gfx.color565(160, 200, 230));
        gfx.setTextDatum(textdatum_t::middle_left);
        gfx.drawString("LIGHT RAIN", barX, cardY + 8);
        gfx.setTextDatum(textdatum_t::middle_right);
        gfx.drawString("HEAVY dBZ", barX + barW, cardY + 8);
    }
}

