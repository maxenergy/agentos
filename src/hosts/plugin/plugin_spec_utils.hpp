#pragma once

#include "hosts/cli/cli_host.hpp"
#include "hosts/plugin/plugin_host.hpp"

#include <string>

namespace agentos {

std::string PluginSpecUnsupportedReason(const PluginSpec& spec);
CliSpec ToCliSpec(const PluginSpec& spec);
CliSpec ToPluginHealthCliSpec(const PluginSpec& spec);

}  // namespace agentos
