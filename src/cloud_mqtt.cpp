#include <cloud_mqtt.h>

#include <secrets_config.h>

#if defined(HAS_SECRETS)

#include <atomic>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <mbedtls/md.h>
#include <mqtt_client.h>

#include <device_commands.h>
#include <firmware_version.h>
#include <iohcCryptoHelpers.h>
#include <iohcRemote1W.h>
#include <sun_schedule.h>
#include <utils.h>

namespace {

// Single place for the topic prefix on the firmware side. The webapp has its own
// constant in webapp/app.js: keep both identical.
#define CLOUD_MQTT_TOPIC_PREFIX "home/esp32lora"
constexpr char kTopicCmd[] = CLOUD_MQTT_TOPIC_PREFIX "/cmd";
constexpr char kTopicAck[] = CLOUD_MQTT_TOPIC_PREFIX "/ack";
constexpr char kTopicState[] = CLOUD_MQTT_TOPIC_PREFIX "/state";
constexpr char kTopicStatus[] = CLOUD_MQTT_TOPIC_PREFIX "/status";

constexpr int kQos = 1;
constexpr int kKeepaliveSeconds = 30;
constexpr int kBufferSize = 2048;
constexpr size_t kMaxCommandBytes = 512;
constexpr size_t kMaxIdLength = 64;
constexpr size_t kDedupSize = 16;
constexpr long long kMaxCommandAgeSeconds = 60;
constexpr time_t kMinValidEpoch = 1700000000;
constexpr uint32_t kBackoffMinMs = 1000;
constexpr uint32_t kBackoffMaxMs = 60000;
constexpr uint32_t kReconnectWatchdogMs = 120000;
constexpr uint32_t kStatePeriodMs = 60000;
constexpr uint32_t kChangeCheckPeriodMs = 5000;

// flespi chain root (GlobalSign Root CA - R6, valid until 2034). SHA-256 fingerprint:
// 2C:AB:EA:FE:37:D0:6C:A2:2A:BA:73:91:C0:03:3D:25:98:29:52:C4:53:64:73:49:76:3A:3A:B5:AD:6C:CF:69
constexpr char kBrokerRootCa[] = R"PEM(-----BEGIN CERTIFICATE-----
MIIFgzCCA2ugAwIBAgIORea7A4Mzw4VlSOb/RVEwDQYJKoZIhvcNAQEMBQAwTDEg
MB4GA1UECxMXR2xvYmFsU2lnbiBSb290IENBIC0gUjYxEzARBgNVBAoTCkdsb2Jh
bFNpZ24xEzARBgNVBAMTCkdsb2JhbFNpZ24wHhcNMTQxMjEwMDAwMDAwWhcNMzQx
MjEwMDAwMDAwWjBMMSAwHgYDVQQLExdHbG9iYWxTaWduIFJvb3QgQ0EgLSBSNjET
MBEGA1UEChMKR2xvYmFsU2lnbjETMBEGA1UEAxMKR2xvYmFsU2lnbjCCAiIwDQYJ
KoZIhvcNAQEBBQADggIPADCCAgoCggIBAJUH6HPKZvnsFMp7PPcNCPG0RQssgrRI
xutbPK6DuEGSMxSkb3/pKszGsIhrxbaJ0cay/xTOURQh7ErdG1rG1ofuTToVBu1k
ZguSgMpE3nOUTvOniX9PeGMIyBJQbUJmL025eShNUhqKGoC3GYEOfsSKvGRMIRxD
aNc9PIrFsmbVkJq3MQbFvuJtMgamHvm566qjuL++gmNQ0PAYid/kD3n16qIfKtJw
LnvnvJO7bVPiSHyMEAc4/2ayd2F+4OqMPKq0pPbzlUoSB239jLKJz9CgYXfIWHSw
1CM69106yqLbnQneXUQtkPGBzVeS+n68UARjNN9rkxi+azayOeSsJDa38O+2HBNX
k7besvjihbdzorg1qkXy4J02oW9UivFyVm4uiMVRQkQVlO6jxTiWm05OWgtH8wY2
SXcwvHE35absIQh1/OZhFj931dmRl4QKbNQCTXTAFO39OfuD8l4UoQSwC+n+7o/h
bguyCLNhZglqsQY6ZZZZwPA1/cnaKI0aEYdwgQqomnUdnjqGBQCe24DWJfncBZ4n
WUx2OVvq+aWh2IMP0f/fMBH5hc8zSPXKbWQULHpYT9NLCEnFlWQaYw55PfWzjMpY
rZxCRXluDocZXFSxZba/jJvcE+kNb7gu3GduyYsRtYQUigAZcIN5kZeR1Bonvzce
MgfYFGM8KEyvAgMBAAGjYzBhMA4GA1UdDwEB/wQEAwIBBjAPBgNVHRMBAf8EBTAD
AQH/MB0GA1UdDgQWBBSubAWjkxPioufi1xzWx/B/yGdToDAfBgNVHSMEGDAWgBSu
bAWjkxPioufi1xzWx/B/yGdToDANBgkqhkiG9w0BAQwFAAOCAgEAgyXt6NH9lVLN
nsAEoJFp5lzQhN7craJP6Ed41mWYqVuoPId8AorRbrcWc+ZfwFSY1XS+wc3iEZGt
Ixg93eFyRJa0lV7Ae46ZeBZDE1ZXs6KzO7V33EByrKPrmzU+sQghoefEQzd5Mr61
55wsTLxDKZmOMNOsIeDjHfrYBzN2VAAiKrlNIC5waNrlU/yDXNOd8v9EDERm8tLj
vUYAGm0CuiVdjaExUd1URhxN25mW7xocBFymFe944Hn+Xds+qkxV/ZoVqW/hpvvf
cDDpw+5CRu3CkwWJ+n1jez/QcYF8AOiYrg54NMMl+68KnyBr3TsTjxKM4kEaSHpz
oHdpx7Zcf4LIHv5YGygrqGytXm3ABdJ7t+uA/iU3/gKbaKxCXcPu9czc8FB10jZp
nOZ7BN9uBmm23goJSFmH63sUYHpkqmlD75HHTOwY3WzvUy2MmeFe8nI+z1TIvWfs
pA9MRf/TuTAjB0yPEL+GltmZWrSZVxykzLsViVO6LAUP5MSeGbEYNNVMnbrt9x+v
JJUEeKgDu+6B5dpffItKoZB0JaezPkvILFa9x8jvOOJckvB595yEunQtYQEgfn7R
8k8HWV+LLUNS60YMlOH1Zkd5d9VUWx+tJDfLRVpOoERIyNiwmcUVhAn21klJwGW4
5hpxbqCo8YLoRT5s1gLXCmeDBVrJpBA=
-----END CERTIFICATE-----
)PEM";

esp_mqtt_client_handle_t s_client = nullptr;
QueueHandle_t s_commandQueue = nullptr;
std::atomic<bool> s_connected{false};
std::atomic<bool> s_retryPending{false};
std::atomic<bool> s_justConnected{false};
std::atomic<bool> s_statePublishRequested{false};

std::vector<std::string> s_seenIds;
size_t s_seenIdsNext = 0;
std::string s_lastValues;

bool alreadySeen(const char *id) {
    for (const auto &seen : s_seenIds) {
        if (seen == id) {
            return true;
        }
    }
    if (s_seenIds.size() < kDedupSize) {
        s_seenIds.emplace_back(id);
    } else {
        s_seenIds[s_seenIdsNext] = id;
        s_seenIdsNext = (s_seenIdsNext + 1) % kDedupSize;
    }
    return false;
}

void publish(const char *topic, const std::string &payload, bool retain) {
    if (esp_mqtt_client_enqueue(s_client, topic, payload.c_str(), static_cast<int>(payload.size()),
                                kQos, retain ? 1 : 0, true) < 0) {
        Serial.printf("CloudMQTT: could not queue message for %s\n", topic);
    }
}

void publishAck(const char *id, bool ok, const char *error) {
    JsonDocument doc;
    if (id != nullptr) {
        doc["id"] = id;
    } else {
        doc["id"] = nullptr;
    }
    doc["ok"] = ok;
    if (error != nullptr) {
        doc["error"] = error;
    } else {
        doc["error"] = nullptr;
    }
    std::string payload;
    serializeJson(doc, payload);
    publish(kTopicAck, payload, false);
}

// Field list is explicit on purpose: pairing keys must never leave the device.
std::string buildValues() {
    JsonDocument doc;
    JsonArray devices = doc["devices"].to<JsonArray>();
    for (const auto &r : IOHC::iohcRemote1W::getInstance()->getRemotes()) {
        JsonObject device = devices.add<JsonObject>();
        device["id"] = bytesToHexString(r.node, sizeof(r.node));
        device["name"] = r.name;
        device["position"] = static_cast<int>(r.positionTracker.getPosition());
        device["paired"] = r.paired;
        device["travel_time"] = r.travelTime;
        device["room"] = r.room;
        device["repeat"] = r.repeatOnNoResponse;
        // Offset in minutes; left out when the sun action is off, to keep
        // the state within kBufferSize.
        if (r.sunrise) {
            device["sunrise"] = r.sunriseOffset;
        }
        if (r.sunset) {
            device["sunset"] = r.sunsetOffset;
        }
    }
    JsonObject sun = doc["sun"].to<JsonObject>();
    std::string latitude;
    std::string longitude;
    sunLocation(latitude, longitude);
    sun["latitude"] = latitude;
    sun["longitude"] = longitude;
    time_t sunrise = 0;
    time_t sunset = 0;
    if (sunTimesToday(sunrise, sunset)) {
        sun["sunrise"] = static_cast<long long>(sunrise);
        sun["sunset"] = static_cast<long long>(sunset);
    }
    std::string values;
    serializeJson(doc, values);
    return values;
}

void publishState(const std::string &values) {
    JsonDocument doc;
    doc["uptime_s"] = millis() / 1000;
    doc["rssi_wifi"] = WiFi.RSSI();
    doc["fw"] = firmwareVersion();
    doc["values"] = serialized(values);
    std::string payload;
    serializeJson(doc, payload);
    if (payload.size() > static_cast<size_t>(kBufferSize)) {
        Serial.printf("CloudMQTT: state too large (%u bytes)\n", static_cast<unsigned>(payload.size()));
        return;
    }
    publish(kTopicState, payload, true);
    s_lastValues = values;
}

// Text covered by the signature. Same construction as signedText() in webapp/app.js.
std::string signedText(const char *id, long long ts, const char *action, JsonObjectConst params) {
    // An offset, or nothing when null.
    const auto offsetText = [](JsonVariantConst offset) {
        return offset.is<int>() ? std::to_string(offset.as<int>()) : std::string();
    };
    const auto text = [](JsonVariantConst value) {
        return std::string(value.is<const char *>() ? value.as<const char *>() : "");
    };
    std::string value;
    if (strcmp(action, "sun1W") == 0) {
        value = offsetText(params["sunrise"]) + "," + offsetText(params["sunset"]);
    } else if (strcmp(action, "location") == 0) {
        value = text(params["latitude"]) + "," + text(params["longitude"]);
    } else if (params["seconds"].is<int>()) {
        value = std::to_string(params["seconds"].as<int>());
    } else if (params["enabled"].is<bool>()) {
        value = params["enabled"].as<bool>() ? "1" : "0";
    } else if (params["name"].is<const char *>()) {
        value = params["name"].as<const char *>();
    } else if (params["percent"].is<int>()) {
        value = std::to_string(params["percent"].as<int>());
    } else if (params["room"].is<const char *>()) {
        value = params["room"].as<const char *>();
    }
    const char *deviceId = params["deviceId"].is<const char *>() ? params["deviceId"].as<const char *>() : "";
    return std::string(id) + "\n" + std::to_string(ts) + "\n" + action + "\n" + deviceId + "\n" + value;
}

bool hasValidSignature(const std::string &text, const char *signatureHex) {
    constexpr size_t kMacSize = 32;
    if (signatureHex == nullptr || strlen(signatureHex) != kMacSize * 2) {
        return false;
    }
    unsigned char mac[kMacSize];
    const auto *key = reinterpret_cast<const unsigned char *>(CLOUD_COMMAND_KEY);
    if (mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), key, strlen(CLOUD_COMMAND_KEY),
                        reinterpret_cast<const unsigned char *>(text.data()), text.size(), mac) != 0) {
        return false;
    }
    // Constant time: the comparison must not reveal how many bytes matched.
    static const char kHex[] = "0123456789abcdef";
    unsigned char difference = 0;
    for (size_t i = 0; i < kMacSize; ++i) {
        difference |= static_cast<unsigned char>(signatureHex[i * 2] ^ kHex[mac[i] >> 4]);
        difference |= static_cast<unsigned char>(signatureHex[i * 2 + 1] ^ kHex[mac[i] & 0x0F]);
    }
    return difference == 0;
}

void processCommand(const std::string &raw) {
    JsonDocument doc;
    if (deserializeJson(doc, raw) != DeserializationError::Ok || !doc.is<JsonObject>()) {
        Serial.println("CloudMQTT: rejected command: invalid JSON");
        publishAck(nullptr, false, "Invalid JSON");
        return;
    }

    const char *id = doc["id"].is<const char *>() ? doc["id"].as<const char *>() : nullptr;
    if (id == nullptr || id[0] == '\0' || strlen(id) > kMaxIdLength) {
        Serial.println("CloudMQTT: rejected command: invalid id");
        publishAck(nullptr, false, "Invalid id");
        return;
    }
    if (!doc["ts"].is<long long>() || !doc["action"].is<const char *>()) {
        publishAck(id, false, "Invalid command");
        return;
    }
    const long long ts = doc["ts"].as<long long>();
    const char *action = doc["action"].as<const char *>();
    const JsonObjectConst params = doc["params"].as<JsonObjectConst>();

    // Checked before anything else is recorded: an unsigned message has no effect at all.
    const char *signature = doc["sig"].is<const char *>() ? doc["sig"].as<const char *>() : nullptr;
    if (!hasValidSignature(signedText(id, ts, action, params), signature)) {
        Serial.printf("CloudMQTT: rejected command %s: bad signature\n", id);
        publishAck(id, false, "Bad signature");
        return;
    }

    if (alreadySeen(id)) {
        Serial.printf("CloudMQTT: ignored duplicate command %s\n", id);
        return;
    }

    // A blind must not move because of an old command delivered late.
    const long long now = time(nullptr);
    if (ts < now - kMaxCommandAgeSeconds || ts > now + kMaxCommandAgeSeconds) {
        Serial.printf("CloudMQTT: rejected stale command %s\n", id);
        publishAck(id, false, "Stale command");
        return;
    }

    Serial.printf("CloudMQTT: command %s action=%s\n", id, action);

    const CommandResult result = handleCommand(action, params);
    publishAck(id, result.ok, result.ok ? nullptr : result.message.c_str());
    publishState(buildValues());
    s_statePublishRequested = false;
}

void onMqttEvent(void *, esp_event_base_t, int32_t eventId, void *eventData) {
    auto *event = static_cast<esp_mqtt_event_handle_t>(eventData);
    switch (static_cast<esp_mqtt_event_id_t>(eventId)) {
        case MQTT_EVENT_CONNECTED:
            Serial.printf("CloudMQTT: connected, free heap %u, minimum %u\n",
                          static_cast<unsigned>(ESP.getFreeHeap()),
                          static_cast<unsigned>(ESP.getMinFreeHeap()));
            s_connected = true;
            s_retryPending = false;
            s_justConnected = true;
            break;
        case MQTT_EVENT_DISCONNECTED:
            Serial.println("CloudMQTT: disconnected");
            s_connected = false;
            s_retryPending = true;
            break;
        case MQTT_EVENT_ERROR:
            if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
                Serial.printf("CloudMQTT: transport error esp_tls=0x%x tls_stack=0x%x errno=%d\n",
                              event->error_handle->esp_tls_last_esp_err,
                              event->error_handle->esp_tls_stack_err,
                              event->error_handle->esp_transport_sock_errno);
            } else if (event->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
                Serial.printf("CloudMQTT: connection refused, code 0x%x\n",
                              event->error_handle->connect_return_code);
            } else {
                Serial.println("CloudMQTT: error");
            }
            break;
        case MQTT_EVENT_DATA: {
            const bool isCommand = event->topic_len == static_cast<int>(strlen(kTopicCmd)) &&
                                   strncmp(event->topic, kTopicCmd, event->topic_len) == 0;
            if (!isCommand) {
                break;
            }
            if (event->total_data_len != event->data_len ||
                static_cast<size_t>(event->data_len) > kMaxCommandBytes) {
                Serial.println("CloudMQTT: rejected oversized command");
                publishAck(nullptr, false, "Command too large");
                break;
            }
            // Runs in the MQTT task: execution happens in cloudTask so that a slow
            // command never delays the keepalive.
            auto *raw = new std::string(event->data, event->data_len);
            if (xQueueSend(s_commandQueue, &raw, 0) != pdTRUE) {
                Serial.println("CloudMQTT: command queue full");
                delete raw;
            }
            break;
        }
        default:
            break;
    }
}

bool startClient() {
    esp_mqtt_client_config_t config{};
    // Port 443 rather than 8883: filtered networks let it through.
    config.uri = "wss://mqtt.flespi.io:443/";
    config.username = CLOUD_MQTT_TOKEN;
    config.client_id = "iohc-esp32";
    config.cert_pem = kBrokerRootCa;
    config.lwt_topic = kTopicStatus;
    config.lwt_msg = "offline";
    config.lwt_qos = kQos;
    config.lwt_retain = 1;
    config.keepalive = kKeepaliveSeconds;
    config.buffer_size = kBufferSize;
    // Clean session: commands sent while the board is offline are dropped by the broker.
    config.disable_clean_session = false;
    // Reconnection is driven by cloudTask, with a 1 s to 60 s backoff.
    config.disable_auto_reconnect = true;
    // Below the radio tasks: a TLS handshake must not disturb them.
    config.task_prio = 1;

    s_client = esp_mqtt_client_init(&config);
    if (s_client == nullptr) {
        Serial.println("CloudMQTT: client init failed");
        return false;
    }
    esp_mqtt_client_register_event(s_client, MQTT_EVENT_ANY, onMqttEvent, nullptr);
    if (esp_mqtt_client_start(s_client) != ESP_OK) {
        Serial.println("CloudMQTT: client start failed");
        esp_mqtt_client_destroy(s_client);
        s_client = nullptr;
        return false;
    }
    Serial.println("CloudMQTT: connecting");
    return true;
}

[[noreturn]] void cloudTask(void *) {
    uint32_t backoffMs = kBackoffMinMs;
    uint32_t nextAttemptMs = 0;
    uint32_t lastAttemptMs = 0;
    uint32_t lastPublishMs = 0;
    uint32_t lastChangeCheckMs = 0;

    for (;;) {
        std::string *raw = nullptr;
        if (xQueueReceive(s_commandQueue, &raw, pdMS_TO_TICKS(250)) == pdTRUE) {
            if (s_connected) {
                processCommand(*raw);
                lastPublishMs = millis();
            }
            delete raw;
        }

        const uint32_t now = millis();
        const bool wifiUp = WiFi.status() == WL_CONNECTED;

        if (wifiUp) {
            startTimeSyncOnce();
        }

        if (s_client == nullptr) {
            // The certificate check needs the current date.
            if (wifiUp && time(nullptr) >= kMinValidEpoch && static_cast<int32_t>(now - nextAttemptMs) >= 0) {
                lastAttemptMs = now;
                if (!startClient()) {
                    nextAttemptMs = now + kBackoffMaxMs;
                }
            }
            continue;
        }

        if (s_justConnected.exchange(false)) {
            backoffMs = kBackoffMinMs;
            esp_mqtt_client_subscribe(s_client, kTopicCmd, kQos);
            publish(kTopicStatus, "online", true);
            publishState(buildValues());
            s_statePublishRequested = false;
            lastPublishMs = now;
        }

        if (!s_connected) {
            if (s_retryPending) {
                s_retryPending = false;
                nextAttemptMs = now + backoffMs;
                backoffMs = backoffMs * 2 > kBackoffMaxMs ? kBackoffMaxMs : backoffMs * 2;
                lastAttemptMs = now;
                Serial.printf("CloudMQTT: next attempt in %u ms\n", static_cast<unsigned>(nextAttemptMs - now));
            } else if (nextAttemptMs != 0 && wifiUp && static_cast<int32_t>(now - nextAttemptMs) >= 0) {
                nextAttemptMs = 0;
                lastAttemptMs = now;
                esp_mqtt_client_reconnect(s_client);
            } else if (nextAttemptMs == 0 && wifiUp && now - lastAttemptMs >= kReconnectWatchdogMs) {
                // No failure event came back from the last attempt.
                lastAttemptMs = now;
                esp_mqtt_client_reconnect(s_client);
            }
            continue;
        }

        bool shouldPublish = s_statePublishRequested.exchange(false) || now - lastPublishMs >= kStatePeriodMs;
        std::string values;
        if (shouldPublish || now - lastChangeCheckMs >= kChangeCheckPeriodMs) {
            lastChangeCheckMs = now;
            values = buildValues();
            shouldPublish = shouldPublish || values != s_lastValues;
        }
        if (shouldPublish) {
            publishState(values);
            lastPublishMs = now;
        }
    }
}

}  // namespace

void initCloudMqtt() {
    // These tags can log connection details, and logs are served by /api/logs.
    for (const char *tag : {"MQTT_CLIENT", "mqtt_client", "esp-tls", "esp-tls-mbedtls", "TRANS_SSL",
                            "TRANSPORT_BASE", "transport_base", "OUTBOX", "outbox"}) {
        esp_log_level_set(tag, ESP_LOG_WARN);
    }

    s_commandQueue = xQueueCreate(4, sizeof(std::string *));
    if (s_commandQueue == nullptr ||
        xTaskCreate(cloudTask, "cloudMqtt", 6144, /* measured ~2 KB used; commands run here */ nullptr, 1, nullptr) != pdPASS) {
        Serial.println("CloudMQTT: could not start");
    }
}

bool cloudMqttConnectedOrDisabled() {
    return s_connected.load();
}

void cloudMqttRequestStatePublish() {
    s_statePublishRequested = true;
}

#else  // No include/secrets.h: cloud link disabled.

void initCloudMqtt() {}
void cloudMqttRequestStatePublish() {}
bool cloudMqttConnectedOrDisabled() { return true; }

#endif
