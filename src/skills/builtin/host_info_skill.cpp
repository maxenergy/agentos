#include "skills/builtin/host_info_skill.hpp"

#include <chrono>
#include <map>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include <nlohmann/json.hpp>

namespace agentos {

namespace {

#ifdef _WIN32
struct WinsockGuard {
    bool initialized = false;
    WinsockGuard() {
        WSADATA data;
        initialized = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }
    ~WinsockGuard() {
        if (initialized) {
            WSACleanup();
        }
    }
    WinsockGuard(const WinsockGuard&) = delete;
    WinsockGuard& operator=(const WinsockGuard&) = delete;
};
#endif

std::string LocalHostname() {
    char buffer[256] = {0};
    if (gethostname(buffer, sizeof(buffer) - 1) != 0) {
        return {};
    }
    return std::string(buffer);
}

#ifdef _WIN32
std::vector<std::pair<std::string, std::vector<std::string>>> CollectInterfaces() {
    std::vector<std::pair<std::string, std::vector<std::string>>> result;

    ULONG buffer_size = 16 * 1024;
    std::vector<unsigned char> buffer(buffer_size);
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;

    DWORD ret = GetAdaptersAddresses(AF_INET, flags, nullptr,
        reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data()), &buffer_size);
    if (ret == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(buffer_size);
        ret = GetAdaptersAddresses(AF_INET, flags, nullptr,
            reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data()), &buffer_size);
    }
    if (ret != NO_ERROR) {
        return result;
    }

    for (auto* adapter = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());
         adapter != nullptr; adapter = adapter->Next) {
        if (adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
            continue;
        }
        if (adapter->OperStatus != IfOperStatusUp) {
            continue;
        }

        std::string name;
        if (adapter->FriendlyName != nullptr) {
            const wchar_t* wname = adapter->FriendlyName;
            int needed = WideCharToMultiByte(CP_UTF8, 0, wname, -1, nullptr, 0, nullptr, nullptr);
            if (needed > 1) {
                std::string converted(static_cast<size_t>(needed - 1), '\0');
                WideCharToMultiByte(CP_UTF8, 0, wname, -1, converted.data(), needed, nullptr, nullptr);
                name = std::move(converted);
            }
        }
        if (name.empty() && adapter->AdapterName != nullptr) {
            name = adapter->AdapterName;
        }

        std::vector<std::string> ipv4_addrs;
        for (auto* unicast = adapter->FirstUnicastAddress; unicast != nullptr; unicast = unicast->Next) {
            const auto* sa = unicast->Address.lpSockaddr;
            if (sa == nullptr || sa->sa_family != AF_INET) {
                continue;
            }
            const auto* addr_in = reinterpret_cast<const sockaddr_in*>(sa);
            char text[INET_ADDRSTRLEN] = {0};
            if (inet_ntop(AF_INET, &addr_in->sin_addr, text, sizeof(text)) != nullptr) {
                ipv4_addrs.emplace_back(text);
            }
        }

        if (!ipv4_addrs.empty()) {
            result.emplace_back(std::move(name), std::move(ipv4_addrs));
        }
    }

    return result;
}
#else
std::vector<std::pair<std::string, std::vector<std::string>>> CollectInterfaces() {
    std::vector<std::pair<std::string, std::vector<std::string>>> result;
    std::map<std::string, std::vector<std::string>> by_name;

    struct ifaddrs* head = nullptr;
    if (getifaddrs(&head) != 0 || head == nullptr) {
        return result;
    }

    for (auto* entry = head; entry != nullptr; entry = entry->ifa_next) {
        if (entry->ifa_addr == nullptr) {
            continue;
        }
        if ((entry->ifa_flags & IFF_LOOPBACK) != 0) {
            continue;
        }
        if ((entry->ifa_flags & IFF_UP) == 0) {
            continue;
        }
        if (entry->ifa_addr->sa_family != AF_INET) {
            continue;
        }

        const auto* addr_in = reinterpret_cast<const sockaddr_in*>(entry->ifa_addr);
        char text[INET_ADDRSTRLEN] = {0};
        if (inet_ntop(AF_INET, &addr_in->sin_addr, text, sizeof(text)) == nullptr) {
            continue;
        }
        const std::string name = entry->ifa_name != nullptr ? entry->ifa_name : "";
        by_name[name].emplace_back(text);
    }

    freeifaddrs(head);

    result.reserve(by_name.size());
    for (auto& [name, addrs] : by_name) {
        result.emplace_back(name, std::move(addrs));
    }
    return result;
}
#endif

}  // namespace

SkillManifest HostInfoSkill::manifest() const {
    return {
        .name = "host_info",
        .version = "0.1.0",
        .description = "Return the local machine's hostname and non-loopback IPv4 addresses.",
        .capabilities = {"host", "introspection"},
        .input_schema_json = R"({"type":"object","properties":{},"additionalProperties":false})",
        .output_schema_json = R"({"type":"object","required":["hostname","interfaces"]})",
        .risk_level = "low",
        .permissions = {},
        .supports_streaming = false,
        .idempotent = true,
        .timeout_ms = 1000,
    };
}

SkillResult HostInfoSkill::execute(const SkillCall& call) {
    (void)call;
    const auto started_at = std::chrono::steady_clock::now();

#ifdef _WIN32
    WinsockGuard winsock;
#endif

    nlohmann::json output;
    output["hostname"] = LocalHostname();

    auto interfaces = nlohmann::json::array();
    for (const auto& [name, addrs] : CollectInterfaces()) {
        nlohmann::json entry;
        entry["name"] = name;
        entry["ipv4"] = addrs;
        interfaces.push_back(std::move(entry));
    }
    output["interfaces"] = std::move(interfaces);

    return {
        .success = true,
        .json_output = output.dump(),
        .duration_ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started_at).count()),
    };
}

bool HostInfoSkill::healthy() const {
    return true;
}

}  // namespace agentos
