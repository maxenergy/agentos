#include "core/schema/schema_validator.hpp"
#include "utils/cancellation.hpp"
#include "utils/spec_parsing.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

int failures = 0;

void Expect(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void TestSplitHelpers() {
    const auto fields = agentos::SplitTsvFields("a\t\tc\t");
    Expect(fields.size() == 4, "SplitTsvFields should preserve empty fields and trailing empty fields");
    Expect(fields[0] == "a", "SplitTsvFields should parse first field");
    Expect(fields[1].empty(), "SplitTsvFields should preserve empty middle fields");
    Expect(fields[2] == "c", "SplitTsvFields should parse later fields");
    Expect(fields[3].empty(), "SplitTsvFields should preserve trailing empty fields");

    const auto list = agentos::SplitNonEmpty("a,,b,", ',');
    Expect(list.size() == 2, "SplitNonEmpty should drop empty list entries");
    Expect(list[0] == "a" && list[1] == "b", "SplitNonEmpty should preserve non-empty entries");
}

void TestStrictNumberParsing() {
    int parsed_int = 0;
    Expect(agentos::ParseStrictInt("42", parsed_int) && parsed_int == 42,
        "ParseStrictInt should parse integers");
    Expect(agentos::ParseStrictInt("-7", parsed_int) && parsed_int == -7,
        "ParseStrictInt should parse negative integers for callers that allow them");
    Expect(!agentos::ParseStrictInt("42ms", parsed_int),
        "ParseStrictInt should reject partially parsed values");

    std::size_t parsed_size = 0;
    Expect(agentos::ParseStrictSize("4096", parsed_size) && parsed_size == 4096,
        "ParseStrictSize should parse unsigned sizes");
    Expect(!agentos::ParseStrictSize("-1", parsed_size),
        "ParseStrictSize should reject negative values");
    Expect(!agentos::ParseStrictSize("1kb", parsed_size),
        "ParseStrictSize should reject partially parsed values");
}

void TestJsonObjectShape() {
    Expect(agentos::IsLikelyJsonObjectString(R"({"type":"object","nested":{"ok":true}})"),
        "IsLikelyJsonObjectString should accept balanced object strings");
    Expect(agentos::IsLikelyJsonObjectString("  {\"brace\":\"}\"}  "),
        "IsLikelyJsonObjectString should ignore braces inside strings");
    Expect(!agentos::IsLikelyJsonObjectString("[1,2,3]"),
        "IsLikelyJsonObjectString should reject non-object JSON values");
    Expect(!agentos::IsLikelyJsonObjectString("{"),
        "IsLikelyJsonObjectString should reject unbalanced objects");
    Expect(!agentos::IsLikelyJsonObjectString(R"({"unterminated":"value})"),
        "IsLikelyJsonObjectString should reject unterminated strings");
}

void TestJoinStrings() {
    Expect(agentos::JoinStrings({"a", "b", "c"}) == "a,b,c",
        "JoinStrings should default to comma separators");
    Expect(agentos::JoinStrings({"a", "b"}, '|') == "a|b",
        "JoinStrings should use caller-provided separators");
}

void TestCancellationToken() {
    agentos::CancellationToken token;
    Expect(!token.is_cancelled(), "CancellationToken should start uncancelled");

    Expect(!token.wait_for_cancel(std::chrono::milliseconds(10)),
        "wait_for_cancel should return false on timeout when not cancelled");

    token.cancel();
    Expect(token.is_cancelled(), "CancellationToken must report cancelled after cancel()");
    Expect(token.wait_for_cancel(std::chrono::milliseconds(0)),
        "wait_for_cancel should return true immediately when already cancelled");

    token.cancel();
    Expect(token.is_cancelled(), "CancellationToken cancel() must be idempotent");

    agentos::CancellationToken async_token;
    std::atomic<bool> wait_returned{false};
    std::thread waiter([&] {
        wait_returned.store(async_token.wait_for_cancel(std::chrono::seconds(5)));
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    async_token.cancel();
    waiter.join();
    Expect(wait_returned.load(),
        "wait_for_cancel must wake up promptly when cancel() is called from another thread");
}

agentos::SkillManifest BaseCapabilityManifest() {
    return {
        .name = "contract_probe",
        .version = "test",
        .description = "Capability Contract test manifest.",
        .capabilities = {"test"},
        .input_schema_json = R"({"type":"object"})",
        .output_schema_json = R"({"type":"object"})",
        .risk_level = "low",
        .permissions = {},
        .supports_streaming = false,
        .idempotent = true,
        .timeout_ms = 1000,
    };
}

void TestCapabilityContractJsonObjectFacade() {
    const auto required = agentos::ValidateCapabilityContractJsonObject(
        R"({"type":"object","required":["path"]})",
        R"({})",
        "skill input");
    Expect(!required.valid, "Capability Contract facade should reject missing required fields");
    Expect(required.error_code == "RequiredFieldMissing",
        "Capability Contract facade should classify required-field diagnostics");
    Expect(!required.diagnostics.empty() && required.diagnostics.front().field == "path",
        "Capability Contract required-field diagnostic should carry the field name");

    const auto type = agentos::ValidateCapabilityContractJsonObject(
        R"({"type":"object","properties":{"count":{"type":"integer"}}})",
        R"({"count":"many"})",
        "skill input");
    Expect(!type.valid, "Capability Contract facade should reject invalid field types");
    Expect(type.error_code == "InvalidFieldType",
        "Capability Contract facade should classify type diagnostics");
    Expect(!type.diagnostics.empty() && type.diagnostics.front().constraint == "integer",
        "Capability Contract type diagnostic should carry the expected type");

    const auto constraint = agentos::ValidateCapabilityContractJsonObject(
        R"({"type":"object","properties":{"count":{"type":"integer","minimum":2}}})",
        R"({"count":1})",
        "skill input");
    Expect(!constraint.valid, "Capability Contract facade should reject shape constraints");
    Expect(constraint.error_code == "ConstraintViolation",
        "Capability Contract facade should classify constraint diagnostics");
    Expect(!constraint.diagnostics.empty() && constraint.diagnostics.front().constraint == "minimum",
        "Capability Contract constraint diagnostic should carry the failed constraint");
}

void TestCapabilityContractDeclarationFacade() {
    auto malformed = BaseCapabilityManifest();
    malformed.input_schema_json = "{";
    const auto malformed_result = agentos::ValidateCapabilityDeclaration(malformed);
    Expect(!malformed_result.valid, "Capability Contract declaration facade should reject malformed schemas");
    Expect(malformed_result.error_code == "MalformedSchema",
        "Capability Contract declaration facade should classify malformed schema diagnostics");
    Expect(!malformed_result.diagnostics.empty() && malformed_result.diagnostics.front().field == "input_schema_json",
        "malformed schema diagnostic should identify the schema field");

    auto invalid_risk = BaseCapabilityManifest();
    invalid_risk.risk_level = "severe";
    const auto risk_result = agentos::ValidateCapabilityDeclaration(invalid_risk);
    Expect(!risk_result.valid, "Capability Contract declaration facade should reject invalid risk levels");
    Expect(risk_result.error_code == "InvalidRiskLevel",
        "Capability Contract declaration facade should classify invalid risk diagnostics");
    Expect(!risk_result.diagnostics.empty() && risk_result.diagnostics.front().field == "risk_level",
        "invalid risk diagnostic should identify risk_level");

    auto unknown_permission = BaseCapabilityManifest();
    unknown_permission.permissions = {"filesystem.read", "unknown.scope"};
    const auto permission_result = agentos::ValidateCapabilityDeclaration(unknown_permission);
    Expect(!permission_result.valid, "Capability Contract declaration facade should reject unknown permissions");
    Expect(permission_result.error_code == "UnknownPermission",
        "Capability Contract declaration facade should classify unknown permission diagnostics");
    Expect(!permission_result.diagnostics.empty() &&
               permission_result.diagnostics.front().constraint.find("unknown.scope") != std::string::npos,
        "unknown permission diagnostic should carry the unknown permission");
}

}  // namespace

int main() {
    TestSplitHelpers();
    TestStrictNumberParsing();
    TestJsonObjectShape();
    TestJoinStrings();
    TestCancellationToken();
    TestCapabilityContractJsonObjectFacade();
    TestCapabilityContractDeclarationFacade();

    if (failures != 0) {
        std::cerr << failures << " spec parsing test assertion(s) failed\n";
        return 1;
    }

    std::cout << "agentos_spec_parsing_tests passed\n";
    return 0;
}
