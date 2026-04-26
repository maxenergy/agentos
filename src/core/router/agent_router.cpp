#include "core/router/router_components.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
#include <memory>

namespace agentos {

namespace {

std::string ToLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool LooksLikeAgentWork(const std::string& objective) {
    const auto lower = ToLower(objective);
    constexpr std::array<const char*, 4> keywords = {"plan", "analyze", "design", "reason"};

    return std::any_of(keywords.begin(), keywords.end(), [&](const char* keyword) {
        return lower.find(keyword) != std::string::npos;
    });
}

int LessonOccurrenceCount(
    const MemoryManager* memory_manager,
    const std::string& task_type,
    const std::string& target_name) {
    if (!memory_manager) {
        return 0;
    }

    int occurrences = 0;
    for (const auto& lesson : memory_manager->lesson_store().list()) {
        if (lesson.enabled && lesson.task_type == task_type && lesson.target_name == target_name) {
            occurrences += lesson.occurrence_count;
        }
    }
    return occurrences;
}

bool HasRuntimeHistory(const MemoryManager* memory_manager) {
    return memory_manager &&
           (!memory_manager->agent_stats().empty() ||
            !memory_manager->lesson_store().list().empty());
}

std::shared_ptr<IAgentAdapter> BestHealthyAgent(
    const AgentRegistry& registry,
    const MemoryManager* memory_manager,
    const std::string& task_type) {
    if (!HasRuntimeHistory(memory_manager)) {
        return registry.first_healthy();
    }

    std::shared_ptr<IAgentAdapter> best_agent;
    double best_score = -std::numeric_limits<double>::infinity();

    for (const auto& profile : registry.list_profiles()) {
        const auto agent = registry.find(profile.agent_name);
        if (!agent || !agent->healthy()) {
            continue;
        }

        double score = 0.0;
        if (const auto stats_it = memory_manager->agent_stats().find(profile.agent_name);
            stats_it != memory_manager->agent_stats().end() && stats_it->second.total_runs > 0) {
            const auto& stats = stats_it->second;
            const auto success_rate = static_cast<double>(stats.success_runs) / static_cast<double>(stats.total_runs);
            score = (success_rate * 100.0) - (stats.avg_duration_ms / 1000.0);
        }
        score -= static_cast<double>(LessonOccurrenceCount(memory_manager, task_type, profile.agent_name)) * 25.0;

        if (!best_agent || score > best_score) {
            best_score = score;
            best_agent = agent;
        }
    }

    return best_agent ? best_agent : registry.first_healthy();
}

}  // namespace

RouteDecision AgentRouter::route_agent_work(
    const TaskRequest& task,
    const AgentRegistry& agent_registry,
    const MemoryManager* memory_manager) const {
    if (task.task_type != "analysis" && task.task_type != "delegate" && !LooksLikeAgentWork(task.objective)) {
        return {RouteTargetKind::none, "", "task does not require agent routing"};
    }

    const auto agent = BestHealthyAgent(agent_registry, memory_manager, task.task_type);
    if (!agent) {
        return {RouteTargetKind::none, "", "no healthy agent route matched"};
    }

    return {
        RouteTargetKind::agent,
        agent->profile().agent_name,
        HasRuntimeHistory(memory_manager)
            ? "objective routed by agent health, historical score, and lessons"
            : "objective is better handled by an agent adapter",
    };
}

RouteDecision AgentRouter::route_named_agent(
    const TaskRequest& task,
    const AgentRegistry& agent_registry) const {
    if (healthy_agent_exists(agent_registry, task.task_type)) {
        return {RouteTargetKind::agent, task.task_type, "task_type matched an agent name directly"};
    }

    return {RouteTargetKind::none, "", "no named agent route matched"};
}

bool AgentRouter::healthy_agent_exists(
    const AgentRegistry& agent_registry,
    const std::string& name) const {
    const auto agent = agent_registry.find(name);
    return agent && agent->healthy();
}

}  // namespace agentos
