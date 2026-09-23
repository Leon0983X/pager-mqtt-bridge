#ifndef ALARMPUBLISHER_H
#define ALARMPUBLISHER_H

/*
 * Turns alarm input events into MQTT messages (R5, R6, R4b).
 * Alarms that cannot be sent are kept until the connection is back,
 * unless they are older than ALARM_MAX_AGE_MS.
 */

/* Call on every loop() pass. Drains alarmInput events, publishes what it can,
 * retries or expires a pending alarm. mqttConnected is the current MQTT state. */
void publisherUpdate(bool mqttConnected);

/* Call once right after every MQTT (re)connect. Publishes a pending ALARM
 * first, then availability "online", the current state, a pending duration,
 * and the boot reason (once per boot). Kicks the watchdog between publishes. */
void publisherOnConnected();

/* True while an ALARM is waiting to be sent and is younger than
 * ALARM_MAX_AGE_MS. The device must not reboot in that case (R11.4). */
bool publisherHasFreshPendingAlarm();

#endif /* ALARMPUBLISHER_H */
