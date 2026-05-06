#include "cli/interactive_intent_registry.hpp"

#include <algorithm>
#include <cctype>
#include <regex>

namespace agentos {

namespace {

bool RegexAny(const std::string& line, const std::string& pattern) {
    return std::regex_search(line, std::regex(pattern, std::regex_constants::icase));
}

std::string ToLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

}  // namespace

bool LooksLikeMainAgentConfigIntent(const std::string& line) {
    static const std::regex config_re(
        R"((\b(default|preferred|primary|chat)\s+(model|agent)|\b(main-agent|ollama|lm\s*studio|llama\.cpp)\b.*\b(model|chat|default|preferred)\b|\b(set|configure|switch|use)\b.*\b(model|main-agent|ollama)\b)|(配置|设置|设为|切换|使用).{0,40}(默认|首选|聊天|对话|模型|model)|(默认|首选).{0,40}(模型|model|聊天|对话)|ollama.{0,40}(模型|model)|模型.{0,40}ollama)",
        std::regex_constants::icase);
    return std::regex_search(line, config_re);
}

bool LooksLikeModelIdentityIntent(const std::string& line) {
    static const std::regex model_re(
        R"((\b(what\s+(model|llm)\s+(are|is)\s+you|what\s+is\s+your\s+model|model\s+name|current\s+model|which\s+model)\b)|你.*(是什么|哪个|什么).*(模型|model)|当前.*(模型|model)|底层.*(模型|model)|使用.*(模型|model)|模型.*(名字|名称|是什么|哪个))",
        std::regex_constants::icase);
    return std::regex_search(line, model_re);
}

bool LooksLikeRuntimeSelfDescriptionIntent(const std::string& line) {
    static const std::regex self_description_re(
        R"((\b(what\s+(skills?|abilities|capabilities|agents?|adapters?|commands?)|list\s+(skills?|agents?|adapters?|commands?)|show\s+(skills?|agents?|adapters?|commands?)|available\s+(skills?|agents?|adapters?|commands?)|how\s+to\s+use\s+agentos|usage|help|what\s+can\s+you\s+do|how\s+do\s+i\s+use\s+agentos)\b)|你.*(有什么|有哪些|会什么|能做什么).*(技能|能力|代理|智能体)|有哪些.*(技能|能力|agent|agents|代理|智能体|命令)|(技能|能力|agent|agents|代理|智能体).*(有哪些|如何用|怎么用|如何使用)|列出.*(技能|能力|代理|智能体)|如何使用\s*AgentOS|AgentOS.*(使用方法|怎么用|如何用)|使用方法|帮助|你能做什么)",
        std::regex_constants::icase);
    return std::regex_search(line, self_description_re);
}

bool LooksLikeHostInfoIntent(const std::string& line) {
    static const std::regex host_re(
        R"((\b(your|host|machine|server)\s*(ip|hostname|address)\b)|(\bip\s*address\b)|(\blocal\s*ip\b)|(\bhostname\b)|(你的\s*ip)|(主机(的)?\s*(ip|地址|名))|(本机(的)?\s*(ip|地址)))",
        std::regex_constants::icase);
    return std::regex_search(line, host_re);
}

bool LooksLikeMemoryInspectIntent(const std::string& line) {
    static const std::regex memory_re(
        R"((\b(what\s+do\s+you\s+remember|what\s+is\s+in\s+memory|show\s+memory|memory\s+summary|remembered|lessons?)\b)|你.*(记得|记住|记忆|学到).*(什么|哪些|内容)|你(还)?记得什么|你记住了什么|你的记忆|记忆里有什么|学到了什么|记住了哪些|记得哪些|详细记忆|完整记忆|全部记忆|记住的教训|可复用工作流|记忆详情)",
        std::regex_constants::icase);
    return std::regex_search(line, memory_re);
}

bool LooksLikeSpecificSkillUsageIntent(const std::string& line,
                                       const SkillRegistry& skill_registry) {
    auto manifests = skill_registry.list();
    std::sort(manifests.begin(), manifests.end(),
              [](const SkillManifest& lhs, const SkillManifest& rhs) {
                  return lhs.name.size() > rhs.name.size();
              });

    const auto lower_line = ToLower(line);
    bool has_mention = false;
    for (const auto& manifest : manifests) {
        if (!manifest.name.empty() && lower_line.find(ToLower(manifest.name)) != std::string::npos) {
            has_mention = true;
            break;
        }
    }
    if (!has_mention) {
        return false;
    }
    return RegexAny(line,
        R"((\b(how\s+to\s+use|usage|help|show|inspect|schema|args?|arguments?|examples?|what\s+does)\b)|如何使用|怎么使用|如何用|怎么用|用法|参数|示例|例子|是什么|能做什么)");
}

std::optional<std::string> ExtractOllamaModelName(const std::string& line) {
    if (line.find("ollama") == std::string::npos &&
        line.find("Ollama") == std::string::npos) {
        return std::nullopt;
    }

    std::smatch match;
    static const std::regex tagged_model_re(
        R"(([A-Za-z][A-Za-z0-9_.-]*:[A-Za-z0-9_.-]+))",
        std::regex_constants::icase);
    if (std::regex_search(line, match, tagged_model_re) && match.size() >= 2) {
        return match[1].str();
    }

    static const std::regex before_model_word_re(
        R"(([A-Za-z][A-Za-z0-9_.:-]*)(?:\s*(?:模型|model)))",
        std::regex_constants::icase);
    if (std::regex_search(line, match, before_model_word_re) && match.size() >= 2) {
        const auto value = match[1].str();
        if (value != "ollama" && value != "Ollama") {
            return value;
        }
    }
    return std::nullopt;
}

std::optional<InteractiveIntentMatch> MatchHardLocalInteractiveIntent(
    const std::string& line,
    const SkillRegistry& skill_registry) {
    if (LooksLikeMainAgentConfigIntent(line)) {
        return InteractiveIntentMatch{
            .intent = InteractiveIntentKind::main_agent_configure,
            .reason = "configures the local main chat model",
            .score = -6,
        };
    }
    if (LooksLikeModelIdentityIntent(line)) {
        return InteractiveIntentMatch{
            .intent = InteractiveIntentKind::main_agent_inspect,
            .reason = "asks for the local main chat model",
            .score = -6,
        };
    }
    if (LooksLikeSpecificSkillUsageIntent(line, skill_registry)) {
        return InteractiveIntentMatch{
            .intent = InteractiveIntentKind::skill_usage,
            .reason = "mentions a registered skill and asks for its usage",
            .score = -4,
        };
    }
    if (LooksLikeRuntimeSelfDescriptionIntent(line)) {
        return InteractiveIntentMatch{
            .intent = InteractiveIntentKind::runtime_self_description,
            .reason = "matches runtime self-description local intent",
            .score = -5,
        };
    }
    if (LooksLikeMemoryInspectIntent(line)) {
        return InteractiveIntentMatch{
            .intent = InteractiveIntentKind::memory_inspect,
            .reason = "asks for local runtime memory",
            .score = -5,
        };
    }
    if (LooksLikeHostInfoIntent(line)) {
        return InteractiveIntentMatch{
            .intent = InteractiveIntentKind::host_info,
            .reason = "matches host-info local intent",
            .score = -5,
        };
    }
    return std::nullopt;
}

}  // namespace agentos
