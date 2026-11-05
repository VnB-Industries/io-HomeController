#ifndef SECRETS_CONFIG_H
#define SECRETS_CONFIG_H

// HAS_SECRETS is defined only when include/secrets.h exists. A partial file is a
// build error: it must not silently leave one of the protections switched off.
#if __has_include(<secrets.h>)
#include <secrets.h>
#define HAS_SECRETS

#if !defined(CLOUD_MQTT_HOST) || !defined(CLOUD_MQTT_USER) || !defined(CLOUD_MQTT_PASSWORD) || \
    !defined(CLOUD_COMMAND_KEY) || !defined(LOCAL_WEB_USER) || !defined(LOCAL_WEB_PASSWORD) || \
    !defined(LOCAL_AP_PASSWORD)
#error "include/secrets.h is incomplete: compare it with include/secrets.example.h"
#endif

static_assert(sizeof(LOCAL_AP_PASSWORD) > 8, "LOCAL_AP_PASSWORD needs at least 8 characters (WPA2)");
static_assert(sizeof(CLOUD_COMMAND_KEY) > 32, "CLOUD_COMMAND_KEY is too short");
#endif

#endif  // SECRETS_CONFIG_H
