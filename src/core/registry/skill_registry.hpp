#pragma once

#include "core/models.hpp"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace agentos {

class SkillRegistry {
public:
    bool register_skill(const std::shared_ptr<ISkillAdapter>& skill);
    bool unregister_skill(const std::string& name);
    std::shared_ptr<ISkillAdapter> find(const std::string& name) const;
    std::shared_ptr<ISkillAdapter> first_healthy() const;
    std::vector<SkillManifest> list() const;

private:
    std::unordered_map<std::string, std::shared_ptr<ISkillAdapter>> skills_;
    std::vector<std::string> registration_order_;
};

}  // namespace agentos
