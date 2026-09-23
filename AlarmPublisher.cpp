#include <Arduino.h>
#include <stdint.h>
#include <stdio.h>

#include "AlarmPublisher.h"
#include "AlarmInput.h"
#include "MqttConnection.h"
#include "SystemHealth.h"
#include "UserConfig.h"

/*
 * Turns AlarmInput events into MQTT messages (R5, R6, R4b).
 *
 * - A single pending-alarm slot holds the most recent undelivered CLOSED
 *   edge (R6). It is cleared only once mqttPublish() for TOPIC_ALARM
 *   returns true, and is dropped (with a Serial log) once it is older
 *   than ALARM_MAX_AGE_MS, whether connected or not.
 * - A small ring buffer of ON/OFF values lets a short closure processed in
 *   a single pass be reported as both ON and OFF, in order, once connected.
 *   Nothing is queued while offline: the current debounced state
 *   (alarmInputIsClosed()) is republished on every (re)connect instead.
 * - A single pending-duration slot holds the most recent unsent closure
 *   duration; a newer one overwrites an older unsent one (R4b).
 * - The boot reason is sent once per boot, retried on every connect until
 *   it succeeds.
 *
 * Every publish attempt is preceded by healthKick(), and a pass stops
 * publishing as soon as one publish fails (the connection is likely dead)
 * so a single publisherUpdate() call cannot chain many blocking calls
 * indefinitely.
 */

/* ---- Pending ALARM (R6) ------------------------------------------------ */

static bool alarmPending = false;
static unsigned long alarmAt = 0;          /* millis() when the closure started */
static bool alarmAttempted = false;        /* has a publish attempt ever been made? */
static unsigned long lastAlarmAttemptMs = 0;
static const unsigned long ALARM_RETRY_INTERVAL_MS = 1000; /* retry at most 1/s */

/* ---- Pending state changes (ON/OFF), only queued while connected ------- */

static const uint8_t STATE_QUEUE_CAPACITY = 16; /* matches AlarmInput's event FIFO size */
static bool stateQueue[STATE_QUEUE_CAPACITY];
static uint8_t stateQueueHead = 0;
static uint8_t stateQueueTail = 0;
static uint8_t stateQueueCount = 0;

static void pushStateQueue(bool closed) {
  if (stateQueueCount >= STATE_QUEUE_CAPACITY) {
    Serial.println("AlarmPublisher: state queue full, dropping a state update");
    return;
  }
  stateQueue[stateQueueTail] = closed;
  stateQueueTail = (uint8_t)((stateQueueTail + 1) % STATE_QUEUE_CAPACITY);
  stateQueueCount++;
}

static bool peekStateQueue(bool &closed) {
  if (stateQueueCount == 0) {
    return false;
  }
  closed = stateQueue[stateQueueHead];
  return true;
}

static void popStateQueue() {
  if (stateQueueCount == 0) {
    return;
  }
  stateQueueHead = (uint8_t)((stateQueueHead + 1) % STATE_QUEUE_CAPACITY);
  stateQueueCount--;
}

static void clearStateQueue() {
  stateQueueHead = 0;
  stateQueueTail = 0;
  stateQueueCount = 0;
}

/* ---- Pending duration (R4b), single slot, newest unsent wins ---------- */

static bool durationPendingValid = false;
static char durationPendingStr[16]; /* "NNNNNNN.NNN\0" fits comfortably */

static void formatDurationSeconds(unsigned long durationMs, char *buf, size_t bufLen) {
  unsigned long wholeSeconds = durationMs / 1000UL;
  unsigned long millisPart = durationMs % 1000UL;
  snprintf(buf, bufLen, "%lu.%03lu", wholeSeconds, millisPart);
}

/* ---- Boot reason, once per boot ---------------------------------------- */

static bool bootReasonSent = false;

/* ---- Limit how many blocking publishes one publisherUpdate() pass can chain */

static const uint8_t MAX_PUBLISHES_PER_PASS = 8;

/* ------------------------------------------------------------------------ */

void publisherUpdate(bool mqttConnected) {
  /* 1. Drain AlarmInput events. This is cheap, in-memory bookkeeping only
   *    (no publishing here), bounded by AlarmInput's own event FIFO size. */
  AlarmEvent ev;
  while (alarmInputNextEvent(ev)) {
    if (ev.type == ALARM_EVENT_CLOSED) {
      alarmPending = true;
      alarmAt = ev.closedAtMs; /* newest wins */
      if (mqttConnected) {
        pushStateQueue(true);
      }
    } else { /* ALARM_EVENT_OPENED */
      if (mqttConnected) {
        pushStateQueue(false);
      }
      formatDurationSeconds(ev.durationMs, durationPendingStr, sizeof(durationPendingStr));
      durationPendingValid = true;
    }
  }

  /* 2. Drop a stale pending alarm, connected or not (R6). */
  if (alarmPending) {
    unsigned long age = millis() - alarmAt; /* wrap-safe unsigned subtraction */
    if (age > ALARM_MAX_AGE_MS) {
      Serial.print("Dropped stale alarm, age ");
      Serial.print(age / 1000UL);
      Serial.println(" s");
      alarmPending = false;
    }
  }

  if (!mqttConnected) {
    /* Nothing more to do until reconnect; publisherOnConnected() catches up. */
    return;
  }

  /* 3. Publish. ALARM always goes first; everything else waits behind it. */
  uint8_t publishes = 0;

  if (alarmPending) {
    unsigned long now = millis();
    bool dueForRetry = !alarmAttempted || (now - lastAlarmAttemptMs >= ALARM_RETRY_INTERVAL_MS);
    if (dueForRetry) {
      alarmAttempted = true;
      lastAlarmAttemptMs = now;
      healthKick();
      publishes++;
      if (mqttPublish(TOPIC_ALARM, "ALARM", false)) {
        Serial.println("Published ALARM");
        alarmPending = false;
        /* Only a FAILED attempt should rate-limit the next one: reset the
         * retry gate so a new alarm pending from here on (e.g. a fresh
         * CLOSED edge in a later pass) is sent immediately, not held back
         * by the timestamp of this unrelated, already-successful send. */
        alarmAttempted = false;
      } else {
        Serial.println("Failed to publish ALARM, will retry");
        return; /* connection likely dead; stop this pass early */
      }
    }
  }

  /* 4. Drain the state queue (only has entries when they were pushed while
   *    connected earlier in this or an earlier pass). */
  bool closedState;
  while (publishes < MAX_PUBLISHES_PER_PASS && peekStateQueue(closedState)) {
    healthKick();
    publishes++;
    const char *payload = closedState ? "ON" : "OFF";
    if (mqttPublish(TOPIC_STATE, payload, true)) {
      popStateQueue();
      Serial.print("Published state ");
      Serial.println(payload);
    } else {
      Serial.println("Failed to publish state, will retry");
      return; /* keep the entry queued, stop this pass early */
    }
  }

  /* 5. Publish a pending duration, if any. */
  if (publishes < MAX_PUBLISHES_PER_PASS && durationPendingValid) {
    healthKick();
    if (mqttPublish(TOPIC_DURATION, durationPendingStr, false)) {
      Serial.println("Published duration");
      durationPendingValid = false;
    } else {
      Serial.println("Failed to publish duration, will retry");
    }
  }
}

void publisherOnConnected() {
  /* 1. Pending ALARM first, before anything else (R6). */
  if (alarmPending) {
    unsigned long age = millis() - alarmAt;
    if (age > ALARM_MAX_AGE_MS) {
      Serial.print("Dropped stale alarm, age ");
      Serial.print(age / 1000UL);
      Serial.println(" s");
      alarmPending = false;
    } else {
      alarmAttempted = true;
      lastAlarmAttemptMs = millis();
      healthKick();
      if (mqttPublish(TOPIC_ALARM, "ALARM", false)) {
        Serial.println("Published ALARM");
        alarmPending = false;
        /* See the matching comment in publisherUpdate(): only a failure
         * should gate the next attempt, not this success. */
        alarmAttempted = false;
      } else {
        Serial.println("Failed to publish ALARM on connect, will retry");
      }
    }
  }

  /* 2. Availability: online, retained (R8). */
  healthKick();
  if (mqttPublish(TOPIC_AVAILABILITY, "online", true)) {
    Serial.println("Published online");
  } else {
    Serial.println("Failed to publish online");
  }

  /* 3. Current debounced state, retained, always resent after (re)connect (R5). */
  healthKick();
  {
    const char *payload = alarmInputIsClosed() ? "ON" : "OFF";
    if (mqttPublish(TOPIC_STATE, payload, true)) {
      Serial.print("Published state ");
      Serial.println(payload);
      /* The fresh, authoritative state just went out; any leftover queued
       * ON/OFF history from before the disconnect is now superseded. */
      clearStateQueue();
    } else {
      Serial.println("Failed to publish state on connect");
    }
  }

  /* 4. Pending duration, if any. */
  if (durationPendingValid) {
    healthKick();
    if (mqttPublish(TOPIC_DURATION, durationPendingStr, false)) {
      Serial.println("Published duration");
      durationPendingValid = false;
    } else {
      Serial.println("Failed to publish duration on connect");
    }
  }

  /* 5. Boot reason, once per boot, retried on later connects until it succeeds. */
  if (!bootReasonSent) {
    healthKick();
    if (mqttPublish(TOPIC_BOOT, healthBootReason(), false)) {
      Serial.println("Published boot reason");
      bootReasonSent = true;
    } else {
      Serial.println("Failed to publish boot reason, will retry on next connect");
    }
  }
}

bool publisherHasFreshPendingAlarm() {
  if (!alarmPending) {
    return false;
  }
  unsigned long age = millis() - alarmAt; /* wrap-safe unsigned subtraction */
  return age <= ALARM_MAX_AGE_MS;
}
