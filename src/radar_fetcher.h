#pragma once
#include <Arduino.h>
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <LittleFS.h>
#include "config_manager.h"

#define MAX_RADAR_FRAMES 8

struct RadarFrameInfo {
    uint32_t timestamp;
    char path[128];
    char formatted_time[32];
    bool valid;
    bool is_satellite;
    bool is_fallback;
    int owm_start_x;
    int owm_start_y;
};

struct WeatherTelemetry {
    float temperature;     // °C
    int humidity;          // %
    float wind_speed;      // km/h
    int wind_direction;    // degrees (0 = North, 90 = East, 180 = South, 270 = West)
    bool valid;
};

class RadarFetcher {
public:
    static bool initFS();
    static bool fetchAllFrames(LovyanGFX &gfx, const AppConfig &cfg, String &statusMsg);
    static bool renderFrameIndex(LovyanGFX &gfx, const AppConfig &cfg, uint8_t frameIdx);
    static void drawTacticalOverlay(LovyanGFX &gfx, const AppConfig &cfg, const RadarFrameInfo &frameInfo, const WeatherTelemetry &telemetry, uint8_t frameIdx, uint8_t totalFrames);
    static void drawWindVector(LovyanGFX &gfx, int cx, int cy, int radius, int angleDeg, float speedKmh, bool isFahrenheit);
    static void drawRadarLegend(LovyanGFX &gfx, uint8_t product);
    static uint8_t getFrameCount();
    static const WeatherTelemetry& getLatestTelemetry();

private:
    static bool queryMetadataMulti(const AppConfig &cfg, String &hostUrl, RadarFrameInfo *outFrames, uint8_t maxFrames, uint8_t &outCount);
    static bool queryTelemetry(const AppConfig &cfg, WeatherTelemetry &telemetry);
    static bool downloadTileToFile(const String &url, const char *filePath);
    static bool fetchOpenWeatherClouds(const AppConfig &cfg, RadarFrameInfo &outFrame);
};

