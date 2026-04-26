#pragma once

// Five-field cron expression parser and next-fire-time evaluator with
// optional timezone awareness.
//
// Field order: minute (0-59) hour (0-23) day-of-month (1-31)
//              month (1-12) day-of-week (0-6, Sun=0).
//
// Supported syntax inside a field:
//   *           -> every value in range
//   N           -> single value
//   A-B         -> inclusive range
//   */S         -> step from minimum, every S
//   A-B/S       -> step over range
//   A,B,C       -> comma-separated list (each item may use any of the above)
//
// Supported aliases (single token in place of all five fields):
//   @hourly @daily @weekly @monthly @yearly @annually @midnight @noon
//
// DOM/DOW semantics: when both day-of-month and day-of-week are restricted
// (i.e. neither is "*"), we OR them, matching the classic Vixie cron
// convention (a date matches if EITHER restriction matches).

#include "scheduler/timezone.hpp"

#include <array>
#include <chrono>
#include <bitset>
#include <optional>
#include <string>

namespace agentos {

class CronExpression {
public:
    // Parse a cron expression. Returns std::nullopt on parse error.
    static std::optional<CronExpression> Parse(const std::string& expr);

    // Compute the next fire time strictly AFTER `after` in the given
    // timezone. Returns std::nullopt if no such time exists within a
    // reasonable bound (8 years, to guard against pathological inputs).
    [[nodiscard]] std::optional<std::chrono::system_clock::time_point>
    next_after(std::chrono::system_clock::time_point after, const Timezone& tz) const;

    [[nodiscard]] const std::string& source() const { return source_; }

private:
    CronExpression() = default;

    // Bit i set => value i is allowed.
    std::bitset<60> minute_;        // 0..59
    std::bitset<24> hour_;          // 0..23
    std::bitset<32> dom_;           // 1..31 (bit 0 unused)
    std::bitset<13> month_;         // 1..12 (bit 0 unused)
    std::bitset<7>  dow_;           // 0..6 (Sun=0)
    bool dom_restricted_ = false;
    bool dow_restricted_ = false;
    std::string source_;
};

}  // namespace agentos
