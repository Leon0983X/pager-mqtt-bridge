#include "UserConfig.h"
#include "arduino_secrets.h"

/**********************/
/* User Configuration */
/**********************/

/* WIFI CONFIGURATION */
const char ssid[] = SECRET_SSID;
const char pass[] = SECRET_PASS;

/* MQTT CONFIGURATION */
const char mqtt_id[]   = SECRET_MQTT_ID;
const char mqtt_user[] = SECRET_MQTT_USER;
const char mqtt_pass[] = SECRET_MQTT_PASS;

const char broker[] = SECRET_BROKER;
const int  port     = 1883;

/* MQTT topics (R5) */
const char TOPIC_ALARM[]        = "homeassistant/dme_alarm";
const char TOPIC_STATE[]        = "homeassistant/dme_alarm/state";
const char TOPIC_AVAILABILITY[] = "homeassistant/dme_alarm/availability";
const char TOPIC_DURATION[]     = "homeassistant/dme_alarm/duration";
const char TOPIC_BOOT[]         = "homeassistant/dme_alarm/boot";

/* Alarm input (R1, R2) */
const int  SWITCH_PIN = 2;  // D2
const bool SWITCH_ACTIVE_HIGH = false;  // relay wired between D2 and GND (INPUT_PULLUP, LOW = closed)
const unsigned long DEBOUNCE_MS = 50;

/* Alarm delivery (R6) */
const unsigned long ALARM_MAX_AGE_MS = 10UL * 60UL * 1000UL;  // 10 min

/* MQTT connection tuning (R7, R12) */
const unsigned long MQTT_KEEP_ALIVE_MS = 20000;
const unsigned long MQTT_CONNECTION_TIMEOUT_MS = 2500;
const uint16_t      TCP_CONNECT_TIMEOUT_MS = 2500;

/* Wi-Fi / reconnect tuning (R10, R11) */
const unsigned long WIFI_CONNECT_TIMEOUT_MS = 10000;
const unsigned long RECONNECT_BACKOFF_MIN_MS = 1000;
const unsigned long RECONNECT_BACKOFF_MAX_MS = 60000;
const int           WIFI_FAILURES_BEFORE_RESET = 5;
const unsigned long MAX_OFFLINE_MS = 5UL * 60UL * 1000UL;  // 5 min

/* Hardware watchdog (R12) */
const unsigned long WATCHDOG_TIMEOUT_MS = 8000;

/*****************************/
/* End of User Configuration */
/*****************************/
