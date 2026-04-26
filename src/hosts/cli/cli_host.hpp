#pragma once

#include "core/models.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace agentos {

struct CliSpec {
    std::string name;
    std::string description;
    std::string binary;
    std::vector<std::string> args_template;
    std::vector<std::string> required_args;
    std::string input_schema_json;
    std::string output_schema_json;
    std::string parse_mode = "text";
    std::string risk_level = "low";
    std::vector<std::string> permissions;
    int timeout_ms = 3000;
    std::size_t output_limit_bytes = 1024 * 1024;
    std::vector<std::string> env_allowlist;
    std::size_t memory_limit_bytes = 0;
    int max_processes = 0;
    int cpu_time_limit_seconds = 0;
    int file_descriptor_limit = 0;
    std::filesystem::path source_file;
    int source_line_number = 0;
};

struct CliRunRequest {
    CliSpec spec;
    StringMap arguments;
    std::filesystem::path workspace_path;
};

struct CliRunResult {
    bool success = false;
    int exit_code = -1;
    bool timed_out = false;
    int duration_ms = 0;
    std::string command_display;
    std::string stdout_text;
    std::string stderr_text;
    std::string error_code;
    std::string error_message;
};

class CliHost {
public:
    CliRunResult run(const CliRunRequest& request) const;

private:
    static std::vector<std::string> RenderArgs(const CliSpec& spec, const StringMap& arguments);
    static std::string RenderTemplateValue(std::string value, const StringMap& arguments);
};

std::string QuoteCommandForDisplay(const std::string& value);

}  // namespace agentos
