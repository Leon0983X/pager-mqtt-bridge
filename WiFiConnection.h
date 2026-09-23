#ifndef WIFICONNECTION_H
#define WIFICONNECTION_H

#include <WiFiNINA.h>

extern WiFiClient wifiClient;

/* Register the watchdog feed callback and connect timeout. Does not connect. */
void wifiSetup();

/* One connection attempt, bounded by WIFI_CONNECT_TIMEOUT_MS (watchdog is fed). */
bool wifiTryConnect();

bool wifiIsConnected();

/* Reset the NINA module (WiFi.end()); the next wifiTryConnect() starts fresh. */
void wifiResetModule();

#endif /* WIFICONNECTION_H */
