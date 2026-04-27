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

}  // namespace

int main() {
    TestSplitHelpers();
    TestStrictNumberParsing();
    TestJsonObjectShape();
    TestJoinStrings();
    TestCancellationToken();

    if (failures != 0) {
        std::cerr << failures << " spec parsing test assertion(s) failed\n";
        return 1;
    }

    std::cout << "agentos_spec_parsing_tests passed\n";
    return 0;
}
