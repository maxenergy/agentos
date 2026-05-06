#include "core/registry/skill_registry.hpp"

#include <algorithm>

namespace agentos {

bool SkillRegistry::register_skill(const std::shared_ptr<ISkillAdapter>& skill) {
    if (!skill) {
        return false;
    }

    const auto name = skill->manifest().name;
    if (!skills_.contains(name)) {
        registration_order_.push_back(name);
    }
    skills_[name] = skill;
    return true;
}

bool SkillRegistry::unregister_skill(const std::string& name) {
    const auto erased = skills_.erase(name);
    if (erased == 0) {
        return false;
    }
    registration_order_.erase(
        std::remove(registration_order_.begin(), registration_order_.end(), name),
        registration_order_.end());
    return true;
}

std::shared_ptr<ISkillAdapter> SkillRegistry::find(const std::string& name) const {
    const auto it = skills_.find(name);
    if (it == skills_.end()) {
        return nullptr;
    }
    return it->second;
}

std::shared_ptr<ISkillAdapter> SkillRegistry::first_healthy() const {
    for (const auto& name : registration_order_) {
        const auto skill = find(name);
        if (skill && skill->healthy()) {
            return skill;
        }
    }
    return nullptr;
}

std::vector<SkillManifest> SkillRegistry::list() const {
    std::vector<SkillManifest> manifests;
    manifests.reserve(skills_.size());

    for (const auto& name : registration_order_) {
        const auto skill = find(name);
        if (skill) {
            manifests.push_back(skill->manifest());
        }
    }

    return manifests;
}

}  // namespace agentos
