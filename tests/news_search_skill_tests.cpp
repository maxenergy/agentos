#include "skills/builtin/news_search_skill.hpp"

#include "test_command_fixtures.hpp"

#include <filesystem>
#include <iostream>

#include <nlohmann/json.hpp>

namespace {

int failures = 0;

void Expect(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

std::filesystem::path FreshWorkspace() {
    const auto workspace = std::filesystem::temp_directory_path() / "agentos_news_search_skill_tests";
    std::filesystem::remove_all(workspace);
    std::filesystem::create_directories(workspace);
    return workspace;
}

void WriteCurlNewsFixture(const std::filesystem::path& bin_dir) {
    std::filesystem::create_directories(bin_dir);
#ifdef _WIN32
    agentos::test::WriteCliFixture(
        bin_dir,
        "curl",
        "@echo off\n"
        "echo ^<rss^>^<channel^>\n"
        "echo ^<item^>^<title^>Google AI News One^</title^>^<link^>https://example.com/1^</link^>^<source^>Example^</source^>^<pubDate^>Wed, 06 May 2026 00:00:00 GMT^</pubDate^>^</item^>\n"
        "echo ^<item^>^<title^>Google AI News Two^</title^>^<link^>https://example.com/2^</link^>^<source^>Example^</source^>^<pubDate^>Wed, 06 May 2026 01:00:00 GMT^</pubDate^>^</item^>\n"
        "echo ^</channel^>^</rss^>\n"
        "exit /b 0\n");
#else
    agentos::test::WriteCliFixture(
        bin_dir,
        "curl",
        "#!/bin/sh\n"
        "cat <<'RSS'\n"
        "<rss><channel>\n"
        "<item><title>Google AI News One</title><link>https://example.com/1</link><source>Example</source><pubDate>Wed, 06 May 2026 00:00:00 GMT</pubDate></item>\n"
        "<item><title>Google AI News Two</title><link>https://example.com/2</link><source>Example</source><pubDate>Wed, 06 May 2026 01:00:00 GMT</pubDate></item>\n"
        "</channel></rss>\n"
        "RSS\n");
#endif
}

void TestNewsSearchParsesGoogleNewsRss() {
    const auto workspace = FreshWorkspace();
    const auto bin_dir = workspace / "bin";
    WriteCurlNewsFixture(bin_dir);
    agentos::test::ScopedEnvOverride path_override("PATH", agentos::test::PrependPathForTest(bin_dir));

    agentos::CliHost cli_host;
    agentos::NewsSearchSkill skill(cli_host);
    agentos::SkillCall call;
    call.workspace_id = workspace.string();
    call.arguments["query"] = "谷歌";
    call.arguments["limit"] = "2";
    call.arguments["days"] = "7";

    const auto result = skill.execute(call);
    Expect(result.success, "news_search should succeed with a valid RSS response");
    const auto output = nlohmann::json::parse(result.json_output);
    Expect(output["items"].is_array() && output["items"].size() == 2,
        "news_search should parse two RSS items");
    Expect(output["items"][0]["title"].get<std::string>() == "Google AI News One",
        "news_search should preserve item title");
    Expect(output["summary"].get<std::string>().find("Google AI News One") != std::string::npos,
        "news_search should include item title in summary");
}

}  // namespace

int main() {
    TestNewsSearchParsesGoogleNewsRss();

    if (failures != 0) {
        std::cerr << failures << " news search skill assertion(s) failed\n";
        return 1;
    }
    std::cout << "news search skill tests passed\n";
    return 0;
}
