#include "autodev/autodev_job_id.hpp"

#include <array>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <random>
#include <regex>
#include <sstream>

namespace agentos {

namespace {

std::tm UtcTime(const std::time_t value) {
    std::tm out{};
#ifdef _WIN32
    gmtime_s(&out, &value);
#else
    gmtime_r(&value, &out);
#endif
    return out;
}

std::string RandomHex6() {
    static constexpr std::array<char, 16> chars = {
        '0', '1', '2', '3', '4', '5', '6', '7',
        '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
    std::random_device device;
    std::uniform_int_distribution<int> dist(0, 15);
    std::string value;
    value.reserve(6);
    for (int i = 0; i < 6; ++i) {
        value.push_back(chars[static_cast<std::size_t>(dist(device))]);
    }
    return value;
}

}  // namespace

std::string GenerateAutoDevJobId() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    const auto utc = UtcTime(time);

    std::ostringstream out;
    out << "autodev-"
        << std::put_time(&utc, "%Y%m%d-%H%M%S")
        << '-' << RandomHex6();
    return out.str();
}

bool IsValidAutoDevJobId(const std::string& job_id) {
    static const std::regex job_id_re(R"(^autodev-[0-9]{8}-[0-9]{6}-[0-9a-f]{6}$)");
    return std::regex_match(job_id, job_id_re);
}

std::string AutoDevJobIdSuffix(const std::string& job_id) {
    if (job_id.size() < 6) {
        return {};
    }
    return job_id.substr(job_id.size() - 6);
}

}  // namespace agentos
