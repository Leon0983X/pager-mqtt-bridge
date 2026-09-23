#include <Arduino.h>

#include "ConnectionManager.h"
#include "WiFiConnection.h"
#include "MqttConnection.h"
#include "SystemHealth.h"
#include "UserConfig.h"

/* Non-blocking Wi-Fi + MQTT supervision with backoff and escalation (R10, R11). */

static unsigned long backoffMs;
static unsigned long nextAttemptAt;
static unsigned long offlineSince;
static int wifiFailures;
static bool wasOnline;

/* Wrap-safe "has `now` reached `target`" (millis() rolls over ~every 49 days). */
static bool reached(unsigned long now, unsigned long target) {
  return (long)(now - target) >= 0;
}

void connectionBegin() {
  wifiSetup();
  mqttSetup();

  backoffMs = RECONNECT_BACKOFF_MIN_MS;
  nextAttemptAt = millis();  // try immediately on the first pass
  offlineSince = millis();
  wifiFailures = 0;
  wasOnline = false;
}

bool connectionUpdate(bool rebootAllowed) {
  unsigned long now = millis();
  bool online = wifiIsConnected() && mqttIsConnected();

  /* 1. Already online: keep the MQTT session alive and do nothing else this
   *    pass. (A transition *into* the online state is reported below, via
   *    the `return true` on a successful mqttTryConnect().) */
  if (online) {
    mqttClient.poll();
    return false;
  }

  /* 2. Just dropped offline: log which link went down, stop MQTT so its
   *    internal state matches reality, and allow an immediate first retry. */
  if (wasOnline) {
    if (!wifiIsConnected()) {
      Serial.println(F("Connection: WiFi link dropped"));
    } else {
      Serial.println(F("Connection: MQTT link dropped"));
    }
    wasOnline = false;
    offlineSince = now;
    nextAttemptAt = now;
    mqttClient.stop();
  }

  /* 3. Escalation ceiling (R11.3/4): reboot if we've been offline too long,
   *    unless a fresh alarm is still waiting to be sent (rebootAllowed is
   *    false while publisherHasFreshPendingAlarm() is true, per the .ino). */
  if (rebootAllowed && (now - offlineSince > MAX_OFFLINE_MS)) {
    Serial.print(F("Connection: offline for over "));
    Serial.print(MAX_OFFLINE_MS);
    Serial.println(F(" ms, rebooting"));
    connectionRebootDevice();  // does not return
  }

  /* 4. One blocking attempt per pass at most (Wi-Fi first, then MQTT), so
   *    alarm processing in the caller's loop() always runs between them. */
  if (reached(now, nextAttemptAt)) {
    if (!wifiIsConnected()) {
      if (wifiTryConnect()) {
        wifiFailures = 0;
        /* Wi-Fi is up but MQTT isn't yet: try MQTT on the very next pass
         * rather than waiting out the backoff. */
        nextAttemptAt = now;
      } else {
        wifiFailures++;
        Serial.print(F("Connection: WiFi failure count = "));
        Serial.println(wifiFailures);

        if (wifiFailures >= WIFI_FAILURES_BEFORE_RESET) {
          Serial.println(F("Connection: too many WiFi failures, resetting NINA module"));
          wifiResetModule();
          wifiFailures = 0;
        }

        nextAttemptAt = now + backoffMs;
        Serial.print(F("Connection: next WiFi attempt in "));
        Serial.print(backoffMs);
        Serial.println(F(" ms"));
        backoffMs = min(backoffMs * 2, RECONNECT_BACKOFF_MAX_MS);
      }
    } else {
      if (mqttTryConnect()) {
        backoffMs = RECONNECT_BACKOFF_MIN_MS;
        wifiFailures = 0;
        wasOnline = true;
        return true;  // just (re)connected this pass
      } else {
        nextAttemptAt = now + backoffMs;
        Serial.print(F("Connection: next MQTT attempt in "));
        Serial.print(backoffMs);
        Serial.println(F(" ms"));
        backoffMs = min(backoffMs * 2, RECONNECT_BACKOFF_MAX_MS);
      }
    }
  }

  return false;
}

bool connectionIsOnline() {
  return wifiIsConnected() && mqttIsConnected();
}

void connectionRebootDevice() {
  /* Guard every MQTT touch behind wifiIsConnected() first (same
   * short-circuit pattern as connectionIsOnline() and the rest of this
   * file): if Wi-Fi is down -- including while the NINA module is
   * deliberately held in reset by wifiResetModule() -- mqttClient.stop()
   * must not run. WiFiClient::stop() only no-ops instantly when its socket
   * is already 255; if a previous connect attempt left a stale non-255
   * socket number set, it instead calls ServerDrv::stopClient() over SPI and
   * then polls status() in a loop for up to 5 s (WiFiClient.cpp), which
   * would either hang talking to a module that is not there to answer, or
   * -- worse -- silently re-initialize it via the same lazy
   * SpiDrv::begin() path documented in WiFiConnection.cpp's
   * wifiIsConnected(), undoing an in-progress reset hold. */
  if (wifiIsConnected() && mqttIsConnected()) {
    Serial.println(F("Connection: publishing offline before reboot"));
    mqttPublish(TOPIC_AVAILABILITY, "offline", true);
  }
  if (wifiIsConnected()) {
    mqttClient.stop();
  }
  delay(100);  // let the offline frame flush; short enough for the watchdog
  healthReboot();  // does not return
}
