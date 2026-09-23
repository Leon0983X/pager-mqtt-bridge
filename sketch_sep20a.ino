#include "AlarmInput.h"
#include "AlarmPublisher.h"
#include "SystemHealth.h"
#include "ConnectionManager.h"

void setup() {
  Serial.begin(9600);  // no wait for Serial (R13): must boot without USB

  healthBegin();       // detect restart reason, start the watchdog (R12, R14)
  alarmInputBegin();    // configure the switch pin and attach the ISR (R1-R4)
  connectionBegin();    // configure Wi-Fi/MQTT clients, does not block
}

void loop() {
  healthKick();

  alarmInputUpdate();
  bool justConnected = connectionUpdate(!publisherHasFreshPendingAlarm());

  healthKick();

  alarmInputUpdate();  // catch anything latched during a blocking reconnect

  if (justConnected) {
    publisherOnConnected();
  }
  publisherUpdate(connectionIsOnline());
}
