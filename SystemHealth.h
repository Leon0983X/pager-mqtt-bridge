#ifndef SYSTEMHEALTH_H
#define SYSTEMHEALTH_H

/* Hardware watchdog (R12) and restart reason (R14). */

/* Detect the restart reason, log it to Serial, and start the watchdog. */
void healthBegin();

/* Kick the watchdog. Safe to call often; also used as WiFiNINA's feed callback. */
void healthKick();

/* Short text for the last restart reason, e.g. "watchdog" or
 * "power-on-or-reset" (power loss and the reset button are indistinguishable
 * on this board; see SystemHealth.cpp). */
const char *healthBootReason();

/* Reset the board immediately (NVIC_SystemReset). Does not return. */
void healthReboot();

#endif /* SYSTEMHEALTH_H */
