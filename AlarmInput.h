#ifndef ALARMINPUT_H
#define ALARMINPUT_H

/*
 * Alarm input on SWITCH_PIN (R1-R4b).
 * A pin-change ISR records edge timestamps; alarmInputUpdate() debounces them
 * and turns them into events, so closures during blocking calls are not lost.
 */

enum AlarmEventType {
  ALARM_EVENT_CLOSED,  /* Debounced open->closed edge: one alarm (R3) */
  ALARM_EVENT_OPENED   /* Debounced closed->open edge: closure ended (R4b) */
};

struct AlarmEvent {
  AlarmEventType type;
  unsigned long closedAtMs;  /* millis() when the closure started (both types) */
  unsigned long durationMs;  /* ALARM_EVENT_OPENED only: how long it was closed */
};

/* Configure the pin and attach the interrupt. A relay that is already closed
 * is reported as a new alarm once it has been closed for DEBOUNCE_MS. */
void alarmInputBegin();

/* Call on every loop() pass (at least every 100 ms). Polls the pin as a backup
 * to the ISR, debounces, and queues events. Never blocks. */
void alarmInputUpdate();

/* Pop the oldest queued event. Returns false when the queue is empty. */
bool alarmInputNextEvent(AlarmEvent &event);

/* Debounced contact state: true while closed. */
bool alarmInputIsClosed();

#endif /* ALARMINPUT_H */
