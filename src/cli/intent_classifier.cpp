#include "cli/intent_classifier.hpp"

#include "cli/interactive_intent_registry.hpp"
#include "cli/interactive_route_policy.hpp"

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

bool LooksLikeWorkspaceMutationRequest(const std::string& line) {
    static const std::regex mutation_re(
        R"((\b(write|create|build|develop|implement|scaffold|modify|edit|patch|code|program|app|install|setup|configure|add|save|persist)\b)|\bgenerate\b.{0,60}\b(file|code|project|repo|repository|plugin|skill|command|manifest|doc|document|artifact)\b|编写|创建|修改|安装|配置|新增|添加|写入|写文件|保存|保存为文件|落地|安装到|接入到\s*\.agents|加入到\s*\.agents|生成.{0,60}(文件|代码|项目|仓库|插件|技能|命令|manifest|文档|产物|交付|PPT|大纲))",
        std::regex_constants::icase);
    return std::regex_search(line, mutation_re);
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

std::string ExecutionModeName(const InteractiveExecutionMode mode) {
    switch (mode) {
    case InteractiveExecutionMode::sync:
        return "sync";
    case InteractiveExecutionMode::async_job:
        return "async_job";
    }
    return "sync";
}

bool LooksLikeExplicitDevelopmentChangeRequest(const std::string& line) {
    static const std::regex skill_creation_re(
        R"((\b(create|generate|install|add|build|scaffold|configure)\b).{0,40}\b(skills?|tools?|commands?)\b|(\b(skills?|tools?|commands?)\b).{0,40}\b(create|generate|install|add|build|scaffold|configure)\b|(创建|生成|安装|新增|添加|配置|写入|保存).{0,40}(技能|工具|命令)|(技能|工具|命令).{0,40}(创建|生成|安装|新增|添加|配置|写入|保存))",
        std::regex_constants::icase);
    if (std::regex_search(line, skill_creation_re)) {
        return true;
    }
    return LooksLikeWorkspaceMutationRequest(line);
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
    return LooksLikeRuntimeSelfDescriptionIntent(line);
}

bool LooksLikeHostInfoRequest(const std::string& line) {
    return LooksLikeHostInfoIntent(line);
}

bool LooksLikeSpecificSkillUsageRequest(const std::string& line,
                                        const SkillRegistry& skill_registry) {
    return LooksLikeSpecificSkillUsageIntent(line, skill_registry);
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
    const auto task_id = MakeRouteTaskId();

    InteractiveRouteProposal proposal;
    proposal.task_id = task_id;
    proposal.user_request = line;
    proposal.signals = InteractiveRouteSignals{
        .development = LooksLikeDevelopmentRequest(line),
        .research = LooksLikeResearchRequest(line),
        .workspace_mutation = LooksLikeWorkspaceMutationRequest(line),
        .artifact = RegexAny(line, R"((\b(ppt|pptx|docx|xlsx|presentation|slides?|3d|model|render|diagram|artifact|file|skill|skills?)\b)|文件|产物|交付|幻灯片|演示文稿|三维|机械结构图|技能|skill)"),
        .multi_step_analysis = RegexAny(line, R"((\b(review|audit|analy[sz]e|debug|diagnose|why|compare)\b)|审核|分析|排查|诊断|为什么|比较)"),
    };
    const InteractiveRouteTargets targets{
        .chat = resolve_chat_target ? resolve_chat_target() : std::string{},
        .development = resolve_dev_target ? resolve_dev_target() : std::string{},
        .research = resolve_research_target ? resolve_research_target() : std::string{},
    };

    if (const auto local_intent = MatchHardLocalInteractiveIntent(line, skill_registry);
        local_intent.has_value()) {
        proposal.route = local_intent->route;
        proposal.score += local_intent->score;
        proposal.reasons.push_back(local_intent->reason);
        proposal.selected_target = local_intent->selected_target;
        proposal.authoritative = true;
        return MakeInteractiveRouteVerdict(proposal, targets).decision;
    }

    proposal.route = InteractiveRouteKind::chat_agent;
    proposal.selected_target = targets.chat;
    proposal.score += 1;
    proposal.reasons.push_back("free-form natural language is handled by main");
    proposal.authoritative = true;

    decision = MakeInteractiveRouteVerdict(proposal, targets).decision;
    (void)usage_snapshot;
    (void)workspace;
    (void)agent_registry;
    (void)looks_like_registered_skill_use;
    (void)resolve_registered_skill;
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
    json["execution_mode"] = ExecutionModeName(decision.execution_mode);
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
    std::cout << "; mode=" << ExecutionModeName(decision.execution_mode);
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
            std::cout << "交由主代理结合上下文判断";
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
