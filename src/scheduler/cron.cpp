#include "scheduler/cron.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <sstream>
#include <string>
#include <vector>

namespace agentos {

namespace {

std::vector<std::string> SplitWhitespace(const std::string& input) {
    std::vector<std::string> parts;
    std::string current;
    for (const char ch : input) {
        if (std::isspace(static_cast<unsigned char>(ch))) {
            if (!current.empty()) {
                parts.push_back(std::move(current));
                current.clear();
            }
        } else {
            current.push_back(ch);
        }
    }
    if (!current.empty()) parts.push_back(std::move(current));
    return parts;
}

std::vector<std::string> SplitComma(const std::string& input) {
    std::vector<std::string> parts;
    std::string current;
    for (const char ch : input) {
        if (ch == ',') {
            parts.push_back(std::move(current));
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    parts.push_back(std::move(current));
    return parts;
}

bool ParseInt(const std::string& s, int& out) {
    if (s.empty()) return false;
    out = 0;
    for (const char ch : s) {
        if (!std::isdigit(static_cast<unsigned char>(ch))) return false;
        out = out * 10 + (ch - '0');
        if (out > 100000) return false;
    }
    return true;
}

// Parse a single field (minute|hour|dom|month|dow) into a vector<bool>
// of size (max-min+1). Returns false on parse error.
bool ParseField(const std::string& field, int min_val, int max_val,
                std::vector<bool>& out, bool& restricted) {
    const int span = max_val - min_val + 1;
    out.assign(span, false);
    restricted = (field != "*");

    for (const auto& token : SplitComma(field)) {
        if (token.empty()) return false;

        // Find optional /step.
        std::size_t slash = token.find('/');
        std::string range_part = slash == std::string::npos ? token : token.substr(0, slash);
        std::string step_part = slash == std::string::npos ? "" : token.substr(slash + 1);

        int step = 1;
        if (!step_part.empty()) {
            if (!ParseInt(step_part, step) || step <= 0) return false;
        }

        int lo = min_val;
        int hi = max_val;
        if (range_part == "*") {
            // Full range; step applies from min.
        } else {
            const auto dash = range_part.find('-');
            if (dash == std::string::npos) {
                int single = 0;
                if (!ParseInt(range_part, single)) return false;
                lo = hi = single;
                if (slash != std::string::npos) {
                    // "N/S" means from N to max stepping by S (cron extension).
                    hi = max_val;
                }
            } else {
                int range_lo = 0;
                int range_hi = 0;
                if (!ParseInt(range_part.substr(0, dash), range_lo)) return false;
                if (!ParseInt(range_part.substr(dash + 1), range_hi)) return false;
                lo = range_lo;
                hi = range_hi;
            }
        }
        if (lo < min_val || hi > max_val || lo > hi) return false;

        for (int v = lo; v <= hi; v += step) {
            out[v - min_val] = true;
        }
    }
    return true;
}

void ApplyField(std::bitset<60>& bs, const std::vector<bool>& src) {
    for (std::size_t i = 0; i < src.size(); ++i) bs.set(i, src[i]);
}
void ApplyField(std::bitset<24>& bs, const std::vector<bool>& src) {
    for (std::size_t i = 0; i < src.size(); ++i) bs.set(i, src[i]);
}
void ApplyFieldOffset1(std::bitset<32>& bs, const std::vector<bool>& src) {
    bs.reset();
    for (std::size_t i = 0; i < src.size(); ++i) bs.set(i + 1, src[i]);
}
void ApplyFieldOffset1(std::bitset<13>& bs, const std::vector<bool>& src) {
    bs.reset();
    for (std::size_t i = 0; i < src.size(); ++i) bs.set(i + 1, src[i]);
}
void ApplyField(std::bitset<7>& bs, const std::vector<bool>& src) {
    for (std::size_t i = 0; i < src.size() && i < 7; ++i) bs.set(i, src[i]);
}

bool ExpandAlias(const std::string& token, std::vector<std::string>& fields) {
    if (token == "@hourly")    { fields = {"0", "*", "*", "*", "*"}; return true; }
    if (token == "@daily" || token == "@midnight") {
        fields = {"0", "0", "*", "*", "*"}; return true;
    }
    if (token == "@noon")      { fields = {"0", "12", "*", "*", "*"}; return true; }
    if (token == "@weekly")    { fields = {"0", "0", "*", "*", "0"}; return true; }
    if (token == "@monthly")   { fields = {"0", "0", "1", "*", "*"}; return true; }
    if (token == "@yearly" || token == "@annually") {
        fields = {"0", "0", "1", "1", "*"}; return true;
    }
    return false;
}

// Day-of-week from civil date. Returns 0=Sun..6=Sat.
unsigned DayOfWeek(int year, unsigned month, unsigned day) {
    // Sakamoto's algorithm.
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    int y = year - (month < 3 ? 1 : 0);
    return static_cast<unsigned>((y + y / 4 - y / 100 + y / 400 + t[month - 1] + static_cast<int>(day)) % 7);
}

unsigned DaysInMonth(int year, unsigned month) {
    static const unsigned dim[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2) {
        const bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
        return leap ? 29U : 28U;
    }
    return dim[month - 1];
}

}  // namespace

std::optional<CronExpression> CronExpression::Parse(const std::string& expr) {
    auto tokens = SplitWhitespace(expr);
    if (tokens.empty()) return std::nullopt;

    std::vector<std::string> fields;
    if (tokens.size() == 1 && !tokens[0].empty() && tokens[0][0] == '@') {
        if (!ExpandAlias(tokens[0], fields)) return std::nullopt;
    } else if (tokens.size() == 5) {
        fields = std::move(tokens);
    } else {
        return std::nullopt;
    }

    CronExpression cron;
    cron.source_ = expr;

    std::vector<bool> minute_bits, hour_bits, dom_bits, month_bits, dow_bits;
    bool minute_restricted = false;
    bool hour_restricted = false;
    bool month_restricted = false;
    if (!ParseField(fields[0], 0, 59, minute_bits, minute_restricted)) return std::nullopt;
    if (!ParseField(fields[1], 0, 23, hour_bits, hour_restricted)) return std::nullopt;
    if (!ParseField(fields[2], 1, 31, dom_bits, cron.dom_restricted_)) return std::nullopt;
    if (!ParseField(fields[3], 1, 12, month_bits, month_restricted)) return std::nullopt;
    (void)minute_restricted;
    (void)hour_restricted;
    (void)month_restricted;
    // dow accepts 7 as Sunday alias.
    std::string dow_normalized;
    dow_normalized.reserve(fields[4].size());
    for (const char ch : fields[4]) {
        if (ch == '7') dow_normalized.push_back('0');
        else dow_normalized.push_back(ch);
    }
    if (!ParseField(dow_normalized, 0, 6, dow_bits, cron.dow_restricted_)) return std::nullopt;

    ApplyField(cron.minute_, minute_bits);
    ApplyField(cron.hour_, hour_bits);
    ApplyFieldOffset1(cron.dom_, dom_bits);
    ApplyFieldOffset1(cron.month_, month_bits);
    ApplyField(cron.dow_, dow_bits);

    if (cron.minute_.none() || cron.hour_.none() || cron.dom_.none() ||
        cron.month_.none() || cron.dow_.none()) {
        return std::nullopt;
    }
    return cron;
}

std::optional<std::chrono::system_clock::time_point>
CronExpression::next_after(std::chrono::system_clock::time_point after, const Timezone& tz) const {
    // Strategy: increment by one minute (in local wall clock) starting from
    // `after + 1 minute`, and walk forward in local time. For each candidate
    // minute, check field membership. Convert to UTC; if the conversion
    // reports a gap (spring-forward), the timezone returns the first valid
    // instant after the gap, which is precisely the desired behaviour. If
    // the wall time is in a fold (fall-back), we want the FIRST occurrence
    // only -- we detect this by tracking whether the previous candidate
    // shared the same wall-clock hour and we already advanced from a fold.
    //
    // To ensure fall-back fires only once: we keep a tracker of the last
    // emitted UTC instant; we only emit if the new UTC > previous UTC. The
    // caller invokes us with `after` set to the prior emit, so the second
    // (later UTC) occurrence of 01:30 will be rejected because we already
    // emitted at the earlier UTC.
    //
    // For computing the strict-after-`after` case: we start from the local
    // wall clock corresponding to `after + 1 second` (round up to next
    // minute), and never emit anything <= after.

    auto local = tz.to_local(after);
    // Round up to the next minute boundary in local time.
    if (local.second != 0) {
        local.minute += 1;
        local.second = 0;
    } else {
        local.minute += 1;
    }
    // Normalize overflow.
    auto normalize = [](LocalTime& lt) {
        while (lt.minute >= 60) { lt.minute -= 60; lt.hour += 1; }
        while (lt.hour >= 24) { lt.hour -= 24; lt.day += 1; }
        // Re-normalize day/month/year iteratively.
        while (true) {
            const auto dim = DaysInMonth(lt.year, static_cast<unsigned>(lt.month));
            if (lt.day <= static_cast<int>(dim)) break;
            lt.day -= static_cast<int>(dim);
            lt.month += 1;
            if (lt.month > 12) { lt.month = 1; lt.year += 1; }
        }
    };
    normalize(local);

    // Search horizon: 8 years.
    constexpr int kMaxYears = 8;
    const int horizon_year = local.year + kMaxYears;

    while (local.year <= horizon_year) {
        // Month match?
        if (!month_.test(static_cast<std::size_t>(local.month))) {
            // Advance to the start of the next month.
            local.month += 1;
            local.day = 1;
            local.hour = 0;
            local.minute = 0;
            if (local.month > 12) {
                local.month = 1;
                local.year += 1;
            }
            continue;
        }
        // Day of month range check.
        const auto dim = DaysInMonth(local.year, static_cast<unsigned>(local.month));
        if (local.day > static_cast<int>(dim)) {
            local.month += 1;
            local.day = 1;
            local.hour = 0;
            local.minute = 0;
            if (local.month > 12) {
                local.month = 1;
                local.year += 1;
            }
            continue;
        }
        const auto dow = DayOfWeek(local.year, static_cast<unsigned>(local.month),
                                   static_cast<unsigned>(local.day));
        // Day match (DOM OR DOW classic semantics).
        const bool dom_match = dom_.test(static_cast<std::size_t>(local.day));
        const bool dow_match = dow_.test(static_cast<std::size_t>(dow));
        bool day_match = false;
        if (dom_restricted_ && dow_restricted_) {
            day_match = dom_match || dow_match;
        } else if (dom_restricted_) {
            day_match = dom_match;
        } else if (dow_restricted_) {
            day_match = dow_match;
        } else {
            day_match = true;
        }
        if (!day_match) {
            // Advance to next day at 00:00.
            local.day += 1;
            local.hour = 0;
            local.minute = 0;
            normalize(local);
            continue;
        }
        if (!hour_.test(static_cast<std::size_t>(local.hour))) {
            local.hour += 1;
            local.minute = 0;
            normalize(local);
            continue;
        }
        if (!minute_.test(static_cast<std::size_t>(local.minute))) {
            local.minute += 1;
            normalize(local);
            continue;
        }

        // Match. Convert to UTC, handling DST gap/fold.
        bool was_gap = false;
        bool was_fold = false;
        const auto utc = tz.local_to_utc(local, DstChoice::Earliest, &was_gap, &was_fold);
        if (utc <= after) {
            // The candidate landed at or before `after`. This can happen
            // when a fall-back fold maps the local time to a UTC that is
            // earlier or equal to the threshold; advance.
            local.minute += 1;
            normalize(local);
            continue;
        }
        return utc;
    }
    return std::nullopt;
}

}  // namespace agentos
