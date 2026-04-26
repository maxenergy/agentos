#include "cli/cli_specs_commands.hpp"

#include "hosts/cli/cli_spec_loader.hpp"

#include <algorithm>
#include <iostream>

namespace agentos {

namespace {

void PrintSpecDiagnostic(
    const std::string& label,
    const std::filesystem::path& file,
    const int line_number,
    const std::string& reason) {
    std::cout
        << label
        << " file=" << file.string()
        << " line=" << line_number
        << " reason=" << reason
        << '\n';
}

std::string CliSpecConflictReason(const std::string& name) {
    return "external CLI spec name conflicts with already registered skill: " + name;
}

bool PrintCliSpecs(const std::filesystem::path& workspace, const std::set<std::string>& conflict_names) {
    const auto loaded = LoadCliSpecsWithDiagnostics(workspace / "runtime" / "cli_specs");
    for (const auto& spec : loaded.specs) {
        const bool conflicts_with_registered_skill = conflict_names.contains(spec.name);
        std::cout
            << spec.name
            << " parse_mode=" << spec.parse_mode
            << " binary=" << spec.binary
            << " valid=" << (conflicts_with_registered_skill ? "false" : "true");
        if (conflicts_with_registered_skill) {
            std::cout << " reason=" << CliSpecConflictReason(spec.name);
        }
        std::cout << '\n';
        if (conflicts_with_registered_skill) {
            PrintSpecDiagnostic(
                "conflicting_cli_spec",
                spec.source_file,
                spec.source_line_number,
                CliSpecConflictReason(spec.name));
        }
    }
    for (const auto& diagnostic : loaded.diagnostics) {
        PrintSpecDiagnostic("skipped_cli_spec", diagnostic.file, diagnostic.line_number, diagnostic.reason);
    }
    const bool no_conflicts = std::none_of(loaded.specs.begin(), loaded.specs.end(), [&](const CliSpec& spec) {
        return conflict_names.contains(spec.name);
    });
    return loaded.diagnostics.empty() && no_conflicts;
}

}  // namespace

int RunCliSpecsCommand(
    const std::filesystem::path& workspace,
    const std::set<std::string>& conflict_names,
    const int argc,
    char* argv[]) {
    const bool valid = PrintCliSpecs(workspace, conflict_names);
    if (argc >= 3 && std::string(argv[2]) == "validate") {
        return valid ? 0 : 1;
    }
    return 0;
}

}  // namespace agentos
