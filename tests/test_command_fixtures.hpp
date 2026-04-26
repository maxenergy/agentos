#pragma once

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <utility>

namespace agentos::test {

inline void SetEnvForTest(const std::string& name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name.c_str(), value.c_str());
#else
    setenv(name.c_str(), value.c_str(), 1);
#endif
}

inline void ClearEnvForTest(const std::string& name) {
#ifdef _WIN32
    _putenv_s(name.c_str(), "");
#else
    unsetenv(name.c_str());
#endif
}

inline std::optional<std::string> ReadEnvForTest(const std::string& name) {
#ifdef _WIN32
    char* raw_value = nullptr;
    std::size_t value_size = 0;
    if (_dupenv_s(&raw_value, &value_size, name.c_str()) != 0 || raw_value == nullptr) {
        return std::nullopt;
    }

    std::string value(raw_value, value_size > 0 ? value_size - 1 : 0);
    std::free(raw_value);
    return value;
#else
    const char* raw_value = std::getenv(name.c_str());
    if (!raw_value) {
        return std::nullopt;
    }
    return std::string(raw_value);
#endif
}

class ScopedEnvOverride {
public:
    ScopedEnvOverride(std::string name, std::string value)
        : name_(std::move(name)),
          old_value_(ReadEnvForTest(name_)) {
        SetEnvForTest(name_, value);
    }

    ScopedEnvOverride(const ScopedEnvOverride&) = delete;
    ScopedEnvOverride& operator=(const ScopedEnvOverride&) = delete;

    ~ScopedEnvOverride() {
        if (old_value_.has_value()) {
            SetEnvForTest(name_, *old_value_);
        } else {
            ClearEnvForTest(name_);
        }
    }

private:
    std::string name_;
    std::optional<std::string> old_value_;
};

inline char PathListSeparatorForTest() {
#ifdef _WIN32
    return ';';
#else
    return ':';
#endif
}

inline std::string PrependPathForTest(const std::filesystem::path& bin_dir) {
    return bin_dir.string() + PathListSeparatorForTest() + ReadEnvForTest("PATH").value_or("");
}

inline std::filesystem::path WriteCliFixture(
    const std::filesystem::path& bin_dir,
    const std::string& command_name,
    const std::string& script_body) {
#ifdef _WIN32
    const auto fixture_path = bin_dir / (command_name + ".cmd");
#else
    const auto fixture_path = bin_dir / command_name;
#endif

    std::ofstream output(fixture_path, std::ios::binary);
    output << script_body;
    output.close();

#ifndef _WIN32
    std::filesystem::permissions(
        fixture_path,
        std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec | std::filesystem::perms::others_exec,
        std::filesystem::perm_options::add);
#endif

    return fixture_path;
}

inline void WriteCodexCliFixture(const std::filesystem::path& bin_dir, const bool logged_in) {
#ifdef _WIN32
    const auto body = logged_in
        ? "@echo off\n"
          "if \"%1\"==\"login\" if \"%2\"==\"status\" (\n"
          "  echo Logged in as fixture-user\n"
          "  exit /b 0\n"
          ")\n"
          "echo unexpected codex args %*\n"
          "exit /b 2\n"
        : "@echo off\n"
          "if \"%1\"==\"login\" if \"%2\"==\"status\" (\n"
          "  echo Not logged in\n"
          "  exit /b 0\n"
          ")\n"
          "exit /b 2\n";
#else
    const auto body = logged_in
        ? "#!/bin/sh\n"
          "if [ \"$1\" = \"login\" ] && [ \"$2\" = \"status\" ]; then\n"
          "  printf '%s\\n' 'Logged in as fixture-user'\n"
          "  exit 0\n"
          "fi\n"
          "printf '%s\\n' \"unexpected codex args $*\"\n"
          "exit 2\n"
        : "#!/bin/sh\n"
          "if [ \"$1\" = \"login\" ] && [ \"$2\" = \"status\" ]; then\n"
          "  printf '%s\\n' 'Not logged in'\n"
          "  exit 0\n"
          "fi\n"
          "exit 2\n";
#endif
    (void)WriteCliFixture(bin_dir, "codex", body);
}

inline void WriteClaudeCliFixture(const std::filesystem::path& bin_dir) {
#ifdef _WIN32
    const auto body =
        "@echo off\n"
        "if \"%1\"==\"auth\" if \"%2\"==\"status\" (\n"
        "  echo {\"loggedIn\": true}\n"
        "  exit /b 0\n"
        ")\n"
        "echo unexpected claude args %*\n"
        "exit /b 2\n";
#else
    const auto body =
        "#!/bin/sh\n"
        "if [ \"$1\" = \"auth\" ] && [ \"$2\" = \"status\" ]; then\n"
        "  printf '%s\\n' '{\"loggedIn\": true}'\n"
        "  exit 0\n"
        "fi\n"
        "printf '%s\\n' \"unexpected claude args $*\"\n"
        "exit 2\n";
#endif
    (void)WriteCliFixture(bin_dir, "claude", body);
}

inline void WriteJqCliFixture(const std::filesystem::path& bin_dir) {
#ifdef _WIN32
    const auto body =
        "@echo off\n"
        "if \"%1\"==\"-c\" (\n"
        "  echo {\"fixture\":\"jq\",\"filter\":\"%2\"}\n"
        "  exit /b 0\n"
        ")\n"
        "echo unexpected jq args %*\n"
        "exit /b 2\n";
#else
    const auto body =
        "#!/bin/sh\n"
        "if [ \"$1\" = \"-c\" ]; then\n"
        "  printf '{\"fixture\":\"jq\",\"filter\":\"%s\"}\\n' \"$2\"\n"
        "  exit 0\n"
        "fi\n"
        "printf '%s\\n' \"unexpected jq args $*\"\n"
        "exit 2\n";
#endif
    (void)WriteCliFixture(bin_dir, "jq", body);
}

inline void WriteGeminiCliFixture(const std::filesystem::path& bin_dir) {
#ifdef _WIN32
    const auto body =
        "@echo off\n"
        "if \"%1\"==\"-p\" (\n"
        "  echo gemini cli response\n"
        "  exit /b 0\n"
        ")\n"
        "echo unexpected gemini args %*\n"
        "exit /b 2\n";
#else
    const auto body =
        "#!/usr/bin/env sh\n"
        "if [ \"$1\" = \"-p\" ]; then\n"
        "  printf '%s\\n' 'gemini cli response'\n"
        "  exit 0\n"
        "fi\n"
        "printf '%s\\n' \"unexpected gemini args $*\"\n"
        "exit 2\n";
#endif
    WriteCliFixture(bin_dir, "gemini", body);
}

inline void WriteGcloudCliFixture(const std::filesystem::path& bin_dir, const std::string& access_token = "fixture-adc-token") {
#ifdef _WIN32
    const auto body =
        "@echo off\n"
        "if \"%1\"==\"auth\" if \"%2\"==\"application-default\" if \"%3\"==\"print-access-token\" (\n"
        "  echo " + access_token + "\n"
        "  exit /b 0\n"
        ")\n"
        "echo unexpected gcloud args %*\n"
        "exit /b 2\n";
#else
    const auto body =
        "#!/usr/bin/env sh\n"
        "if [ \"$1\" = \"auth\" ] && [ \"$2\" = \"application-default\" ] && [ \"$3\" = \"print-access-token\" ]; then\n"
        "  printf '%s\\n' '" + access_token + "'\n"
        "  exit 0\n"
        "fi\n"
        "printf '%s\\n' \"unexpected gcloud args $*\"\n"
        "exit 2\n";
#endif
    WriteCliFixture(bin_dir, "gcloud", body);
}

}  // namespace agentos::test
