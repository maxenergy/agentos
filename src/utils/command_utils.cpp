#include "utils/command_utils.hpp"

#include <cstdlib>
#include <optional>
#include <sstream>
#include <vector>

namespace agentos {

namespace {

#ifdef _WIN32
constexpr char kPathSeparator = ';';
#else
constexpr char kPathSeparator = ':';
#endif

std::vector<std::string> Split(const std::string& value, const char separator) {
    std::vector<std::string> parts;
    std::stringstream stream(value);
    std::string part;

    while (std::getline(stream, part, separator)) {
        if (!part.empty()) {
            parts.push_back(std::move(part));
        }
    }

    return parts;
}

std::optional<std::string> GetEnv(const char* name);

std::vector<std::string> CandidateExtensions(const std::filesystem::path& command) {
#ifdef _WIN32
    if (command.has_extension()) {
        return {""};
    }

    const auto pathext_value = GetEnv("PATHEXT");
    auto extensions = Split(pathext_value.value_or(".COM;.EXE;.BAT;.CMD"), ';');
    extensions.push_back("");
    return extensions;
#else
    (void)command;
    return {""};
#endif
}

std::optional<std::string> GetEnv(const char* name) {
#ifdef _WIN32
    char* raw_value = nullptr;
    std::size_t value_size = 0;
    if (_dupenv_s(&raw_value, &value_size, name) != 0 || raw_value == nullptr) {
        return std::nullopt;
    }

    std::string value(raw_value, value_size > 0 ? value_size - 1 : 0);
    std::free(raw_value);
    return value;
#else
    const char* raw_value = std::getenv(name);
    if (!raw_value) {
        return std::nullopt;
    }
    return std::string(raw_value);
#endif
}

}  // namespace

std::optional<std::filesystem::path> ResolveCommandPath(const std::string& command) {
    if (command.empty()) {
        return std::nullopt;
    }

    const std::filesystem::path command_path(command);
    if (command_path.has_parent_path()) {
        if (std::filesystem::exists(command_path)) {
            return std::filesystem::absolute(command_path).lexically_normal();
        }
        return std::nullopt;
    }

    const auto path_value = GetEnv("PATH");
    if (!path_value.has_value()) {
        return std::nullopt;
    }

    for (const auto& directory : Split(*path_value, kPathSeparator)) {
        for (const auto& extension : CandidateExtensions(command_path)) {
            auto candidate = std::filesystem::path(directory) / (command + extension);
            if (std::filesystem::exists(candidate)) {
                return std::filesystem::absolute(candidate).lexically_normal();
            }
        }
    }

    return std::nullopt;
}

bool CommandExists(const std::string& command) {
    return ResolveCommandPath(command).has_value();
}

}  // namespace agentos
