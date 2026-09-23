#include <Arduino.h>
#include <ArduinoMqttClient.h>

#include "MqttConnection.h"
#include "WiFiConnection.h"
#include "SystemHealth.h"
#include "UserConfig.h"

MqttClient mqttClient(wifiClient);

void mqttSetup() {
  mqttClient.setId(mqtt_id);
  mqttClient.setUsernamePassword(mqtt_user, mqtt_pass);

  /* R7: short keep-alive so the broker notices a dead device quickly. */
  mqttClient.setKeepAliveInterval(MQTT_KEEP_ALIVE_MS);

  /* Bounds the CONNACK wait in connect() *and* the PUBACK wait of every QoS
   * 1 endMessage() (verified in MqttClient.cpp: both use the same
   * _connectionTimeout-based loop). */
  mqttClient.setConnectionTimeout(MQTT_CONNECTION_TIMEOUT_MS);

  /* Bounds the underlying TCP connect (see WiFiConnection.cpp). Set again
   * here defensively in case connectionBegin()'s call order ever changes;
   * cheap and idempotent. */
  wifiClient.setConnectionTimeout(TCP_CONNECT_TIMEOUT_MS);
}

bool mqttTryConnect() {
  /* Worst case for this one call: TCP connect up to TCP_CONNECT_TIMEOUT_MS
   * (2.5 s) + CONNACK wait up to MQTT_CONNECTION_TIMEOUT_MS (2.5 s) = ~5 s,
   * well under the ~8 s watchdog timeout. Kick right before, since nothing
   * inside this call feeds the watchdog for us (unlike WiFi.begin()). */
  healthKick();

  Serial.println(F("MQTT: connecting..."));

  /* R8: register the LWT before every connect() attempt. beginWill()
   * reallocates MqttClient's internal will buffer each time (verified in
   * MqttClient.cpp: it uses realloc(), not append), so re-registering on
   * every attempt is safe and does not leak; it also has to be redone here
   * because MqttClient::connect() clears/reopens the TCP socket each time,
   * and the will is only sent as part of the CONNECT packet built from this
   * buffer. */
  mqttClient.beginWill(TOPIC_AVAILABILITY, /* retain */ true, /* qos */ 1);
  mqttClient.print("offline");
  mqttClient.endWill();

  if (!mqttClient.connect(broker, port)) {
    Serial.print(F("MQTT: connect failed, connectError="));
    Serial.println(mqttClient.connectError());
    return false;
  }

  Serial.println(F("MQTT: connected"));
  return true;
}

bool mqttIsConnected() {
  return mqttClient.connected();
}

bool mqttPublish(const char *topic, const char *payload, bool retained) {
  size_t len = strlen(payload);

  if (!mqttClient.beginMessage(topic, len, retained, /* qos */ 1)) {
    Serial.print(F("MQTT: beginMessage failed for "));
    Serial.println(topic);
    return false;
  }

  mqttClient.print(payload);

  /* QoS 1 endMessage() blocks until PUBACK (or MQTT_CONNECTION_TIMEOUT_MS
   * elapses) and returns 1 only once the broker actually acknowledged it. */
  if (mqttClient.endMessage() != 1) {
    Serial.print(F("MQTT: publish not acked for "));
    Serial.println(topic);
    return false;
  }

  return true;
}
