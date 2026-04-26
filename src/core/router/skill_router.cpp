#include "core/router/router_components.hpp"

namespace agentos {

RouteDecision SkillRouter::route_builtin_or_named(
    const TaskRequest& task,
    const SkillRegistry& skill_registry) const {
    if (task.task_type == "read_file" && healthy_skill_exists(skill_registry, "file_read")) {
        return {RouteTargetKind::skill, "file_read", "task_type maps to file_read"};
    }
    if (task.task_type == "write_file" && healthy_skill_exists(skill_registry, "file_write")) {
        return {RouteTargetKind::skill, "file_write", "task_type maps to file_write"};
    }
    if (task.task_type == "patch_file" && healthy_skill_exists(skill_registry, "file_patch")) {
        return {RouteTargetKind::skill, "file_patch", "task_type maps to file_patch"};
    }

    if (healthy_skill_exists(skill_registry, task.task_type)) {
        return {RouteTargetKind::skill, task.task_type, "task_type matched a skill name directly"};
    }

    return {RouteTargetKind::none, "", "no skill route matched"};
}

bool SkillRouter::healthy_skill_exists(
    const SkillRegistry& skill_registry,
    const std::string& name) const {
    const auto skill = skill_registry.find(name);
    return skill && skill->healthy();
}

}  // namespace agentos
