#include "skills/builtin/news_search_skill.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace agentos {

namespace {

int ElapsedMs(const std::chrono::steady_clock::time_point started_at) {
    return static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started_at).count());
}

int ParseBoundedInt(const std::string& value, const int fallback, const int min_value, const int max_value) {
    try {
        return std::clamp(std::stoi(value), min_value, max_value);
    } catch (const std::exception&) {
        return fallback;
    }
}

std::string UrlEncode(const std::string& value) {
    std::ostringstream output;
    output << std::uppercase << std::hex;
    for (const unsigned char ch : value) {
        if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            output << static_cast<char>(ch);
        } else if (ch == ' ') {
            output << '+';
        } else {
            output << '%' << std::setw(2) << std::setfill('0') << static_cast<int>(ch);
        }
    }
    return output.str();
}

void ReplaceAll(std::string& value, const std::string& from, const std::string& to) {
    std::size_t pos = 0;
    while ((pos = value.find(from, pos)) != std::string::npos) {
        value.replace(pos, from.size(), to);
        pos += to.size();
    }
}

std::string DecodeXmlEntities(std::string value) {
    ReplaceAll(value, "<![CDATA[", "");
    ReplaceAll(value, "]]>", "");
    ReplaceAll(value, "&amp;", "&");
    ReplaceAll(value, "&lt;", "<");
    ReplaceAll(value, "&gt;", ">");
    ReplaceAll(value, "&quot;", "\"");
    ReplaceAll(value, "&#39;", "'");
    ReplaceAll(value, "&apos;", "'");
    return value;
}

std::string ExtractTagValue(const std::string& item, const std::string& tag) {
    const auto open_start = item.find("<" + tag);
    if (open_start == std::string::npos) {
        return {};
    }
    const auto open_end = item.find('>', open_start);
    if (open_end == std::string::npos) {
        return {};
    }
    const auto close = item.find("</" + tag + ">", open_end + 1);
    if (close == std::string::npos) {
        return {};
    }
    return DecodeXmlEntities(item.substr(open_end + 1, close - open_end - 1));
}

nlohmann::ordered_json ParseGoogleNewsRss(const std::string& rss, const int limit) {
    nlohmann::ordered_json items = nlohmann::ordered_json::array();
    std::size_t cursor = 0;
    while (items.size() < static_cast<std::size_t>(limit)) {
        const auto start = rss.find("<item>", cursor);
        if (start == std::string::npos) {
            break;
        }
        const auto end = rss.find("</item>", start);
        if (end == std::string::npos) {
            break;
        }
        const auto item_xml = rss.substr(start, end - start);
        cursor = end + 7;

        nlohmann::ordered_json item;
        item["title"] = ExtractTagValue(item_xml, "title");
        item["link"] = ExtractTagValue(item_xml, "link");
        item["source"] = ExtractTagValue(item_xml, "source");
        item["published"] = ExtractTagValue(item_xml, "pubDate");
        if (!item["title"].get<std::string>().empty()) {
            items.push_back(std::move(item));
        }
    }
    return items;
}

std::string BuildSummary(const nlohmann::ordered_json& items) {
    std::ostringstream summary;
    int index = 1;
    for (const auto& item : items) {
        summary << index++ << ". " << item.value("title", std::string{});
        const auto source = item.value("source", std::string{});
        if (!source.empty()) {
            summary << " (" << source << ")";
        }
        summary << '\n';
        const auto link = item.value("link", std::string{});
        if (!link.empty()) {
            summary << "   " << link << '\n';
        }
    }
    return summary.str();
}

}  // namespace

NewsSearchSkill::NewsSearchSkill(const CliHost& cli_host)
    : cli_host_(cli_host) {}

SkillManifest NewsSearchSkill::manifest() const {
    return {
        .name = "news_search",
        .version = "0.1.0",
        .description = "Fetch top news results through Google News RSS without invoking an agent.",
        .capabilities = {"network", "news", "search"},
        .input_schema_json = R"({"type":"object","properties":{"query":{"type":"string"},"limit":{"type":"string"},"days":{"type":"string"}},"required":["query"]})",
        .output_schema_json = R"({"type":"object","required":["query","items","summary"]})",
        .risk_level = "medium",
        .permissions = {"network.access", "process.spawn"},
        .supports_streaming = false,
        .idempotent = true,
        .timeout_ms = 12000,
    };
}

SkillResult NewsSearchSkill::execute(const SkillCall& call) {
    const auto started_at = std::chrono::steady_clock::now();
    const auto maybe_query = call.get_arg("query");
    if (!maybe_query.has_value() || maybe_query->empty()) {
        return {false, "", "InvalidArguments", "query is required", ElapsedMs(started_at)};
    }
    const auto limit = ParseBoundedInt(call.get_arg("limit").value_or("5"), 5, 1, 10);
    const auto days = ParseBoundedInt(call.get_arg("days").value_or("7"), 7, 1, 30);
    const auto search_query = *maybe_query + " when:" + std::to_string(days) + "d";
    const auto url = "https://news.google.com/rss/search?q=" + UrlEncode(search_query) +
        "&hl=zh-CN&gl=CN&ceid=CN:zh-Hans";

    const CliSpec spec{
        .name = "news_search",
        .description = "Fetch Google News RSS through curl.",
        .binary = "curl",
        .args_template = {"-L", "--silent", "--show-error", "--max-time", "10", "{{url}}"},
        .required_args = {"url"},
        .input_schema_json = R"({"type":"object","required":["url"]})",
        .output_schema_json = R"({"type":"object","required":["stdout","stderr","exit_code"]})",
        .parse_mode = "text",
        .risk_level = "medium",
        .permissions = {"network.access", "process.spawn"},
        .timeout_ms = 12000,
    };

    const auto result = cli_host_.run(CliRunRequest{
        .spec = spec,
        .arguments = {{"url", url}},
        .workspace_path = call.workspace_id,
    });

    const auto items = ParseGoogleNewsRss(result.stdout_text, limit);
    nlohmann::ordered_json output;
    output["query"] = *maybe_query;
    output["days"] = days;
    output["limit"] = limit;
    output["url"] = url;
    output["items"] = items;
    output["summary"] = BuildSummary(items);
    output["stderr"] = result.stderr_text;
    output["exit_code"] = result.exit_code;
    output["timed_out"] = result.timed_out;

    const bool success = result.success && !items.empty();
    return {
        .success = success,
        .json_output = output.dump(),
        .error_code = success ? "" : (result.error_code.empty() ? "NoNewsResults" : result.error_code),
        .error_message = success ? "" : (result.error_message.empty() ? "no news items were parsed" : result.error_message),
        .duration_ms = ElapsedMs(started_at),
    };
}

bool NewsSearchSkill::healthy() const {
    return true;
}

}  // namespace agentos
