#pragma once

// Lightweight timezone abstraction for the scheduler.
//
// We deliberately do NOT depend on Howard Hinnant's date/tz library or ICU,
// and we do NOT rely on `std::chrono::tzdb` because its availability across
// MSVC and libstdc++ is inconsistent (libstdc++ shipped tzdb support late and
// requires linking to a tz database file that is not always present on
// minimal CI images).
//
// The implementation supports three kinds of zones:
//
//   1. UTC (default; preserves backward compatibility for old TSV rows).
//   2. Fixed offsets, expressed as "UTC+HH:MM", "UTC-HH:MM", "UTC", "GMT".
//      These never observe DST.
//   3. A small curated table of named IANA-style zones with built-in DST
//      rules. Currently:
//        - America/New_York, America/Chicago, America/Denver,
//          America/Los_Angeles  (post-2007 US rules)
//        - Europe/London, Europe/Berlin, Europe/Paris, Europe/Madrid,
//          Europe/Rome, Europe/Amsterdam, Europe/Brussels (post-1996 EU rules)
//        - Australia/Sydney, Australia/Melbourne (post-2008 AU rules)
//        - Asia/Shanghai, Asia/Tokyo, Asia/Kolkata, Asia/Singapore,
//          Asia/Hong_Kong, Asia/Seoul, Asia/Taipei (fixed offsets, no DST)
//
// Everything else falls back to a fixed-offset interpretation if it parses
// as one, otherwise the timezone is rejected with TimezoneUnknown.
//
// Rationale: this is a stopgap that covers the most common scheduling
// scenarios while keeping the build dependency-free. See
// docs/ROADMAP.md and ARCH_ALIGNMENT.md for the longer-term plan to migrate
// to full IANA tzdb once available.

#include <chrono>
#include <optional>
#include <string>

namespace agentos {

// Disambiguation choice when a local wall-clock time falls in a DST gap or
// fold. Spring-forward gap: choose the first valid wall-clock minute AFTER
// the gap. Fall-back fold: choose the FIRST occurrence (the earlier UTC
// instant that maps to that wall-clock time).
enum class DstChoice {
    SkipGap,        // gap: advance to first valid minute after the gap
    Earliest,       // fold: choose earlier (pre-DST-end) instant
};

struct LocalTime {
    int year = 0;       // e.g. 2026
    int month = 1;      // 1..12
    int day = 1;        // 1..31
    int hour = 0;       // 0..23
    int minute = 0;     // 0..59
    int second = 0;     // 0..59
    int day_of_week = 0; // 0=Sunday..6=Saturday (informational; recomputed by helpers)
};

class Timezone {
public:
    // Parse a timezone name. Returns std::nullopt for unrecognized input.
    static std::optional<Timezone> Parse(const std::string& name);

    // The canonical UTC zone (default).
    static Timezone Utc();

    // Original input name (e.g. "America/New_York" or "UTC+08:00"). Empty
    // input is normalized to "UTC".
    [[nodiscard]] const std::string& name() const { return name_; }

    // True if this zone never observes DST.
    [[nodiscard]] bool is_fixed() const { return is_fixed_; }

    // Convert a UTC time_point to this zone's local wall-clock components.
    [[nodiscard]] LocalTime to_local(std::chrono::system_clock::time_point utc) const;

    // Convert local wall-clock components to a UTC time_point. The choice
    // parameter resolves DST gaps and folds.
    //
    // - For spring-forward gaps where the local time does not exist:
    //     SkipGap -> returns the first valid instant after the gap, with
    //                local_components describing that instant in this zone.
    //     Earliest -> still skips the gap (gaps have no earlier mapping).
    // - For fall-back folds where the local time exists twice:
    //     Earliest / SkipGap -> the earlier (pre-DST-end) instant.
    //
    // The returned time_point is exact UTC. If the caller wants to know
    // whether the input was in a gap, use was_gap.
    [[nodiscard]] std::chrono::system_clock::time_point local_to_utc(
        const LocalTime& local,
        DstChoice choice,
        bool* was_gap = nullptr,
        bool* was_fold = nullptr) const;

private:
    Timezone() = default;

    enum class RuleSet {
        Fixed,
        UsPost2007,
        EuPost1996,
        AuPost2008,
    };

    // Standard-time offset in seconds east of UTC.
    long long std_offset_seconds_ = 0;
    // DST offset in seconds (typically 3600). Zero for fixed zones.
    long long dst_offset_seconds_ = 0;
    bool is_fixed_ = true;
    RuleSet rule_set_ = RuleSet::Fixed;
    std::string name_;

    // Returns true if the given UTC instant falls in DST for this zone.
    [[nodiscard]] bool is_dst_utc(std::chrono::system_clock::time_point utc) const;
};

}  // namespace agentos
