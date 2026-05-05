#include "cli/intent_classifier.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <regex>
#include <string>

#include <nlohmann/json.hpp>

namespace agentos {

namespace {

std::string MakeRouteTaskId() {
    const auto value = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
    return std::string("interactive-route-") + std::to_string(value);
}

bool RegexAny(const std::string& line, const std::string& pattern) {
    return std::regex_search(line, std::regex(pattern, std::regex_constants::icase));
}

}  // namespace

std::string RouteKindName(const InteractiveRouteKind kind) {
    switch (kind) {
    case InteractiveRouteKind::local_intent:
        return "local_intent";
    case InteractiveRouteKind::direct_skill:
        return "direct_skill";
    case InteractiveRouteKind::development_agent:
        return "development_agent";
    case InteractiveRouteKind::research_agent:
        return "research_agent";
    case InteractiveRouteKind::chat_agent:
        return "chat_agent";
    case InteractiveRouteKind::unknown_command:
        return "unknown_command";
    }
    return "unknown";
}

bool LooksLikeExplicitDevelopmentChangeRequest(const std::string& line) {
    static const std::regex skill_creation_re(
        R"((\b(create|generate|install|add|build|scaffold|configure|integrate)\b).{0,40}\b(skills?|tools?|commands?)\b|(\b(skills?|tools?|commands?)\b).{0,40}\b(create|generate|install|add|build|scaffold|configure|integrate)\b|(创建|生成|安装|新增|添加|接入|配置|获得|获取).{0,40}(技能|工具|命令)|(技能|工具|命令).{0,40}(创建|生成|安装|新增|添加|接入|配置|获得|获取))",
        std::regex_constants::icase);
    if (std::regex_search(line, skill_creation_re)) {
        return true;
    }
    static const std::regex dev_change_re(
        R"((\b(write|create|build|develop|continue|implement|generate|scaffold|modify|edit|patch|code|program|app|project|repo|repository|files?|install|setup|configure|add|claude\s+code|codex)\b)|编写|创建|实现|生成|修改|开发|继续|安装|配置|新增|添加|代码|程序|项目|仓库|输出文件|写文件)",
        std::regex_constants::icase);
    return std::regex_search(line, dev_change_re);
}

bool LooksLikeDevelopmentRequest(const std::string& line) {
    if (LooksLikeExplicitDevelopmentChangeRequest(line)) {
        return true;
    }
    static const std::regex dev_re(
        R"((\b(write|create|build|develop|continue|implement|generate|scaffold|modify|edit|patch|code|program|app|player|project|repo|repository|files?|install|setup|configure|add|claude\s+code|codex)\b)|编写|创建|实现|生成|修改|开发|继续|安装|配置|新增|添加|代码|程序|项目|仓库|播放器|输出文件|写文件)",
        std::regex_constants::icase);
    return std::regex_search(line, dev_re);
}

bool LooksLikeResearchRequest(const std::string& line) {
    static const std::regex research_re(
        R"((\b(research|investigate|look\s+up|search|web\s+search|find\s+out|integrat(e|ion)|connect)\b)|研究|调研|搜索|查找|网上|互联网|接入|如何接入|怎么接入)",
        std::regex_constants::icase);
    return std::regex_search(line, research_re);
}

bool LooksLikeRuntimeSelfDescriptionRequest(const std::string& line) {
    static const std::regex self_description_re(
        R"((\b(what\s+(skills?|abilities|capabilities|agents?|adapters?)|list\s+(skills?|agents?|adapters?)|show\s+(skills?|agents?|adapters?)|available\s+(skills?|agents?|adapters?)|how\s+to\s+use\s+agentos|usage|help|commands?|what\s+can\s+you\s+do|how\s+do\s+i\s+use\s+agentos)\b)|你.*(有什么|有哪些|会什么|能做什么).*(技能|能力|代理|智能体)|有哪些.*(技能|能力|agent|agents|代理|智能体|命令)|(技能|能力|agent|agents|代理|智能体).*(有哪些|如何用|怎么用|如何使用)|列出.*(技能|能力|代理|智能体)|如何使用\s*AgentOS|AgentOS.*(使用方法|怎么用|如何用)|使用方法|帮助|你能做什么)",
        std::regex_constants::icase);
    return std::regex_search(line, self_description_re);
}

bool LooksLikeHostInfoRequest(const std::string& line) {
    static const std::regex host_re(
        R"((\b(your|host|machine|server)\s*(ip|hostname|address)\b)|(\bip\s*address\b)|(\blocal\s*ip\b)|(\bhostname\b)|(你的\s*ip)|(主机(的)?\s*(ip|地址|名))|(本机(的)?\s*(ip|地址)))",
        std::regex_constants::icase);
    return std::regex_search(line, host_re);
}

bool LooksLikeSpecificSkillUsageRequest(const std::string& line,
                                        const SkillRegistry& skill_registry) {
    auto manifests = skill_registry.list();
    std::sort(manifests.begin(), manifests.end(),
              [](const SkillManifest& lhs, const SkillManifest& rhs) {
                  return lhs.name.size() > rhs.name.size();
              });

    auto to_lower = [](std::string value) {
        std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        return value;
    };
    const auto lower_line = to_lower(line);

    bool has_mention = false;
    for (const auto& manifest : manifests) {
        if (manifest.name.empty()) {
            continue;
        }
        if (lower_line.find(to_lower(manifest.name)) != std::string::npos) {
            has_mention = true;
            break;
        }
    }
    if (!has_mention) {
        return false;
    }
    static const std::regex usage_re(
        R"((\b(how\s+to\s+use|usage|help|show|inspect|schema|args?|arguments?|examples?|what\s+does)\b)|如何使用|怎么使用|如何用|怎么用|用法|参数|示例|例子|是什么|能做什么)",
        std::regex_constants::icase);
    return std::regex_search(line, usage_re);
}

RouteDecisionExplanation ClassifyInteractiveRequest(
    const std::string& line,
    SkillRegistry& skill_registry,
    AgentRegistry& agent_registry,
    const UsageSnapshot& usage_snapshot,
    const std::filesystem::path& workspace,
    const TargetResolver& resolve_chat_target,
    const TargetResolver& resolve_dev_target,
    const TargetResolver& resolve_research_target,
    const RegisteredSkillUseChecker& looks_like_registered_skill_use,
    const RegisteredSkillResolver& resolve_registered_skill) {
    RouteDecisionExplanation decision;
    decision.task_id = MakeRouteTaskId();
    decision.user_request = line;

    const bool looks_development = LooksLikeDevelopmentRequest(line);
    const bool looks_research = LooksLikeResearchRequest(line);
    const bool looks_artifact =
        RegexAny(line, R"((\b(ppt|pptx|docx|xlsx|presentation|slides?|3d|model|render|diagram|artifact|file|skill|skills?)\b)|文件|产物|交付|幻灯片|演示文稿|三维|机械结构图|技能|skill)");

    if (!looks_development && !looks_research &&
        LooksLikeSpecificSkillUsageRequest(line, skill_registry)) {
        decision.route = InteractiveRouteKind::local_intent;
        decision.score -= 4;
        decision.reasons.push_back("mentions a registered skill and asks for its usage");
        decision.selected_target = "interactive_runtime";
        return decision;
    }

    if (looks_like_registered_skill_use && looks_like_registered_skill_use(line)) {
        decision.route = InteractiveRouteKind::direct_skill;
        decision.score += 5;
        decision.reasons.push_back("asks to use a registered skill as a tool");
        const auto resolved = resolve_registered_skill ? resolve_registered_skill(line) : std::string{};
        decision.selected_target = resolved.empty() ? "interactive_runtime" : resolved;
        return decision;
    }

    if (!looks_development && !looks_research &&
        (LooksLikeRuntimeSelfDescriptionRequest(line) || LooksLikeHostInfoRequest(line))) {
        decision.route = InteractiveRouteKind::local_intent;
        decision.score -= 5;
        decision.reasons.push_back("matches runtime self-description or host-info local intent");
        decision.selected_target = "interactive_runtime";
        return decision;
    }

    if (skill_registry.find("host_info") &&
        !looks_development && !looks_research &&
        LooksLikeHostInfoRequest(line)) {
        decision.route = InteractiveRouteKind::direct_skill;
        decision.score -= 4;
        decision.reasons.push_back("directly maps to registered host_info skill");
        decision.selected_target = "host_info";
        return decision;
    }

    if (looks_development) {
        decision.route = InteractiveRouteKind::development_agent;
        decision.score += 4;
        decision.reasons.push_back("requires file/code/artifact style development work");
        decision.selected_target = resolve_dev_target ? resolve_dev_target() : std::string{};
    }
    if (looks_research) {
        if (decision.route != InteractiveRouteKind::development_agent) {
            decision.route = InteractiveRouteKind::research_agent;
            decision.selected_target = resolve_research_target ? resolve_research_target() : std::string{};
        }
        decision.score += 3;
        decision.reasons.push_back("requires current external research or integration analysis");
    }
    if (looks_artifact) {
        decision.score += 4;
        decision.reasons.push_back("mentions concrete deliverables or artifacts");
        if (decision.route == InteractiveRouteKind::chat_agent) {
            decision.route = InteractiveRouteKind::development_agent;
            decision.selected_target = resolve_dev_target ? resolve_dev_target() : std::string{};
        }
    }
    if (RegexAny(line, R"((\b(review|audit|analy[sz]e|debug|diagnose|why|compare)\b)|审核|分析|排查|诊断|为什么|比较)")) {
        decision.score += 2;
        decision.reasons.push_back("requires multi-step analysis");
    }

    if (decision.reasons.empty()) {
        decision.route = InteractiveRouteKind::chat_agent;
        decision.selected_target = resolve_chat_target ? resolve_chat_target() : std::string{};
        decision.reasons.push_back("no direct skill, development, or research trigger matched");
    }
    if (decision.selected_target.empty()) {
        decision.selected_target = decision.route == InteractiveRouteKind::chat_agent
            ? (resolve_chat_target ? resolve_chat_target() : std::string{})
            : "unavailable";
    }
    (void)usage_snapshot;
    (void)workspace;
    (void)agent_registry;
    return decision;
}

void WriteRouteDecision(const std::filesystem::path& workspace,
                        const RouteDecisionExplanation& decision) {
    const auto path = workspace / "runtime" / "routing" / (decision.task_id + ".json");
    std::filesystem::create_directories(path.parent_path());
    nlohmann::ordered_json json;
    json["task_id"] = decision.task_id;
    json["user_request"] = decision.user_request;
    json["route"] = RouteKindName(decision.route);
    json["selected_target"] = decision.selected_target;
    json["score"] = decision.score;
    json["reasons"] = decision.reasons;
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << json.dump(2) << '\n';
}

namespace {

std::string LocalizedRouteKindName(const InteractiveRouteKind route,
                                   const RuntimeLanguage language) {
    if (language != RuntimeLanguage::Chinese) {
        return RouteKindName(route);
    }
    switch (route) {
    case InteractiveRouteKind::local_intent:
        return "本地意图";
    case InteractiveRouteKind::direct_skill:
        return "直接调用 skill";
    case InteractiveRouteKind::development_agent:
        return "开发任务";
    case InteractiveRouteKind::research_agent:
        return "研究任务";
    case InteractiveRouteKind::chat_agent:
        return "聊天";
    case InteractiveRouteKind::unknown_command:
        return "未知命令";
    }
    return "未知";
}

}  // namespace

void PrintRouteDecision(const RouteDecisionExplanation& decision,
                        const RuntimeLanguage language) {
    std::cout << (language == RuntimeLanguage::Chinese ? "(路由: " : "(route: ")
              << LocalizedRouteKindName(decision.route, language);
    if (!decision.selected_target.empty()) {
        std::cout << " -> " << decision.selected_target;
    }
    if (language == RuntimeLanguage::Chinese) {
        std::cout << "; ";
        switch (decision.route) {
        case InteractiveRouteKind::local_intent:
            if (std::find(decision.reasons.begin(), decision.reasons.end(),
                          "mentions a registered skill and asks for its usage") != decision.reasons.end()) {
                std::cout << "询问已注册 skill 的具体用法";
            } else {
                std::cout << "匹配运行时自说明或主机信息本地意图";
            }
            break;
        case InteractiveRouteKind::direct_skill:
            if (std::find(decision.reasons.begin(), decision.reasons.end(),
                          "asks to use a registered skill as a tool") != decision.reasons.end()) {
                std::cout << "使用已注册 skill 作为工具执行";
            } else {
                std::cout << "可直接映射到已登记的 skill";
            }
            break;
        case InteractiveRouteKind::development_agent:
            std::cout << "需要生成或修改文件/代码/交付物";
            break;
        case InteractiveRouteKind::research_agent:
            std::cout << "需要联网研究或集成分析";
            break;
        case InteractiveRouteKind::chat_agent:
            std::cout << "未匹配到本地 skill、开发或研究触发条件";
            break;
        case InteractiveRouteKind::unknown_command:
            std::cout << "命令不存在";
            break;
        }
    } else {
        std::cout << "; ";
        for (std::size_t i = 0; i < decision.reasons.size(); ++i) {
            if (i != 0) std::cout << "; ";
            std::cout << decision.reasons[i];
        }
    }
    std::cout << ")\n";
}

}  // namespace agentos
