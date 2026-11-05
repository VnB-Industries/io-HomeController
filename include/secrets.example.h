// Copy this file to include/secrets.h and fill in the values.
// include/secrets.h is ignored by Git: never commit real credentials.
// Generate each secret with: openssl rand -hex 16   (CLOUD_COMMAND_KEY: rand -hex 32)
// Without include/secrets.h the firmware builds as upstream: no cloud link,
// no local authentication, open setup access point.
#ifndef SECRETS_H
#define SECRETS_H

// flespi ACL token used only by this board.
#define CLOUD_MQTT_TOKEN    "<flespi token>"

// Shared with the webapp, never with the broker: every command is signed with it
// (HMAC-SHA256), so neither the broker nor a leaked MQTT password can move a blind.
#define CLOUD_COMMAND_KEY   "<64 hex characters>"

// Login of the local dashboard (HTTP digest authentication).
#define LOCAL_WEB_USER      "admin"
#define LOCAL_WEB_PASSWORD  "<random password>"

// WPA2 password of the iohc-setup access point, at least 8 characters.
#define LOCAL_AP_PASSWORD   "<random password>"

#endif
