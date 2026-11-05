#ifndef DEVICE_COMMANDS_H
#define DEVICE_COMMANDS_H

#include <ArduinoJson.h>
#include <string>

struct CommandResult {
    bool ok;
    std::string message;  // Error text when ok is false, human-readable summary otherwise
};

/**
 * Single entry point for commands coming from the local dashboard (/api/action)
 * and from the cloud MQTT link. It is a whitelist: only the actions below run,
 * and every parameter is validated first. Nothing reaches the radio otherwise.
 *
 *   open | close | stop   { deviceId }
 *   time1W                { deviceId, seconds: 1..600 }
 *   repeat1W              { deviceId, enabled: bool }
 *   edit1W                { deviceId, name: 1..32 bytes, no control characters }
 */
CommandResult handleCommand(const char *action, JsonObjectConst params);

#endif  // DEVICE_COMMANDS_H
