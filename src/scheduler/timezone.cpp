#include "scheduler/timezone.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <string>
#include <string_view>

namespace agentos {

namespace {

constexpr long long kSecondsPerHour = 3600;
constexpr long long kSecondsPerMinute = 60;

std::string ToLowerAscii(std::string_view input) {
    std::string output;
    output.reserve(input.size());
    for (const auto ch : input) {
        output.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return output;
}

// Compute days from civil 1970-01-01 to (y, m, d). Algorithm by Howard
// Hinnant (public domain, "chrono-Compatible Low-Level Date Algorithms"),
// reimplemented here to keep the build dependency-free.
long long DaysFromCivil(int year, unsigned month, unsigned day) {
    year -= month <= 2;
    const long long era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(year - era * 400);
    const unsigned mp = month > 2 ? month - 3U : month + 9U;
    const unsigned doy = (153U * mp + 2U) / 5U + day - 1U;
    const unsigned doe = yoe * 365U + yoe / 4U - yoe / 100U + doy;
    return era * 146097LL + static_cast<long long>(doe) - 719468LL;
}

void CivilFromDays(long long days, int& year, unsigned& month, unsigned& day, unsigned& weekday) {
    days += 719468LL;
    const long long era = (days >= 0 ? days : days - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(days - era * 146097);
    const unsigned yoe = (doe - doe / 1460U + doe / 36524U - doe / 146096U) / 365U;
    const long long y = static_cast<long long>(yoe) + era * 400;
    const unsigned doy = doe - (365U * yoe + yoe / 4U - yoe / 100U);
    const unsigned mp = (5U * doy + 2U) / 153U;
    day = doy - (153U * mp + 2U) / 5U + 1U;
    month = mp < 10U ? mp + 3U : mp - 9U;
    year = static_cast<int>(y + (month <= 2 ? 1 : 0));
    // Weekday for civil 1970-01-01 was Thursday = 4. days from epoch.
    const long long w = (days % 7 + 7) % 7;
    // Adjust: epoch day (days==-719468 + 719468 = 0) corresponds to Thu (4).
    // Our `days` here has already been shifted by +719468, so day 0 inside
    // this function corresponds to civil 0000-03-01 (a Wednesday in
    // proleptic Gregorian). We need the un-shifted weekday.
    // Recompute using the original epoch-day before shift:
    const long long epoch_day = days - 719468LL;
    const long long ww = ((epoch_day % 7) + 11) % 7;  // 1970-01-01 = Thu = 4
    weekday = static_cast<unsigned>(ww);
    (void)w;
}

// Compute (year, month, day) for the n-th weekday-of-month in the given
// year/month. weekday: 0=Sunday..6=Saturday. n: 1=first, 2=second, etc.
// If n=0 returns the LAST weekday-of-month. Returns the day-of-month.
unsigned NthWeekdayOfMonth(int year, unsigned month, unsigned weekday, unsigned n) {
    const auto first_days = DaysFromCivil(year, month, 1);
    int yy;
    unsigned mm;
    unsigned dd;
    unsigned wd;
    CivilFromDays(first_days, yy, mm, dd, wd);
    // Day of month for the first matching weekday in this month.
    unsigned offset = (weekday + 7U - wd) % 7U;
    unsigned first_match = 1U + offset;
    if (n == 0) {
        // last: walk forward in 7-day steps while still in the month.
        // Determine month length.
        static const unsigned month_lengths[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
        unsigned days_in_month = month_lengths[month - 1];
        if (month == 2) {
            const bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
            if (leap) days_in_month = 29;
        }
        unsigned candidate = first_match;
        while (candidate + 7 <= days_in_month) {
            candidate += 7;
        }
        return candidate;
    }
    return first_match + (n - 1) * 7;
}

// Compute UTC seconds since epoch for the given UTC components.
long long UtcSecondsFromComponents(int year, unsigned month, unsigned day,
                                   int hour, int minute, int second) {
    const auto days = DaysFromCivil(year, month, day);
    return days * 86400LL + static_cast<long long>(hour) * kSecondsPerHour
        + static_cast<long long>(minute) * kSecondsPerMinute
        + static_cast<long long>(second);
}

// Spring-forward and fall-back transition instants for a given zone in a
// given year, expressed as UTC seconds-since-epoch. Returns std::nullopt
// for fixed zones.
struct DstWindow {
    long long dst_start_utc_sec = 0;  // moment when DST begins (UTC)
    long long dst_end_utc_sec = 0;    // moment when DST ends (UTC)
};

std::optional<DstWindow> ComputeDstWindow(int year, long long std_offset_sec,
                                          long long dst_offset_sec, int rule_set) {
    // rule_set: 1=US post-2007, 2=EU post-1996, 3=AU post-2008.
    // Returns boundaries as UTC seconds.
    switch (rule_set) {
    case 1: {
        // US: DST begins 2nd Sunday of March, 02:00 local standard time.
        // DST ends 1st Sunday of November, 02:00 local daylight time.
        const unsigned start_dom = NthWeekdayOfMonth(year, 3, 0 /*Sun*/, 2);
        const unsigned end_dom = NthWeekdayOfMonth(year, 11, 0 /*Sun*/, 1);
        // Local-standard-time 02:00 -> UTC = local + (-std_offset).
        // (std_offset is signed east-of-UTC, e.g. -18000 for EST.)
        const long long start_utc =
            UtcSecondsFromComponents(year, 3, start_dom, 2, 0, 0) - std_offset_sec;
        // End is at 02:00 local DAYLIGHT time; UTC = 02:00 - (std_offset+dst_offset).
        const long long end_utc =
            UtcSecondsFromComponents(year, 11, end_dom, 2, 0, 0) - std_offset_sec - dst_offset_sec;
        return DstWindow{start_utc, end_utc};
    }
    case 2: {
        // EU: DST begins last Sunday of March, 01:00 UTC.
        //     DST ends   last Sunday of October, 01:00 UTC.
        const unsigned start_dom = NthWeekdayOfMonth(year, 3, 0, 0);
        const unsigned end_dom = NthWeekdayOfMonth(year, 10, 0, 0);
        const long long start_utc = UtcSecondsFromComponents(year, 3, start_dom, 1, 0, 0);
        const long long end_utc = UtcSecondsFromComponents(year, 10, end_dom, 1, 0, 0);
        return DstWindow{start_utc, end_utc};
    }
    case 3: {
        // Australia (Sydney/Melbourne): DST begins 1st Sunday of October,
        // 02:00 local standard time; ends 1st Sunday of April, 03:00 local
        // daylight time. NB: southern hemisphere -> DST window wraps the
        // year boundary. We model "DST begins" as the spring-forward and
        // "DST ends" as the fall-back, where spring is Oct (forward) and
        // fall is April (back).
        const unsigned start_dom = NthWeekdayOfMonth(year, 10, 0, 1);
        const unsigned end_dom = NthWeekdayOfMonth(year, 4, 0, 1);
        const long long start_utc =
            UtcSecondsFromComponents(year, 10, start_dom, 2, 0, 0) - std_offset_sec;
        const long long end_utc =
            UtcSecondsFromComponents(year, 4, end_dom, 3, 0, 0) - std_offset_sec - dst_offset_sec;
        return DstWindow{start_utc, end_utc};
    }
    default:
        return std::nullopt;
    }
}

struct NamedZoneEntry {
    const char* name;
    long long std_offset_seconds;
    long long dst_offset_seconds;
    int rule_set;  // 0=Fixed, 1=US, 2=EU, 3=AU
};

constexpr NamedZoneEntry kNamedZones[] = {
    // North America (post-2007 US rules)
    {"America/New_York",     -5 * kSecondsPerHour, kSecondsPerHour, 1},
    {"America/Detroit",      -5 * kSecondsPerHour, kSecondsPerHour, 1},
    {"America/Toronto",      -5 * kSecondsPerHour, kSecondsPerHour, 1},
    {"America/Chicago",      -6 * kSecondsPerHour, kSecondsPerHour, 1},
    {"America/Denver",       -7 * kSecondsPerHour, kSecondsPerHour, 1},
    {"America/Los_Angeles",  -8 * kSecondsPerHour, kSecondsPerHour, 1},
    {"America/Vancouver",    -8 * kSecondsPerHour, kSecondsPerHour, 1},
    // Europe (post-1996 EU rules; UK uses same dates and 0/+1)
    {"Europe/London",         0,                   kSecondsPerHour, 2},
    {"Europe/Dublin",         0,                   kSecondsPerHour, 2},
    {"Europe/Lisbon",         0,                   kSecondsPerHour, 2},
    {"Europe/Berlin",         1 * kSecondsPerHour, kSecondsPerHour, 2},
    {"Europe/Paris",          1 * kSecondsPerHour, kSecondsPerHour, 2},
    {"Europe/Madrid",         1 * kSecondsPerHour, kSecondsPerHour, 2},
    {"Europe/Rome",           1 * kSecondsPerHour, kSecondsPerHour, 2},
    {"Europe/Amsterdam",      1 * kSecondsPerHour, kSecondsPerHour, 2},
    {"Europe/Brussels",       1 * kSecondsPerHour, kSecondsPerHour, 2},
    {"Europe/Vienna",         1 * kSecondsPerHour, kSecondsPerHour, 2},
    {"Europe/Warsaw",         1 * kSecondsPerHour, kSecondsPerHour, 2},
    {"Europe/Zurich",         1 * kSecondsPerHour, kSecondsPerHour, 2},
    {"Europe/Athens",         2 * kSecondsPerHour, kSecondsPerHour, 2},
    {"Europe/Helsinki",       2 * kSecondsPerHour, kSecondsPerHour, 2},
    // Australia (post-2008 rules; only zones that observe DST)
    {"Australia/Sydney",     10 * kSecondsPerHour, kSecondsPerHour, 3},
    {"Australia/Melbourne",  10 * kSecondsPerHour, kSecondsPerHour, 3},
    {"Australia/Hobart",     10 * kSecondsPerHour, kSecondsPerHour, 3},
    // Asia / fixed-offset (no DST)
    {"Asia/Shanghai",         8 * kSecondsPerHour, 0, 0},
    {"Asia/Hong_Kong",        8 * kSecondsPerHour, 0, 0},
    {"Asia/Singapore",        8 * kSecondsPerHour, 0, 0},
    {"Asia/Taipei",           8 * kSecondsPerHour, 0, 0},
    {"Asia/Tokyo",            9 * kSecondsPerHour, 0, 0},
    {"Asia/Seoul",            9 * kSecondsPerHour, 0, 0},
    {"Asia/Kolkata",          5 * kSecondsPerHour + 30 * kSecondsPerMinute, 0, 0},
    {"Asia/Calcutta",         5 * kSecondsPerHour + 30 * kSecondsPerMinute, 0, 0},
    {"Asia/Dubai",            4 * kSecondsPerHour, 0, 0},
    // Brisbane & Perth do not observe DST.
    {"Australia/Brisbane",   10 * kSecondsPerHour, 0, 0},
    {"Australia/Perth",       8 * kSecondsPerHour, 0, 0},
};

bool TryParseFixedOffset(const std::string& name, long long& out_seconds, std::string& canonical) {
    // Accept "UTC", "GMT", "Z", "UTC+HH", "UTC+HH:MM", "UTC-HH:MM",
    // "GMT+HH:MM", "+HH:MM", "-HH:MM".
    std::string trimmed;
    trimmed.reserve(name.size());
    for (char ch : name) {
        if (ch != ' ' && ch != '\t') trimmed.push_back(ch);
    }
    if (trimmed.empty()) return false;

    const auto lower = ToLowerAscii(trimmed);
    if (lower == "utc" || lower == "gmt" || lower == "z" || lower == "utc+0" ||
        lower == "utc-0" || lower == "utc+00" || lower == "utc-00" ||
        lower == "utc+00:00" || lower == "utc-00:00") {
        out_seconds = 0;
        canonical = "UTC";
        return true;
    }

    std::size_t pos = 0;
    if (lower.rfind("utc", 0) == 0) pos = 3;
    else if (lower.rfind("gmt", 0) == 0) pos = 3;

    if (pos >= trimmed.size()) return false;
    const char sign_char = trimmed[pos];
    if (sign_char != '+' && sign_char != '-') return false;
    const int sign = sign_char == '+' ? 1 : -1;
    ++pos;

    // Expect HH or HH:MM or HHMM.
    int hours = 0;
    int minutes = 0;
    std::size_t consumed = 0;
    while (pos + consumed < trimmed.size() && consumed < 2 &&
           std::isdigit(static_cast<unsigned char>(trimmed[pos + consumed]))) {
        hours = hours * 10 + (trimmed[pos + consumed] - '0');
        ++consumed;
    }
    if (consumed == 0) return false;
    pos += consumed;
    if (pos < trimmed.size()) {
        if (trimmed[pos] == ':') ++pos;
        std::size_t mc = 0;
        while (pos + mc < trimmed.size() && mc < 2 &&
               std::isdigit(static_cast<unsigned char>(trimmed[pos + mc]))) {
            minutes = minutes * 10 + (trimmed[pos + mc] - '0');
            ++mc;
        }
        if (mc != 0 && mc != 2) return false;
        pos += mc;
    }
    if (pos != trimmed.size()) return false;
    if (hours > 23 || minutes > 59) return false;

    out_seconds = sign * (static_cast<long long>(hours) * kSecondsPerHour
                          + static_cast<long long>(minutes) * kSecondsPerMinute);

    // Canonical form: "UTC+HH:MM" / "UTC-HH:MM" / "UTC".
    if (out_seconds == 0) {
        canonical = "UTC";
    } else {
        char buf[16];
        const long long abs_sec = out_seconds < 0 ? -out_seconds : out_seconds;
        const int hh = static_cast<int>(abs_sec / kSecondsPerHour);
        const int mm = static_cast<int>((abs_sec % kSecondsPerHour) / kSecondsPerMinute);
        std::snprintf(buf, sizeof(buf), "UTC%c%02d:%02d", out_seconds < 0 ? '-' : '+', hh, mm);
        canonical = buf;
    }
    return true;
}

}  // namespace

Timezone Timezone::Utc() {
    Timezone tz;
    tz.name_ = "UTC";
    tz.std_offset_seconds_ = 0;
    tz.dst_offset_seconds_ = 0;
    tz.is_fixed_ = true;
    tz.rule_set_ = RuleSet::Fixed;
    return tz;
}

std::optional<Timezone> Timezone::Parse(const std::string& name) {
    if (name.empty()) return Utc();

    // Named zones first (case-sensitive match against canonical names).
    for (const auto& entry : kNamedZones) {
        if (name == entry.name) {
            Timezone tz;
            tz.name_ = entry.name;
            tz.std_offset_seconds_ = entry.std_offset_seconds;
            tz.dst_offset_seconds_ = entry.dst_offset_seconds;
            tz.is_fixed_ = entry.rule_set == 0;
            switch (entry.rule_set) {
            case 1: tz.rule_set_ = RuleSet::UsPost2007; break;
            case 2: tz.rule_set_ = RuleSet::EuPost1996; break;
            case 3: tz.rule_set_ = RuleSet::AuPost2008; break;
            default: tz.rule_set_ = RuleSet::Fixed; break;
            }
            return tz;
        }
    }

    long long offset_sec = 0;
    std::string canonical;
    if (TryParseFixedOffset(name, offset_sec, canonical)) {
        Timezone tz;
        tz.name_ = canonical;
        tz.std_offset_seconds_ = offset_sec;
        tz.dst_offset_seconds_ = 0;
        tz.is_fixed_ = true;
        tz.rule_set_ = RuleSet::Fixed;
        return tz;
    }

    return std::nullopt;
}

bool Timezone::is_dst_utc(std::chrono::system_clock::time_point utc) const {
    if (rule_set_ == RuleSet::Fixed) return false;
    const auto secs = std::chrono::duration_cast<std::chrono::seconds>(utc.time_since_epoch()).count();

    // Determine year (in UTC); DST window edges are within the same calendar year
    // (or wrap, in the AU case). We check the current year and previous year.
    // To keep it robust, derive the year from the local-standard date.
    const long long local_std_secs = secs + std_offset_seconds_;
    const long long days = local_std_secs >= 0
        ? local_std_secs / 86400
        : -((-local_std_secs + 86399) / 86400);
    int year;
    unsigned month;
    unsigned day;
    unsigned weekday;
    CivilFromDays(days, year, month, day, weekday);
    (void)month;
    (void)day;
    (void)weekday;

    int rs = 0;
    switch (rule_set_) {
    case RuleSet::UsPost2007: rs = 1; break;
    case RuleSet::EuPost1996: rs = 2; break;
    case RuleSet::AuPost2008: rs = 3; break;
    default: return false;
    }

    // For AU (southern hemisphere), the DST "window" starts in October and
    // ends in April of the following year. We need to evaluate against the
    // window that contains the moment in question.
    if (rs == 3) {
        const auto cur = ComputeDstWindow(year, std_offset_seconds_, dst_offset_seconds_, rs);
        const auto prev = ComputeDstWindow(year - 1, std_offset_seconds_, dst_offset_seconds_, rs);
        // In the southern hemisphere model, current-year start = Oct of `year`,
        // current-year end = April of `year`. So the DST that's "active" near
        // April of `year` started in October of `year-1`.
        if (prev && secs < prev->dst_end_utc_sec) {
            return secs >= prev->dst_start_utc_sec;
        }
        if (cur && secs >= cur->dst_start_utc_sec) {
            return true;
        }
        return false;
    }

    const auto window = ComputeDstWindow(year, std_offset_seconds_, dst_offset_seconds_, rs);
    if (!window) return false;
    return secs >= window->dst_start_utc_sec && secs < window->dst_end_utc_sec;
}

LocalTime Timezone::to_local(std::chrono::system_clock::time_point utc) const {
    const long long secs = std::chrono::duration_cast<std::chrono::seconds>(utc.time_since_epoch()).count();
    long long offset = std_offset_seconds_;
    if (is_dst_utc(utc)) {
        offset += dst_offset_seconds_;
    }
    const long long local_secs = secs + offset;
    const long long days = local_secs >= 0
        ? local_secs / 86400
        : -((-local_secs + 86399) / 86400);
    const long long tod = local_secs - days * 86400;

    int year;
    unsigned month;
    unsigned day;
    unsigned weekday;
    CivilFromDays(days, year, month, day, weekday);

    LocalTime out;
    out.year = year;
    out.month = static_cast<int>(month);
    out.day = static_cast<int>(day);
    out.hour = static_cast<int>(tod / kSecondsPerHour);
    out.minute = static_cast<int>((tod % kSecondsPerHour) / kSecondsPerMinute);
    out.second = static_cast<int>(tod % kSecondsPerMinute);
    out.day_of_week = static_cast<int>(weekday);
    return out;
}

std::chrono::system_clock::time_point Timezone::local_to_utc(
    const LocalTime& local,
    DstChoice choice,
    bool* was_gap,
    bool* was_fold) const {
    if (was_gap) *was_gap = false;
    if (was_fold) *was_fold = false;

    const long long local_secs = UtcSecondsFromComponents(
        local.year, static_cast<unsigned>(local.month), static_cast<unsigned>(local.day),
        local.hour, local.minute, local.second);

    if (rule_set_ == RuleSet::Fixed) {
        const long long utc_secs = local_secs - std_offset_seconds_;
        return std::chrono::system_clock::time_point{std::chrono::seconds{utc_secs}};
    }

    int rs = 0;
    switch (rule_set_) {
    case RuleSet::UsPost2007: rs = 1; break;
    case RuleSet::EuPost1996: rs = 2; break;
    case RuleSet::AuPost2008: rs = 3; break;
    default: rs = 0;
    }

    // Two candidate UTC instants: standard-time interpretation and DST
    // interpretation.
    const long long utc_std = local_secs - std_offset_seconds_;
    const long long utc_dst = local_secs - std_offset_seconds_ - dst_offset_seconds_;

    // Determine DST membership of each candidate.
    const auto std_tp = std::chrono::system_clock::time_point{std::chrono::seconds{utc_std}};
    const auto dst_tp = std::chrono::system_clock::time_point{std::chrono::seconds{utc_dst}};
    const bool std_in_dst = is_dst_utc(std_tp);
    const bool dst_in_dst = is_dst_utc(dst_tp);

    // Four cases for (std_in_dst, dst_in_dst):
    //   (false,false): unambiguous standard time  -> utc_std
    //   (true,true):   unambiguous daylight time  -> utc_dst
    //   (false,true):  fall-back fold (both valid) -> earliest = utc_dst
    //                  (because dst interpretation is the EARLIER instant
    //                   during fall-back -- DST ends, so dst-clock applied
    //                   first, then std-clock applied to the same wall time)
    //   (true,false):  spring-forward gap (neither valid) -> skip ahead

    if (!std_in_dst && !dst_in_dst) {
        return std_tp;
    }
    if (std_in_dst && dst_in_dst) {
        return dst_tp;
    }
    if (!std_in_dst && dst_in_dst) {
        // Fall-back: wall-clock time exists twice. The earlier UTC instant
        // is when the wall clock was still on DST. utc_dst is earlier when
        // dst_offset > 0.
        if (was_fold) *was_fold = true;
        if (choice == DstChoice::Earliest) {
            return dst_tp;
        }
        return dst_tp;  // "SkipGap" doesn't really apply to fold; default earliest
    }
    // Spring-forward gap: neither candidate is valid. Advance the wall
    // clock by dst_offset to the first valid instant.
    if (was_gap) *was_gap = true;
    const auto window_year = local.year;
    auto window = ComputeDstWindow(window_year, std_offset_seconds_, dst_offset_seconds_, rs);
    if (!window) {
        // Should not happen for non-fixed zones.
        return std_tp;
    }
    // The first valid instant is the spring-forward boundary itself
    // (in UTC). After spring-forward, wall clock jumps from std_offset to
    // std_offset+dst_offset. The first valid local minute is the start of
    // DST, which corresponds to dst_start_utc_sec.
    return std::chrono::system_clock::time_point{std::chrono::seconds{window->dst_start_utc_sec}};
}

}  // namespace agentos
