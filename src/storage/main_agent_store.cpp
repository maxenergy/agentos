#include "storage/main_agent_store.hpp"

#include "utils/atomic_file.hpp"

#include <fstream>
#include <sstream>
#include <string>
#include <utility>

namespace agentos {

namespace {

std::string Escape(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (const char c : value) {
        if (c == '\t') {
            out += "\\t";
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else if (c == '\\') {
            out += "\\\\";
        } else {
            out += c;
        }
    }
    return out;
}

std::string Unescape(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '\\' && i + 1 < value.size()) {
            const char next = value[i + 1];
            if (next == 't') { out += '\t'; ++i; continue; }
            if (next == 'n') { out += '\n'; ++i; continue; }
            if (next == 'r') { out += '\r'; ++i; continue; }
            if (next == '\\') { out += '\\'; ++i; continue; }
        }
        out += value[i];
    }
    return out;
}

}  // namespace

MainAgentStore::MainAgentStore(std::filesystem::path path) : path_(std::move(path)) {}

std::optional<MainAgentConfig> MainAgentStore::load() const {
    std::ifstream in(path_);
    if (!in) {
        return std::nullopt;
    }

    MainAgentConfig config;
    bool any_field_set = false;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line.front() == '#') {
            continue;
        }
        const auto tab_pos = line.find('\t');
        if (tab_pos == std::string::npos) {
            continue;
        }
        const auto key = line.substr(0, tab_pos);
        const auto value = Unescape(line.substr(tab_pos + 1));

        if (key == "provider_kind") { config.provider_kind = value; any_field_set = true; }
        else if (key == "base_url") { config.base_url = value; any_field_set = true; }
        else if (key == "api_key_env") { config.api_key_env = value; any_field_set = true; }
        else if (key == "oauth_file") { config.oauth_file = value; any_field_set = true; }
        else if (key == "model") { config.model = value; any_field_set = true; }
        else if (key == "default_timeout_ms") {
            try { config.default_timeout_ms = std::stoi(value); any_field_set = true; }
            catch (...) {}
        }
    }
    if (!any_field_set) {
        return std::nullopt;
    }
    return config;
}

bool MainAgentStore::save(const MainAgentConfig& config) const {
    std::ostringstream out;
    out << "# AgentOS main-agent configuration. Managed by `agentos main-agent set`.\n";
    out << "provider_kind\t" << Escape(config.provider_kind) << "\n";
    out << "base_url\t" << Escape(config.base_url) << "\n";
    out << "api_key_env\t" << Escape(config.api_key_env) << "\n";
    out << "oauth_file\t" << Escape(config.oauth_file) << "\n";
    out << "model\t" << Escape(config.model) << "\n";
    out << "default_timeout_ms\t" << config.default_timeout_ms << "\n";

    std::error_code ec;
    if (path_.has_parent_path()) {
        std::filesystem::create_directories(path_.parent_path(), ec);
    }
    try {
        WriteFileAtomically(path_, out.str());
        return true;
    } catch (...) {
        return false;
    }
}

bool MainAgentStore::clear() const {
    std::error_code ec;
    std::filesystem::remove(path_, ec);
    return !ec;
}

}  // namespace agentos
