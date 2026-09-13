#pragma once
#include <Arduino.h>
#include <WebServer.h>
#include <DNSServer.h>
#include "config_manager.h"

class Portal {
public:
    static void startAP(AppConfig &currentConfig);
    static void startLocalServer(AppConfig &currentConfig);
    static void handle();
    static bool isAPMode();
    static bool isRunning();
    static bool isLiveRefreshRequested();

private:
    static bool pendingLiveRefresh;
    static void setupRoutes();
    static void handleRoot();
    static void handleSave();
    static void handleSwitchMode();
    static void handleSwitchLayer();
    static void handleScan();
    static void handleRestart();
    static void handleNotFound();
};

