#ifndef USERCONFIG_H
#define USERCONFIG_H

#include <stdint.h>

/**********************/
/* User Configuration */
/**********************/

/* Wi-Fi and MQTT credentials, populated from arduino_secrets.h in the .cpp. */
extern const char ssid[];
extern const char pass[];
extern const char mqtt_id[];
extern const char mqtt_user[];
extern const char mqtt_pass[];
extern const char broker[];
extern const int  port;                                // 1883

/* MQTT topics (R5). TOPIC_ALARM is unchanged so the existing HA automation keeps working. */
extern const char TOPIC_ALARM[];                        // "homeassistant/dme_alarm"
extern const char TOPIC_STATE[];                        // "homeassistant/dme_alarm/state"
extern const char TOPIC_AVAILABILITY[];                 // "homeassistant/dme_alarm/availability"
extern const char TOPIC_DURATION[];                     // "homeassistant/dme_alarm/duration"
extern const char TOPIC_BOOT[];                         // "homeassistant/dme_alarm/boot"

/* Alarm input (R1, R2). */
extern const int  SWITCH_PIN;                           // 2 (D2)
extern const bool SWITCH_ACTIVE_HIGH;                   // false (recommended): relay to GND, INPUT_PULLUP, LOW = closed
                                                          // true: relay to 3.3V, INPUT_PULLDOWN, HIGH = closed
extern const unsigned long DEBOUNCE_MS;                 // 50

/* Alarm delivery (R6). */
extern const unsigned long ALARM_MAX_AGE_MS;             // max age of a pending alarm before it is dropped

/* MQTT connection tuning (R7, R12). */
extern const unsigned long MQTT_KEEP_ALIVE_MS;
extern const unsigned long MQTT_CONNECTION_TIMEOUT_MS;   // CONNACK and PUBACK wait
extern const uint16_t      TCP_CONNECT_TIMEOUT_MS;

/* Wi-Fi / reconnect tuning (R10, R11). */
extern const unsigned long WIFI_CONNECT_TIMEOUT_MS;      // watchdog is fed inside WiFi.begin
extern const unsigned long RECONNECT_BACKOFF_MIN_MS;
extern const unsigned long RECONNECT_BACKOFF_MAX_MS;
extern const int           WIFI_FAILURES_BEFORE_RESET;
extern const unsigned long MAX_OFFLINE_MS;

/* Hardware watchdog (R12). */
extern const unsigned long WATCHDOG_TIMEOUT_MS;          // clamped to the hardware max at runtime

/*****************************/
/* End of User Configuration */
/*****************************/

#endif /* USERCONFIG_H */
