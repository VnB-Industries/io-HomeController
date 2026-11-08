#include <sun_schedule.h>

#include <atomic>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <map>
#include <mutex>
#include <vector>

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cloud_mqtt.h>
#include <device_commands.h>
#include <iohcRemote1W.h>
#include <nvs_helpers.h>
#include <utils.h>

namespace {

constexpr time_t kMinValidEpoch = 1700000000;
constexpr uint32_t kTickMs = 15000;
constexpr size_t kMaxCoordinateLength = 12;
constexpr double kDegree = 3.14159265358979323846 / 180.0;
constexpr double kUnixEpochJulianDay = 2440587.5;
constexpr double kJ2000 = 2451545.0;

// Written by the web and cloud tasks, read by the scheduler task.
std::mutex s_locationMutex;
bool s_hasLocation = false;
double s_latitude = 0.0;
double s_longitude = 0.0;
std::string s_latitudeText;
std::string s_longitudeText;

// A plain decimal number ("48.8566", "-1.5"), at most 12 characters, within
// [-limit, limit]. Kept strict so the signed cloud text has one spelling.
bool parseCoordinate(const std::string &text, double limit, double &value) {
    if (text.empty() || text.size() > kMaxCoordinateLength) {
        return false;
    }
    bool digits = false;
    bool dot = false;
    for (size_t i = text[0] == '-' ? 1 : 0; i < text.size(); ++i) {
        const char c = text[i];
        if (isdigit(static_cast<unsigned char>(c))) {
            digits = true;
        } else if (c == '.' && !dot) {
            dot = true;
        } else {
            return false;
        }
    }
    if (!digits) {
        return false;
    }
    value = strtod(text.c_str(), nullptr);
    return value >= -limit && value <= limit;
}

struct SunEvents {
    bool valid;
    time_t sunrise;
    time_t sunset;
};

time_t julianToUnix(double julianDay) {
    return static_cast<time_t>(llround((julianDay - kUnixEpochJulianDay) * 86400.0));
}

// Sunrise equation (https://en.wikipedia.org/wiki/Sunrise_equation), accurate
// to a minute or two. day counts days from J2000; longitude is east-positive.
SunEvents sunEventsForDay(long day, double latitude, double longitude) {
    const double meanNoon = static_cast<double>(day) - longitude / 360.0;
    const double anomaly = fmod(357.5291 + 0.98560028 * meanNoon, 360.0) * kDegree;
    const double center = 1.9148 * sin(anomaly) + 0.0200 * sin(2 * anomaly) + 0.0003 * sin(3 * anomaly);
    const double eclipticLongitude =
        fmod(anomaly / kDegree + center + 180.0 + 102.9372, 360.0) * kDegree;
    const double transit = kJ2000 + meanNoon + 0.0053 * sin(anomaly) - 0.0069 * sin(2 * eclipticLongitude);
    const double sinDeclination = sin(eclipticLongitude) * sin(23.4397 * kDegree);
    const double cosDeclination = cos(asin(sinDeclination));
    const double cosHourAngle = (sin(-0.833 * kDegree) - sin(latitude * kDegree) * sinDeclination) /
                                (cos(latitude * kDegree) * cosDeclination);
    if (cosHourAngle < -1.0 || cosHourAngle > 1.0) {
        return {false, 0, 0};  // Midnight sun or polar night.
    }
    const double halfDay = acos(cosHourAngle) / kDegree / 360.0;
    return {true, julianToUnix(transit - halfDay), julianToUnix(transit + halfDay)};
}

// The day (from J2000) whose solar noon at this longitude is nearest to t.
long solarDay(time_t t, double longitude) {
    const double julianDay = static_cast<double>(t) / 86400.0 + kUnixEpochJulianDay;
    return lround(julianDay - kJ2000 + longitude / 360.0);
}

bool readLocation(double &latitude, double &longitude) {
    const std::lock_guard<std::mutex> lock(s_locationMutex);
    latitude = s_latitude;
    longitude = s_longitude;
    return s_hasLocation;
}

struct Job {
    std::string id;
    std::string name;
    bool sunrise;
    int sunriseOffset;
    bool sunset;
    int sunsetOffset;
};

// Last day each device's sunrise and sunset action ran. Scheduler task only.
struct LastRun {
    long sunrise = LONG_MIN;
    long sunset = LONG_MIN;
};
std::map<std::string, LastRun> s_lastRun;

bool isDue(time_t now, time_t at) {
    return now >= at && now < at + kSunCatchUpMinutes * 60;
}

void run(const char *action, const Job &job, const char *event) {
    JsonDocument params;
    params["deviceId"] = job.id;
    const CommandResult result = handleCommand(action, params.as<JsonObjectConst>());
    Serial.printf("Sun: %s %s at %s: %s\n", action, job.name.c_str(), event, result.message.c_str());
    cloudMqttRequestStatePublish();
}

void runDueActions(time_t now, double latitude, double longitude) {
    std::vector<Job> jobs;
    for (const auto &r : IOHC::iohcRemote1W::getInstance()->getRemotes()) {
        if (r.sunrise || r.sunset) {
            jobs.push_back({bytesToHexString(r.node, sizeof(r.node)), r.name, r.sunrise, r.sunriseOffset,
                            r.sunset, r.sunsetOffset});
        }
    }
    if (jobs.empty()) {
        return;
    }
    // Neighbouring days too: a large offset can move an action past solar midnight.
    const long today = solarDay(now, longitude);
    for (long day = today - 1; day <= today + 1; ++day) {
        const SunEvents events = sunEventsForDay(day, latitude, longitude);
        if (!events.valid) {
            continue;
        }
        for (const auto &job : jobs) {
            LastRun &last = s_lastRun[job.id];
            if (job.sunrise && last.sunrise != day && isDue(now, events.sunrise + job.sunriseOffset * 60)) {
                last.sunrise = day;
                run("open", job, "sunrise");
            }
            if (job.sunset && last.sunset != day && isDue(now, events.sunset + job.sunsetOffset * 60)) {
                last.sunset = day;
                run("close", job, "sunset");
            }
        }
    }
}

[[noreturn]] void sunTask(void *) {
    for (;;) {
        if (WiFi.status() == WL_CONNECTED) {
            startTimeSyncOnce();
        }
        double latitude = 0.0;
        double longitude = 0.0;
        if (clockIsSet() && readLocation(latitude, longitude)) {
            runDueActions(time(nullptr), latitude, longitude);
        }
        vTaskDelay(pdMS_TO_TICKS(kTickMs));
    }
}

}  // namespace

void startTimeSyncOnce() {
    static std::atomic<bool> started{false};
    if (started.exchange(true)) {
        return;
    }
    std::string server;
    if (!nvs_read_string(NVS_KEY_NET_SNTP, server) || server.empty()) {
        server = "pool.ntp.org";
    }
    // Stored by configTime: the string must outlive this call.
    static std::string s_ntpServer;
    s_ntpServer = server;
    configTime(0, 0, s_ntpServer.c_str());
    Serial.printf("NTP sync with %s\n", s_ntpServer.c_str());
}

bool clockIsSet() {
    return time(nullptr) >= kMinValidEpoch;
}

void sunLocation(std::string &latitude, std::string &longitude) {
    const std::lock_guard<std::mutex> lock(s_locationMutex);
    latitude = s_latitudeText;
    longitude = s_longitudeText;
}

bool setSunLocation(const std::string &latitude, const std::string &longitude) {
    const bool clear = latitude.empty() && longitude.empty();
    double lat = 0.0;
    double lon = 0.0;
    if (!clear && (!parseCoordinate(latitude, 90.0, lat) || !parseCoordinate(longitude, 180.0, lon))) {
        return false;
    }
    {
        const std::lock_guard<std::mutex> lock(s_locationMutex);
        s_hasLocation = !clear;
        s_latitude = lat;
        s_longitude = lon;
        s_latitudeText = latitude;
        s_longitudeText = longitude;
    }
    nvs_write_string(NVS_KEY_SUN_LAT, latitude);
    nvs_write_string(NVS_KEY_SUN_LON, longitude);
    return true;
}

bool sunTimesToday(time_t &sunrise, time_t &sunset) {
    double latitude = 0.0;
    double longitude = 0.0;
    if (!clockIsSet() || !readLocation(latitude, longitude)) {
        return false;
    }
    const SunEvents events = sunEventsForDay(solarDay(time(nullptr), longitude), latitude, longitude);
    sunrise = events.sunrise;
    sunset = events.sunset;
    return events.valid;
}

void initSunSchedule() {
    std::string latitude;
    std::string longitude;
    double lat = 0.0;
    double lon = 0.0;
    if (nvs_read_string(NVS_KEY_SUN_LAT, latitude) && nvs_read_string(NVS_KEY_SUN_LON, longitude) &&
        parseCoordinate(latitude, 90.0, lat) && parseCoordinate(longitude, 180.0, lon)) {
        const std::lock_guard<std::mutex> lock(s_locationMutex);
        s_hasLocation = true;
        s_latitude = lat;
        s_longitude = lon;
        s_latitudeText = latitude;
        s_longitudeText = longitude;
    }
    // Same stack as the cloud task: both run device commands.
    if (xTaskCreate(sunTask, "sunSchedule", 6144, nullptr, 1, nullptr) != pdPASS) {
        Serial.println("Sun schedule: could not start");
    }
}
