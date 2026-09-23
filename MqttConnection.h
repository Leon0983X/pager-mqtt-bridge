#ifndef MQTTCONNECTION_H
#define MQTTCONNECTION_H

#include <ArduinoMqttClient.h>

extern MqttClient mqttClient;

/* Set id, credentials, keep-alive and timeouts. Does not connect. */
void mqttSetup();

/* One connection attempt: registers the LWT (R8), then connects.
 * Bounded by TCP_CONNECT_TIMEOUT_MS + MQTT_CONNECTION_TIMEOUT_MS. */
bool mqttTryConnect();

bool mqttIsConnected();

/* Publish helper: QoS 1. Returns true when the broker acknowledged it. */
bool mqttPublish(const char *topic, const char *payload, bool retained);

#endif /* MQTTCONNECTION_H */
