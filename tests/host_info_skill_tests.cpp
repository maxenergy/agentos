#include "skills/builtin/host_info_skill.hpp"

#include <iostream>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

namespace {

int failures = 0;

void Expect(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void TestManifestShape() {
    agentos::HostInfoSkill skill;
    const auto manifest = skill.manifest();
    Expect(manifest.name == "host_info", "manifest name should be host_info");
    Expect(manifest.risk_level == "low", "manifest risk_level should be low");
    Expect(manifest.idempotent, "manifest should mark host_info as idempotent");
    Expect(manifest.permissions.empty(), "host_info should not require any permissions");
}

void TestExecuteReturnsHostnameAndInterfaces() {
    auto skill = std::make_shared<agentos::HostInfoSkill>();

    agentos::SkillCall call;
    call.call_id = "host-info-1";
    call.skill_name = "host_info";

    const auto result = skill->execute(call);

    Expect(result.success, "host_info execute should succeed");
    Expect(result.error_code.empty(), "host_info execute should not produce an error code");
    Expect(!result.json_output.empty(), "host_info execute should return JSON output");

    nlohmann::json parsed;
    bool parse_ok = true;
    try {
        parsed = nlohmann::json::parse(result.json_output);
    } catch (const std::exception&) {
        parse_ok = false;
    }
    Expect(parse_ok, "host_info JSON output should parse");
    if (!parse_ok) {
        return;
    }

    Expect(parsed.contains("hostname"), "output should have a hostname field");
    Expect(parsed["hostname"].is_string(), "hostname should be a string");
    Expect(!parsed["hostname"].get<std::string>().empty(), "hostname should not be empty");

    Expect(parsed.contains("interfaces"), "output should have an interfaces field");
    Expect(parsed["interfaces"].is_array(), "interfaces should be an array");

    for (const auto& entry : parsed["interfaces"]) {
        Expect(entry.contains("name"), "interface entry should have a name");
        Expect(entry["name"].is_string(), "interface name should be a string");
        Expect(entry.contains("ipv4"), "interface entry should have an ipv4 array");
        Expect(entry["ipv4"].is_array(), "interface ipv4 should be an array");
        for (const auto& addr : entry["ipv4"]) {
            Expect(addr.is_string(), "ipv4 address entries should be strings");
            const auto text = addr.get<std::string>();
            Expect(text != "127.0.0.1", "ipv4 address should not be loopback");
            Expect(!text.empty(), "ipv4 address should not be empty");
        }
    }
}

void TestHealthy() {
    agentos::HostInfoSkill skill;
    Expect(skill.healthy(), "host_info should report healthy");
}

}  // namespace

int main() {
    TestManifestShape();
    TestExecuteReturnsHostnameAndInterfaces();
    TestHealthy();

    if (failures != 0) {
        std::cerr << failures << " host_info skill test assertion(s) failed\n";
        return 1;
    }

    std::cout << "agentos_host_info_skill_tests passed\n";
    return 0;
}
