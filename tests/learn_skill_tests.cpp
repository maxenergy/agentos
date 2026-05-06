#include "core/audit/audit_logger.hpp"
#include "core/registry/skill_registry.hpp"
#include "hosts/cli/cli_host.hpp"
#include "hosts/plugin/plugin_host.hpp"
#include "skills/builtin/learn_skill.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

namespace {

int failures = 0;

void Expect(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

class StaticSkill final : public agentos::ISkillAdapter {
public:
    explicit StaticSkill(std::string name) : name_(std::move(name)) {}

    agentos::SkillManifest manifest() const override {
        return {
            .name = name_,
            .version = "1.0.0",
            .description = "test skill",
            .capabilities = {"test"},
            .input_schema_json = R"({"type":"object"})",
            .output_schema_json = R"({"type":"object"})",
            .risk_level = "low",
            .permissions = {},
        };
    }

    agentos::SkillResult execute(const agentos::SkillCall&) override {
        return {.success = true, .json_output = R"({})"};
    }

    bool healthy() const override { return true; }

private:
    std::string name_;
};

std::filesystem::path FreshWorkspace(const std::string& tag) {
    const auto workspace = std::filesystem::temp_directory_path() / ("agentos_learn_skill_tests_" + tag);
    std::filesystem::remove_all(workspace);
    std::filesystem::create_directories(workspace);
    return workspace;
}

struct Fixture {
    std::filesystem::path workspace;
    agentos::SkillRegistry skill_registry;
    agentos::CliHost cli_host;
    agentos::PluginHost plugin_host;
    agentos::AuditLogger audit_logger;
    std::shared_ptr<agentos::LearnSkill> skill;

    explicit Fixture(const std::string& tag)
        : workspace(FreshWorkspace(tag)),
          plugin_host(cli_host),
          audit_logger(workspace / "audit.log"),
          skill(std::make_shared<agentos::LearnSkill>(
              skill_registry, cli_host, plugin_host, audit_logger, workspace)) {}
};

agentos::SkillCall MakeCall(const std::string& call_id) {
    agentos::SkillCall call;
    call.call_id = call_id;
    call.skill_name = "learn_skill";
    return call;
}

void TestManifestShape() {
    Fixture fx("manifest");
    const auto manifest = fx.skill->manifest();
    Expect(manifest.name == "learn_skill", "manifest name should be learn_skill");
    Expect(manifest.risk_level == "medium", "manifest risk_level should be medium");
    Expect(!manifest.idempotent, "learn_skill should not be idempotent");
    bool has_filesystem_write = false;
    for (const auto& permission : manifest.permissions) {
        if (permission == "filesystem.write") {
            has_filesystem_write = true;
        }
    }
    Expect(has_filesystem_write, "learn_skill should require filesystem.write");
}

void TestHappyPathRegistersSkill() {
    Fixture fx("happy");

    auto call = MakeCall("learn-1");
    call.arguments = {
        {"name", "echo_test"},
        {"binary", "echo"},
        {"args_template", "{{message}}"},
        {"required_args", "message"},
    };

    const auto result = fx.skill->execute(call);

    Expect(result.success, "happy-path execute should succeed (error=" + result.error_message + ")");
    Expect(result.error_code.empty(), "happy-path execute should not produce an error code");

    const auto spec_path = fx.workspace / "runtime" / "cli_specs" / "learned" / "echo_test.tsv";
    Expect(std::filesystem::exists(spec_path), "spec file should exist on disk");

    std::ifstream input(spec_path, std::ios::binary);
    std::string line;
    std::getline(input, line);
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }

    std::vector<std::string> fields;
    {
        std::stringstream ss(line);
        std::string field;
        while (std::getline(ss, field, '\t')) {
            fields.push_back(field);
        }
    }
    Expect(fields.size() >= 12, "spec line should have at least 12 columns");
    if (fields.size() >= 12) {
        Expect(fields[0] == "echo_test", "column 0 should be name");
        Expect(fields[2] == "echo", "column 2 should be binary");
        Expect(fields[3] == "{{message}}", "column 3 should be args_template");
        Expect(fields[4] == "message", "column 4 should be required_args");
        Expect(fields[5] == "text", "column 5 should be parse_mode=text");
        Expect(fields[6] == "medium", "column 6 should be default risk_level=medium");
        Expect(fields[7] == "process.spawn", "column 7 should declare process.spawn permission");
        Expect(fields[8] == "30000", "column 8 should be default timeout_ms=30000");
        Expect(fields[11] == "131072", "column 11 should be output_limit_bytes=131072");
    }

    const auto registered = fx.skill_registry.find("echo_test");
    Expect(registered != nullptr, "skill_registry should resolve echo_test after learn_skill");

    nlohmann::json parsed;
    bool parsed_ok = true;
    try {
        parsed = nlohmann::json::parse(result.json_output);
    } catch (const std::exception&) {
        parsed_ok = false;
    }
    Expect(parsed_ok, "json_output should parse");
    if (parsed_ok) {
        Expect(parsed.value("registered", false), "json_output.registered should be true");
        Expect(parsed.value("skill_name", std::string{}) == "echo_test",
               "json_output.skill_name should be echo_test");
        const auto reported_path = parsed.value("spec_path", std::string{});
        Expect(reported_path == "runtime/cli_specs/learned/echo_test.tsv",
               "json_output.spec_path should be the relative spec path");
    }
}

void TestProtectedNameRejected() {
    Fixture fx("protected");

    auto call = MakeCall("learn-2");
    call.arguments = {
        {"name", "file_read"},
        {"binary", "cat"},
        {"args_template", "{{path}}"},
    };

    const auto result = fx.skill->execute(call);
    Expect(!result.success, "execute should fail for protected name");
    Expect(result.error_code == "SchemaValidationFailed",
           "error_code should be SchemaValidationFailed (got=" + result.error_code + ")");
}

void TestRegisteredNameRejected() {
    Fixture fx("registered-conflict");
    fx.skill_registry.register_skill(std::make_shared<StaticSkill>("already_registered"));

    auto call = MakeCall("learn-registered-conflict");
    call.arguments = {
        {"name", "already_registered"},
        {"binary", "echo"},
        {"args_template", "{{message}}"},
    };

    const auto result = fx.skill->execute(call);
    Expect(!result.success, "execute should fail for already registered names");
    Expect(result.error_code == "SchemaValidationFailed",
           "registered-name conflict should return SchemaValidationFailed (got=" + result.error_code + ")");
    Expect(!std::filesystem::exists(
               fx.workspace / "runtime" / "cli_specs" / "learned" / "already_registered.tsv"),
           "registered-name conflict should not write a learned spec");
}

void TestBadNameRegexRejected() {
    Fixture fx("badname");

    auto call = MakeCall("learn-3");
    call.arguments = {
        {"name", "bad name with spaces"},
        {"binary", "echo"},
        {"args_template", "{{message}}"},
    };

    const auto result = fx.skill->execute(call);
    Expect(!result.success, "execute should fail for invalid name regex");
    Expect(result.error_code == "SchemaValidationFailed",
           "error_code should be SchemaValidationFailed (got=" + result.error_code + ")");
}

void TestMalformedArgsRejected() {
    {
        Fixture fx("missing-binary");
        auto call = MakeCall("learn-missing-binary");
        call.arguments = {
            {"name", "missing_binary"},
            {"args_template", "{{message}}"},
        };

        const auto result = fx.skill->execute(call);
        Expect(!result.success, "execute should fail when binary is missing");
        Expect(result.error_code == "SchemaValidationFailed",
               "missing binary should return SchemaValidationFailed (got=" + result.error_code + ")");
    }

    {
        Fixture fx("bad-timeout");
        auto call = MakeCall("learn-bad-timeout");
        call.arguments = {
            {"name", "bad_timeout"},
            {"binary", "echo"},
            {"args_template", "{{message}}"},
            {"timeout_ms", "not-an-int"},
        };

        const auto result = fx.skill->execute(call);
        Expect(!result.success, "execute should fail when timeout_ms is malformed");
        Expect(result.error_code == "SchemaValidationFailed",
               "malformed timeout_ms should return SchemaValidationFailed (got=" + result.error_code + ")");
    }
}

}  // namespace

int main() {
    TestManifestShape();
    TestHappyPathRegistersSkill();
    TestProtectedNameRejected();
    TestRegisteredNameRejected();
    TestBadNameRegexRejected();
    TestMalformedArgsRejected();

    if (failures != 0) {
        std::cerr << failures << " learn_skill test assertion(s) failed\n";
        return 1;
    }

    std::cout << "agentos_learn_skill_tests passed\n";
    return 0;
}
