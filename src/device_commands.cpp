#include <device_commands.h>

#include <algorithm>
#include <cstring>
#include <mutex>

#include <cloud_mqtt.h>
#include <iohcCryptoHelpers.h>
#include <iohcRemote1W.h>
#include <tokens.h>
#include <user_config.h>
#include <utils.h>
#if defined(WEBSERVER)
#include <web_server_handler.h>
#endif

namespace {

constexpr int kMaxTravelTimeSeconds = 600;
constexpr size_t kMaxNameLength = 32;

// Serializes the web server task and the cloud MQTT task.
std::mutex s_commandMutex;

using Remote = IOHC::iohcRemote1W::remote;

const Remote *findRemote(const char *deviceId) {
    const auto &remotes = IOHC::iohcRemote1W::getInstance()->getRemotes();
    auto it = std::find_if(remotes.begin(), remotes.end(), [&](const Remote &r) {
        return bytesToHexString(r.node, sizeof(r.node)) == deviceId;
    });
    return it == remotes.end() ? nullptr : &*it;
}

bool isValidName(const char *name) {
    const size_t length = strlen(name);
    if (length == 0 || length > kMaxNameLength) {
        return false;
    }
    return std::none_of(name, name + length, [](char c) {
        const auto byte = static_cast<unsigned char>(c);
        return byte < 0x20 || byte == 0x7F;
    });
}

bool toButton(const char *action, IOHC::RemoteButton &button) {
    if (strcmp(action, "open") == 0) {
        button = IOHC::RemoteButton::Open;
    } else if (strcmp(action, "close") == 0) {
        button = IOHC::RemoteButton::Close;
    } else if (strcmp(action, "stop") == 0) {
        button = IOHC::RemoteButton::Stop;
    } else {
        return false;
    }
    return true;
}

}  // namespace

CommandResult handleCommand(const char *action, JsonObjectConst params) {
    const std::lock_guard<std::mutex> lock(s_commandMutex);

    IOHC::RemoteButton button{};
    const bool isButton = toButton(action, button);
    const bool isPosition = strcmp(action, "position") == 0;
    const bool isSetting = strcmp(action, "time1W") == 0 || strcmp(action, "repeat1W") == 0 ||
                           strcmp(action, "edit1W") == 0;
    if (!isButton && !isPosition && !isSetting) {
        return {false, "Invalid action"};
    }

    if (!params["deviceId"].is<const char *>()) {
        return {false, "Missing deviceId"};
    }
    const Remote *remote = findRemote(params["deviceId"].as<const char *>());
    if (remote == nullptr) {
        return {false, "Unknown device"};
    }
    // Copied because a rename below may touch the remote the pointer refers to.
    const std::string description = remote->description;
    const std::string displayName = remote->name;
    const std::string deviceId = params["deviceId"].as<const char *>();
    auto *remotes = IOHC::iohcRemote1W::getInstance();

    CommandResult result{true, ""};
    if (isButton) {
        Tokens tokens;
        tokens.push_back(action);
        tokens.push_back(description);
        remotes->cmd(button, &tokens);
#if defined(WEBSERVER)
        broadcastDevicePosition(deviceId.c_str(), static_cast<int>(remote->positionTracker.getPosition()));
#endif
        result.message = std::string("Action ") + action + " sent to " + displayName;
    } else if (isPosition) {
        // The motor moves to the percentage itself; the tracker only estimates progress.
        if (!params["percent"].is<int>()) {
            return {false, "Invalid percent"};
        }
        const int percent = params["percent"].as<int>();
        if (percent < 0 || percent > 100) {
            return {false, "Invalid percent"};
        }
        Tokens tokens;
        tokens.push_back(action);
        tokens.push_back(description);
        tokens.push_back(std::to_string(percent));
        remotes->cmd(IOHC::RemoteButton::Position, &tokens);
#if defined(WEBSERVER)
        broadcastDevicePosition(deviceId.c_str(), static_cast<int>(remote->positionTracker.getPosition()));
#endif
        result.message = "Position " + std::to_string(percent) + "% sent to " + displayName;
    } else if (strcmp(action, "time1W") == 0) {
        if (!params["seconds"].is<int>()) {
            return {false, "Invalid seconds"};
        }
        const int seconds = params["seconds"].as<int>();
        if (seconds < 1 || seconds > kMaxTravelTimeSeconds) {
            return {false, "Invalid seconds"};
        }
        if (!remotes->setTravelTime(description, static_cast<uint32_t>(seconds))) {
            return {false, "Could not set travel time"};
        }
        result.message = "Travel time updated";
    } else if (strcmp(action, "repeat1W") == 0) {
        if (!params["enabled"].is<bool>()) {
            return {false, "Invalid enabled"};
        }
        if (!remotes->setRepeatOnNoResponse(description, params["enabled"].as<bool>())) {
            return {false, "Could not set repeat"};
        }
        result.message = "Repeat updated";
    } else {
        if (!params["name"].is<const char *>() || !isValidName(params["name"].as<const char *>())) {
            return {false, "Invalid name"};
        }
        if (!remotes->renameRemote(description, params["name"].as<const char *>())) {
            return {false, "Could not rename device"};
        }
        result.message = "Device renamed";
    }

    cloudMqttRequestStatePublish();
    return result;
}
