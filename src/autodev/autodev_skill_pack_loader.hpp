#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace agentos {

struct AutoDevSkillPackLoadResult {
    bool success = false;
    std::string error_message;
    std::string manifest_hash;
    std::string commit;
    std::vector<std::string> required_steps;
    nlohmann::json snapshot;
};

class AutoDevSkillPackLoader {
public:
    AutoDevSkillPackLoadResult load_local_path(const std::filesystem::path& local_path) const;
};

}  // namespace agentos
