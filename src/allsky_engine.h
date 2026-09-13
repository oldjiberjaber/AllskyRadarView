#pragma once
#include <Arduino.h>
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include "config_manager.h"

struct AllskyTelemetry {
    char timestamp[32];
    char exposure[24];
    char gain[16];
    float temp;
    bool hasTelemetry;
};

class AllskyEngine {
public:
    static void init(const AppConfig &cfg);
    static void stop();
    static bool render(LovyanGFX &gfx, const AppConfig &cfg);
    static bool hasNewImage();
    static void clearNewImageFlag();
    static uint32_t getImageCount();
    static const AllskyTelemetry& getTelemetry();
    static bool isConnected();

private:
    static bool parseJpgDimensions(const uint8_t *data, size_t len, uint16_t *width, uint16_t *height);
    static float calculateDctScale(uint16_t w, uint16_t h, uint16_t targetDim);
};

