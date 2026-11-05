#ifndef CLOUD_MQTT_H
#define CLOUD_MQTT_H

// Cloud MQTT link (TLS, verified certificate) used by the iOS webapp. It is
// independent of the Home Assistant client in mqtt_handler.cpp, which has no
// TLS and publishes pairing keys: that client must never point at a cloud broker.
// See docs/mqtt-contract.md for the topics and message formats.
//
// The link is compiled in only when include/secrets.h exists.

void initCloudMqtt();
void cloudMqttRequestStatePublish();

#endif  // CLOUD_MQTT_H
