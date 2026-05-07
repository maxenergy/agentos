#include "autodev/autodev_skill_pack_loader.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#ifndef _WIN32
#include <sys/wait.h>
#endif

namespace agentos {

namespace {

constexpr std::array<const char*, 8> kRequiredAutoDevSteps = {
    "understand-system",
    "grill-requirements",
    "spec-freeze",
    "impact-analysis",
    "task-slice",
    "goal-pack",
    "verify-loop",
    "goal-review",
};

std::string ShellQuote(const std::string& value) {
#ifdef _WIN32
    std::string quoted = "\"";
    for (const char ch : value) {
        if (ch == '"') {
            quoted += "\\\"";
        } else {
            quoted.push_back(ch);
        }
    }
    quoted.push_back('"');
    return quoted;
#else
    std::string quoted = "'";
    for (const char ch : value) {
        if (ch == '\'') {
            quoted += "'\\''";
        } else {
            quoted.push_back(ch);
        }
    }
    quoted.push_back('\'');
    return quoted;
#endif
}

std::string Trim(std::string value) {
    const auto start = value.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        return {};
    }
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(start, end - start + 1);
}

struct CommandResult {
    int exit_code = -1;
    std::string output;
};

CommandResult RunCommand(const std::string& command) {
#ifdef _WIN32
    FILE* pipe = _popen((command + " 2>&1").c_str(), "r");
#else
    FILE* pipe = popen((command + " 2>&1").c_str(), "r");
#endif
    if (!pipe) {
        return {.exit_code = -1, .output = "failed to start command"};
    }
    std::string output;
    char buffer[4096];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        output += buffer;
    }
#ifdef _WIN32
    const int status = _pclose(pipe);
    return {.exit_code = status, .output = std::move(output)};
#else
    const int status = pclose(pipe);
    if (WIFEXITED(status)) {
        return {.exit_code = WEXITSTATUS(status), .output = std::move(output)};
    }
    return {.exit_code = status, .output = std::move(output)};
#endif
}

std::string GitCommit(const std::filesystem::path& path) {
    const auto result = RunCommand("git -C " + ShellQuote(path.string()) + " rev-parse HEAD");
    if (result.exit_code != 0) {
        return {};
    }
    return Trim(result.output);
}

bool StepExists(const std::filesystem::path& root, const std::string& step) {
    std::error_code ec;
    if (!std::filesystem::exists(root, ec) || !std::filesystem::is_directory(root, ec)) {
        return false;
    }
    for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
        if (ec || !entry.is_directory(ec)) {
            continue;
        }
        const auto dirname = entry.path().filename().string();
        if (dirname == step ||
            (dirname.size() > step.size() && dirname.ends_with("-" + step))) {
            const auto skill_file = entry.path() / "SKILL.md";
            if (std::filesystem::exists(skill_file, ec) && std::filesystem::is_regular_file(skill_file, ec)) {
                return true;
            }
        }
    }
    return false;
}

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

std::string Fnv1a64Hex(const std::string& value) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char ch : value) {
        hash ^= ch;
        hash *= 1099511628211ULL;
    }
    std::ostringstream out;
    out << std::hex << std::setw(16) << std::setfill('0') << hash;
    return out.str();
}

std::string ManifestHash(const std::filesystem::path& root, const std::vector<std::string>& steps) {
    std::vector<std::filesystem::path> files;
    std::error_code ec;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root, ec)) {
        if (ec) {
            break;
        }
        if (entry.is_regular_file(ec) && entry.path().filename() == "SKILL.md") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());

    std::ostringstream manifest;
    for (const auto& step : steps) {
        manifest << "step:" << step << '\n';
    }
    for (const auto& file : files) {
        manifest << "file:" << std::filesystem::relative(file, root, ec).generic_string() << '\n';
        manifest << ReadFile(file) << '\n';
    }
    return Fnv1a64Hex(manifest.str());
}

}  // namespace

AutoDevSkillPackLoadResult AutoDevSkillPackLoader::load_local_path(
    const std::filesystem::path& local_path) const {
    AutoDevSkillPackLoadResult result;
    const auto root = std::filesystem::absolute(local_path).lexically_normal();
    std::error_code ec;
    if (!std::filesystem::exists(root, ec) || !std::filesystem::is_directory(root, ec)) {
        result.error_message = "skill pack path does not exist: " + root.string();
        return result;
    }

    std::vector<std::string> found_steps;
    std::vector<std::string> missing_steps;
    for (const auto* step : kRequiredAutoDevSteps) {
        if (StepExists(root, step)) {
            found_steps.emplace_back(step);
        } else {
            missing_steps.emplace_back(step);
        }
    }
    if (!missing_steps.empty()) {
        std::ostringstream error;
        error << "skill pack missing required AutoDev step(s): ";
        for (std::size_t i = 0; i < missing_steps.size(); ++i) {
            if (i != 0) {
                error << ", ";
            }
            error << missing_steps[i];
        }
        result.error_message = error.str();
        result.required_steps = std::move(found_steps);
        return result;
    }

    result.success = true;
    result.required_steps = std::move(found_steps);
    result.commit = GitCommit(root);
    result.manifest_hash = ManifestHash(root, result.required_steps);
    result.snapshot = nlohmann::json{
        {"name", "maxenergy/skills"},
        {"source_type", "local_path"},
        {"local_path", root.string()},
        {"commit", result.commit.empty() ? nlohmann::json(nullptr) : nlohmann::json(result.commit)},
        {"manifest_hash", result.manifest_hash},
        {"required_steps", result.required_steps},
        {"schema_versions_supported", nlohmann::json::array({"1.0.0"})},
    };
    return result;
}

}  // namespace agentos
