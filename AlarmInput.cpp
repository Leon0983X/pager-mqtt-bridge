#include <Arduino.h>
#include <pinDefinitions.h>  // digitalPinToGpio()

#include "AlarmInput.h"
#include "UserConfig.h"

/*
 * Alarm input: ISR-latched edge timestamps + a main-loop debounce state
 * machine (R1-R4b). The ISR only latches *complete* pulses of at least
 * DEBOUNCE_MS (see isrPulseSeq below); relying on the latest edge timestamps
 * alone would lose a closure that bounces at release during a blocking call.
 * Beyond that latch, while debouncedClosed is true, alarmInputUpdate() also
 * detects a release AND a new re-close that both happened inside the same
 * blocking gap (a release
 * followed by open >= DEBOUNCE_MS, then a fresh closure already closed for
 * >= DEBOUNCE_MS by the time we poll). Without this, such a pair would be
 * silently swallowed because the pin reads "still closed" on both sides of
 * the gap, violating R4 ("never miss a closure during blocking work"). A
 * short open glitch in the middle of a closure (< DEBOUNCE_MS) is still
 * ignored, exactly as before.
 *
 * millis() on this core (arduino:mbed_nano 4.6.0) reads an mbed::LowPowerTimer
 * via duration_cast<milliseconds>(elapsed_time()).count() (wiring.cpp) - a
 * plain hardware-timer read with no locking, so it is safe to call from ISR
 * context. attachInterrupt(..., CHANGE) on this core creates an
 * mbed::InterruptIn whose rise()/fall() callbacks run in genuine IRQ context
 * (Interrupts.cpp), not a deferred RTOS thread, so the ISR below must stay
 * minimal: millis() + volatile writes only.
 * noInterrupts()/interrupts() map to __disable_irq()/__enable_irq()
 * (Arduino.h), i.e. a global interrupt-mask critical section - appropriate
 * here since the "critical section" only ever needs to exclude this one pin
 * ISR and is kept to a handful of instructions.
 */

namespace {

/* ---- ISR-owned state: written only inside the ISR or inside a
 * noInterrupts()/interrupts() critical section that mirrors it. ---- */
volatile bool          isrLevelClosed = false;  // raw (undebounced) level right now
volatile unsigned long isrCloseStart  = 0;      // millis() of the latest open->closed edge
volatile unsigned long isrLastOpen    = 0;      // millis() of the latest closed->open edge
volatile unsigned long isrPulseStart  = 0;      // start of the latest COMPLETE pulse (>= DEBOUNCE_MS)
volatile unsigned long isrPulseEnd    = 0;      // end of that pulse
volatile unsigned long isrPulseSeq    = 0;      // increments once per complete pulse

/* ---- main-loop-owned debounce state (touched only from alarmInputUpdate) ---- */
bool          debouncedClosed = false;
unsigned long closedSince     = 0;
unsigned long ackedSeq        = 0;  // last isrPulseSeq already turned into events

/* ---- 16-entry event FIFO ---- */
const uint8_t EVENT_QUEUE_SIZE = 16;
AlarmEvent    eventQueue[EVENT_QUEUE_SIZE];
uint8_t       eventHead  = 0;  // index of the oldest queued event
uint8_t       eventCount = 0;

bool readRawClosed() {
  int level = digitalRead(SWITCH_PIN);
  return SWITCH_ACTIVE_HIGH ? (level == HIGH) : (level == LOW);
}

/* Applies one edge to the ISR-owned state. Caller must already hold the
 * critical section (interrupts disabled) and supply the edge timestamp. */
void applyEdgeLocked(bool closed, unsigned long now) {
  isrLevelClosed = closed;
  if (closed) {
    isrCloseStart = now;
  } else {
    isrLastOpen = now;
    if (now - isrCloseStart >= DEBOUNCE_MS) {  // unsigned subtraction: millis() wrap-safe
      isrPulseStart = isrCloseStart;
      isrPulseEnd   = now;
      isrPulseSeq++;
    }
  }
}

/* Pin-change ISR (R4). Kept minimal: millis() + volatile writes only. */
void isr() {
  unsigned long now    = millis();
  bool          closed = readRawClosed();
  if (closed == isrLevelClosed) {
    return;  // spurious / duplicate notification
  }
  applyEdgeLocked(closed, now);
}

void queueEvent(AlarmEventType type, unsigned long closedAtMs, unsigned long durationMs) {
  if (eventCount >= EVENT_QUEUE_SIZE) {
    Serial.println("AlarmInput: event queue full, dropping event");
    return;
  }
  uint8_t idx           = (eventHead + eventCount) % EVENT_QUEUE_SIZE;
  eventQueue[idx].type       = type;
  eventQueue[idx].closedAtMs = closedAtMs;
  eventQueue[idx].durationMs = durationMs;
  eventCount++;
}

void logClosed() {
  Serial.println("Alarm input: CLOSED");
}

void logDuration(unsigned long durationMs) {
  unsigned long wholeSeconds = durationMs / 1000UL;
  unsigned long millisPart   = durationMs % 1000UL;
  Serial.print("Contact closed for ");
  Serial.print(wholeSeconds);
  Serial.print(".");
  if (millisPart < 100UL) Serial.print("0");
  if (millisPart < 10UL)  Serial.print("0");
  Serial.print(millisPart);
  Serial.println(" s");
}

}  // namespace

void alarmInputBegin() {
  pinMode(SWITCH_PIN, SWITCH_ACTIVE_HIGH ? INPUT_PULLDOWN : INPUT_PULLUP);

  /* Seed the ISR state before the ISR can run. */
  noInterrupts();
  isrLevelClosed = readRawClosed();
  isrCloseStart  = millis();
  isrLastOpen    = isrCloseStart;
  interrupts();

  attachInterrupt(digitalPinToInterrupt(SWITCH_PIN), isr, CHANGE);

  /* attachInterrupt() creates an mbed::InterruptIn, whose constructor
   * re-initialises the pin with PullDefault, which is PullNone on RP2040. That
   * silently drops the pull set by pinMode() above and leaves D2 floating
   * (random closures). Re-apply the pull on the existing DigitalInOut: no
   * heap allocation and no object swap under the running ISR. */
  digitalPinToGpio(SWITCH_PIN)->mode(SWITCH_ACTIVE_HIGH ? PullDown : PullUp);

  /* Anything the ISR recorded while the pin floated is meaningless; restart
   * from the settled level and acknowledge any pulse latched meanwhile.
   * A relay already closed at boot is picked up by the normal
   * "!debouncedClosed && levelClosed && now - closeStart >= DEBOUNCE_MS"
   * path in alarmInputUpdate() once DEBOUNCE_MS has elapsed (R3). */
  delay(1);  // let the pad settle after the pull change
  noInterrupts();
  isrLevelClosed = readRawClosed();
  isrCloseStart  = millis();
  isrLastOpen    = isrCloseStart;
  isrPulseStart  = isrPulseEnd = 0;
  ackedSeq       = isrPulseSeq;  // ignore anything latched while floating
  interrupts();
}

void alarmInputUpdate() {
  unsigned long now = millis();

  bool          levelClosed;
  unsigned long closeStart;
  unsigned long lastOpen;
  unsigned long pulseStart;
  unsigned long pulseEnd;
  unsigned long pulseSeq;

  noInterrupts();
  bool rawClosed = readRawClosed();
  if (rawClosed != isrLevelClosed) {
    /* The ISR missed this edge (e.g. we were stuck in a blocking Wi-Fi/MQTT
     * call). Synthesize it here, using the same logic as the ISR (R4). */
    applyEdgeLocked(rawClosed, now);
  }
  levelClosed = isrLevelClosed;
  closeStart  = isrCloseStart;
  lastOpen    = isrLastOpen;
  pulseStart  = isrPulseStart;
  pulseEnd    = isrPulseEnd;
  pulseSeq    = isrPulseSeq;
  interrupts();

  if (!debouncedClosed) {
    if (pulseSeq != ackedSeq) {
      /* A complete closure >= DEBOUNCE_MS started and ended without us ever
       * observing "closed" in between (a short pulse entirely inside a
       * blocking gap). Report it as one CLOSED + one OPENED. */
      unsigned long duration = pulseEnd - pulseStart;  // wrap-safe
      queueEvent(ALARM_EVENT_CLOSED, pulseStart, 0);
      logClosed();
      queueEvent(ALARM_EVENT_OPENED, pulseStart, duration);
      logDuration(duration);
      ackedSeq = pulseSeq;
      /* Fall through: a new closure may already be in progress. */
    }

    if (levelClosed && (now - closeStart) >= DEBOUNCE_MS) {  // wrap-safe
      debouncedClosed = true;
      closedSince      = closeStart;
      queueEvent(ALARM_EVENT_CLOSED, closedSince, 0);
      logClosed();
    }
  } else {
    if (pulseSeq != ackedSeq && levelClosed && (closeStart - pulseEnd) >= DEBOUNCE_MS) {  // wrap-safe
      /* The contact released AND closed again for a new alarm, both inside
       * one gap between alarmInputUpdate() calls (e.g. a blocking Wi-Fi/MQTT
       * call): pulseEnd is the release that ended the current closure, and
       * closeStart is a later, separate re-close that has itself already
       * been closed for >= DEBOUNCE_MS, with an open gap of >= DEBOUNCE_MS
       * in between them (R4 - never miss a closure during blocking work).
       * Report the end of the old closure and the start of the new one;
       * debouncedClosed stays true since the contact is closed right now.
       * A short open glitch in between (gap < DEBOUNCE_MS) falls through to
       * the check below instead, and is ignored like any mid-closure
       * bounce - ackedSeq is left alone and is caught up at the real
       * release, same as today. */
      unsigned long duration = pulseEnd - closedSince;  // wrap-safe
      queueEvent(ALARM_EVENT_OPENED, closedSince, duration);
      logDuration(duration);
      closedSince = closeStart;
      queueEvent(ALARM_EVENT_CLOSED, closedSince, 0);
      logClosed();
      ackedSeq = pulseSeq;
    } else if (!levelClosed && (now - lastOpen) >= DEBOUNCE_MS) {  // wrap-safe
      debouncedClosed          = false;
      unsigned long duration   = lastOpen - closedSince;    // wrap-safe
      queueEvent(ALARM_EVENT_OPENED, closedSince, duration);
      /* This release's pulse is the same closure already reported as
       * CLOSED; acknowledge it so it can never fire a second CLOSED. */
      ackedSeq = pulseSeq;
      logDuration(duration);
    }
  }
}

bool alarmInputNextEvent(AlarmEvent &event) {
  if (eventCount == 0) {
    return false;
  }
  event = eventQueue[eventHead];
  eventHead  = (eventHead + 1) % EVENT_QUEUE_SIZE;
  eventCount--;
  return true;
}

bool alarmInputIsClosed() {
  return debouncedClosed;
}
