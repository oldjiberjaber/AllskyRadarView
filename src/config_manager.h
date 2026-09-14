#pragma once
#include <Arduino.h>
#include <Preferences.h>

enum DisplayMode : uint8_t {
    MODE_DUAL_DISPLAY = 0,    // Screen 1 (CS1): Allsky, Screen 2 (CS2): Radar
    MODE_SINGLE_RADAR = 1,    // Single Screen (CS1): Radar Only
    MODE_SINGLE_ALLSKY = 2,   // Single Screen (CS1): Allsky Only
    MODE_SINGLE_CAROUSEL = 3  // Single Screen (CS1): Alternating Allsky & Radar
};

struct AppConfig {
    // 1. Wi-Fi Configuration
    char wifi_ssid[64];
    char wifi_pass[64];

    // 2. Display & Operating Mode
    uint8_t display_mode;          // DisplayMode enum
    uint16_t carousel_interval_sec; // Seconds per screen in carousel mode (10 - 120, default: 30)

    // 3. Allsky Camera MQTT Configuration
    char mqtt_host[64];
    uint16_t mqtt_port;
    char mqtt_topic[64];
    char mqtt_user[32];
    char mqtt_pass[32];

    // 4. Geolocation & Radar Data Source
    float latitude;
    float longitude;
    char location_name[48];
    uint8_t data_product;          // 0: RainViewer Precipitation Radar, 1: OpenWeatherMap Cloud Cover
    char owm_api_key[40];          // OpenWeatherMap API Key
    uint8_t zoom;                  // 4 to 7 (Default: 6)
    uint8_t color_scheme;          // 0 to 8 (Default: 2 - Universal Blue)
    uint8_t smooth;                // 1: smoothed, 0: raw pixels
    uint8_t snow;                  // 1: separate snow colors, 0: standard rain
    uint16_t refresh_interval_min; // 1 to 60 minutes (Default: 10)

    // 5. Radar Motion Loop & History
    uint8_t anim_frames;           // 0: Disabled, 3, 5 (Default), 8 frames
    uint16_t anim_speed_ms;        // Frame step duration in ms (default: 700)
    uint16_t anim_dwell_ms;        // Dwell pause on live frame in ms (default: 3500)

    // 6. UI & Weather Telemetry Overlays
    uint8_t show_range_rings;      // 1: ON, 0: OFF
    uint8_t show_clock;            // 1: ON, 0: OFF
    uint8_t show_telemetry;        // 1: ON (Open-Meteo), 0: OFF
    uint8_t temp_units;            // 0: Celsius (°C) & km/h, 1: Fahrenheit (°F) & mph

    // 7. System & Hardware
    char timezone[64];
    uint8_t brightness;            // 0 - 255
};

class ConfigManager {
public:
    static void load(AppConfig &cfg) {
        Preferences prefs;
        prefs.begin("allskyradar", true); // Read-only mode

        // Wi-Fi
        String ssid = prefs.getString("ssid", "Jiberjaber_iot");
        String pass = prefs.getString("pass", "123apples456");
        strncpy(cfg.wifi_ssid, ssid.c_str(), sizeof(cfg.wifi_ssid) - 1);
        strncpy(cfg.wifi_pass, pass.c_str(), sizeof(cfg.wifi_pass) - 1);
        cfg.wifi_ssid[sizeof(cfg.wifi_ssid) - 1] = '\0';
        cfg.wifi_pass[sizeof(cfg.wifi_pass) - 1] = '\0';

        // Display & Operating Mode
        cfg.display_mode = prefs.getUChar("mode", MODE_DUAL_DISPLAY);
        if (cfg.display_mode > MODE_SINGLE_CAROUSEL) cfg.display_mode = MODE_DUAL_DISPLAY;
        cfg.carousel_interval_sec = prefs.getUShort("car_int", 30);
        if (cfg.carousel_interval_sec < 5 || cfg.carousel_interval_sec > 300) cfg.carousel_interval_sec = 30;

        // Allsky MQTT Settings
        String mHost = prefs.getString("mq_host", "192.168.0.6");
        strncpy(cfg.mqtt_host, mHost.c_str(), sizeof(cfg.mqtt_host) - 1);
        cfg.mqtt_host[sizeof(cfg.mqtt_host) - 1] = '\0';
        cfg.mqtt_port = prefs.getUShort("mq_port", 1883);

        String mTopic = prefs.getString("mq_topic", "indi-allsky/thumbnail");
        strncpy(cfg.mqtt_topic, mTopic.c_str(), sizeof(cfg.mqtt_topic) - 1);
        cfg.mqtt_topic[sizeof(cfg.mqtt_topic) - 1] = '\0';

        String mUser = prefs.getString("mq_user", "");
        strncpy(cfg.mqtt_user, mUser.c_str(), sizeof(cfg.mqtt_user) - 1);
        cfg.mqtt_user[sizeof(cfg.mqtt_user) - 1] = '\0';

        String mPass = prefs.getString("mq_pass", "");
        strncpy(cfg.mqtt_pass, mPass.c_str(), sizeof(cfg.mqtt_pass) - 1);
        cfg.mqtt_pass[sizeof(cfg.mqtt_pass) - 1] = '\0';

        // Geolocation & Radar
        cfg.latitude = prefs.getFloat("lat", 51.5074f);
        cfg.longitude = prefs.getFloat("lon", -0.1278f);
        String loc = prefs.getString("loc", "London, UK");
        strncpy(cfg.location_name, loc.c_str(), sizeof(cfg.location_name) - 1);
        cfg.location_name[sizeof(cfg.location_name) - 1] = '\0';

        cfg.data_product = prefs.getUChar("product", 0);
        if (cfg.data_product > 1) cfg.data_product = 0;

        String owmKey = prefs.getString("owm_key", "");
        strncpy(cfg.owm_api_key, owmKey.c_str(), sizeof(cfg.owm_api_key) - 1);
        cfg.owm_api_key[sizeof(cfg.owm_api_key) - 1] = '\0';

        cfg.zoom = prefs.getUChar("zoom", 6);
        if (cfg.zoom < 1 || cfg.zoom > 7) cfg.zoom = 6;

        cfg.color_scheme = prefs.getUChar("color", 2);
        if (cfg.color_scheme > 8) cfg.color_scheme = 2;

        cfg.smooth = prefs.getUChar("smooth", 1);
        cfg.snow = prefs.getUChar("snow", 1);
        cfg.refresh_interval_min = prefs.getUShort("refresh", 10);
        if (cfg.refresh_interval_min < 1 || cfg.refresh_interval_min > 60) cfg.refresh_interval_min = 10;

        // Motion Loop & History
        cfg.anim_frames = prefs.getUChar("aframes", 5);
        if (cfg.anim_frames > 8) cfg.anim_frames = 5;

        cfg.anim_speed_ms = prefs.getUShort("aspeed", 700);
        if (cfg.anim_speed_ms < 200 || cfg.anim_speed_ms > 3000) cfg.anim_speed_ms = 700;

        cfg.anim_dwell_ms = prefs.getUShort("adwell", 3500);
        if (cfg.anim_dwell_ms < 1000 || cfg.anim_dwell_ms > 10000) cfg.anim_dwell_ms = 3500;

        // UI & Telemetry
        cfg.show_range_rings = prefs.getUChar("rings", 1);
        cfg.show_clock = prefs.getUChar("clock", 1);
        cfg.show_telemetry = prefs.getUChar("telem", 1);
        cfg.temp_units = prefs.getUChar("units", 0);

        // System
        String tz = prefs.getString("tz", "GMT0BST,M3.5.0/1,M10.5.0");
        strncpy(cfg.timezone, tz.c_str(), sizeof(cfg.timezone) - 1);
        cfg.timezone[sizeof(cfg.timezone) - 1] = '\0';

        cfg.brightness = prefs.getUChar("brightness", 220);

        prefs.end();
    }

    static void save(const AppConfig &cfg) {
        Preferences prefs;
        prefs.begin("allskyradar", false); // Read-write mode

        prefs.putString("ssid", cfg.wifi_ssid);
        prefs.putString("pass", cfg.wifi_pass);
        prefs.putUChar("mode", cfg.display_mode);
        prefs.putUShort("car_int", cfg.carousel_interval_sec);

        prefs.putString("mq_host", cfg.mqtt_host);
        prefs.putUShort("mq_port", cfg.mqtt_port);
        prefs.putString("mq_topic", cfg.mqtt_topic);
        prefs.putString("mq_user", cfg.mqtt_user);
        prefs.putString("mq_pass", cfg.mqtt_pass);

        prefs.putFloat("lat", cfg.latitude);
        prefs.putFloat("lon", cfg.longitude);
        prefs.putString("loc", cfg.location_name);
        prefs.putUChar("product", cfg.data_product);
        prefs.putString("owm_key", cfg.owm_api_key);
        prefs.putUChar("zoom", cfg.zoom);
        prefs.putUChar("color", cfg.color_scheme);
        prefs.putUChar("smooth", cfg.smooth);
        prefs.putUChar("snow", cfg.snow);
        prefs.putUShort("refresh", cfg.refresh_interval_min);

        prefs.putUChar("aframes", cfg.anim_frames);
        prefs.putUShort("aspeed", cfg.anim_speed_ms);
        prefs.putUShort("adwell", cfg.anim_dwell_ms);

        prefs.putUChar("rings", cfg.show_range_rings);
        prefs.putUChar("clock", cfg.show_clock);
        prefs.putUChar("telem", cfg.show_telemetry);
        prefs.putUChar("units", cfg.temp_units);

        prefs.putString("tz", cfg.timezone);
        prefs.putUChar("brightness", cfg.brightness);

        prefs.end();
        Serial.println("[CONFIG] Saved AllskyRadarView settings to NVS.");
    }

    static bool hasValidWiFi(const AppConfig &cfg) {
        return (strlen(cfg.wifi_ssid) > 0);
    }
};

