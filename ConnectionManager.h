#ifndef CONNECTIONMANAGER_H
#define CONNECTIONMANAGER_H

/*
 * Non-blocking Wi-Fi + MQTT supervision with backoff and escalation (R10, R11).
 */

/* Configure Wi-Fi and MQTT clients. Does not connect or block. */
void connectionBegin();

/* Call on every loop() pass. Checks the links and, when a backoff delay has
 * passed, makes one reconnect attempt (Wi-Fi first, then MQTT). Calls
 * mqttClient.poll() while connected. May escalate to a NINA reset or, when
 * offline longer than MAX_OFFLINE_MS and rebootAllowed is true, a reboot.
 * Returns true exactly on the pass where MQTT has just (re)connected. */
bool connectionUpdate(bool rebootAllowed);

/* True while Wi-Fi and MQTT are both connected. */
bool connectionIsOnline();

/* Publish retained "offline" if connected, then reboot. Does not return. */
void connectionRebootDevice();

#endif /* CONNECTIONMANAGER_H */
