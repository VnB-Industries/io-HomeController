#ifndef SUN_SCHEDULE_H
#define SUN_SCHEDULE_H

#include <ctime>
#include <string>

// Opens or closes each 1W device at sunrise or sunset, shifted by its own
// offset. The board location (latitude and longitude) is stored in NVS.
// Times are computed in UTC from the NTP clock, so no timezone is needed.
//
// A device's sunrise or sunset action runs once per day, when the clock
// reaches the event plus its offset, or up to kSunCatchUpMinutes later (for
// example after a reboot). Nothing runs until the clock is set and a
// location is stored. Days without sunrise or sunset (polar regions) skip it.

constexpr int kSunMaxOffsetMinutes = 180;
constexpr int kSunCatchUpMinutes = 10;

// Starts NTP once; safe to call from several tasks.
void startTimeSyncOnce();
// True once NTP has set the clock.
bool clockIsSet();

void initSunSchedule();

// Latitude and longitude as decimal strings, both empty when not set.
void sunLocation(std::string &latitude, std::string &longitude);
// Validates and stores the location. Two empty strings clear it.
bool setSunLocation(const std::string &latitude, const std::string &longitude);

// Today's sunrise and sunset (UTC epoch seconds) at the stored location.
// Returns false when the clock or the location is not set, or when the sun
// does not rise or set today.
bool sunTimesToday(time_t &sunrise, time_t &sunset);

#endif  // SUN_SCHEDULE_H
