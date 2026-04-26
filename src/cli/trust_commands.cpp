#include "cli/trust_commands.hpp"

#include "core/audit/audit_logger.hpp"
#include "core/policy/approval_store.hpp"
#include "core/policy/role_catalog.hpp"
#include "trust/identity_manager.hpp"
#include "trust/pairing_invite_store.hpp"
#include "trust/pairing_manager.hpp"

#include <exception>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace agentos {

namespace {

std::map<std::string, std::string> ParseOptionsFromArgs(const int argc, char* argv[], const int start_index) {
    std::map<std::string, std::string> options;
    for (int index = start_index; index < argc; ++index) {
        std::string argument = argv[index];
        if (argument.rfind("--", 0) == 0) {
            argument = argument.substr(2);
        }

        const auto separator = argument.find('=');
        if (separator == std::string::npos) {
            continue;
        }

        options[argument.substr(0, separator)] = argument.substr(separator + 1);
    }
    return options;
}

std::vector<std::string> SplitCommaList(const std::string& value) {
    std::vector<std::string> items;
    std::stringstream input(value);
    std::string item;
    while (std::getline(input, item, ',')) {
        if (!item.empty()) {
            items.push_back(std::move(item));
        }
    }
    return items;
}

int ParseIntOption(const std::map<std::string, std::string>& options, const std::string& key, const int fallback) {
    if (!options.contains(key)) {
        return fallback;
    }

    try {
        return std::stoi(options.at(key));
    } catch (const std::exception&) {
        return fallback;
    }
}

void PrintTrustUsage() {
    std::cerr
        << "trust commands:\n"
        << "  agentos trust identity-add identity=<id> [user=<user>] [label=name]\n"
        << "  agentos trust identities\n"
        << "  agentos trust identity-remove identity=<id>\n"
        << "  agentos trust invite-create identity=<id> device=<id> [label=name] [user=<user>] [identity_label=name] [permissions=task.submit] [ttl_seconds=600]\n"
        << "  agentos trust invite-accept token=<token>\n"
        << "  agentos trust invites\n"
        << "  agentos trust pair identity=<id> device=<id> [label=name] [permissions=task.submit]\n"
        << "  agentos trust list\n"
        << "  agentos trust role-set role=<name> permissions=filesystem.read,agent.invoke\n"
        << "  agentos trust role-show role=<name>\n"
        << "  agentos trust user-role user=<user> roles=<role1,role2>\n"
        << "  agentos trust user-role-show user=<user>\n"
        << "  agentos trust role-remove role=<name>\n"
        << "  agentos trust user-role-remove user=<user>\n"
        << "  agentos trust roles\n"
        << "  agentos trust approval-request subject=<text> [reason=text] [requested_by=user]\n"
        << "  agentos trust approval-show approval=<id>\n"
        << "  agentos trust approval-approve approval=<id> [approved_by=user]\n"
        << "  agentos trust approval-revoke approval=<id> [approved_by=user]\n"
        << "  agentos trust approvals\n"
        << "  agentos trust device-label identity=<id> device=<id> label=<name>\n"
        << "  agentos trust device-show identity=<id> device=<id>\n"
        << "  agentos trust device-seen identity=<id> device=<id>\n"
        << "  agentos trust unblock identity=<id> device=<id>\n"
        << "  agentos trust block identity=<id> device=<id>\n"
        << "  agentos trust remove identity=<id> device=<id>\n";
}

void PrintTrustedPeer(const TrustedPeer& peer) {
    std::cout
        << peer.identity_id
        << " device=" << peer.device_id
        << " label=" << peer.label
        << " trust=" << ToString(peer.trust_level)
        << " paired_epoch_ms=" << peer.paired_epoch_ms
        << " last_seen_epoch_ms=" << peer.last_seen_epoch_ms
        << " permissions=";

    for (std::size_t index = 0; index < peer.permissions.size(); ++index) {
        if (index != 0) {
            std::cout << ',';
        }
        std::cout << peer.permissions[index];
    }
    std::cout << '\n';
}

void PrintPairingInvite(const PairingInvite& invite) {
    std::cout
        << "invite token=" << invite.token
        << " identity=" << invite.identity_id
        << " device=" << invite.device_id
        << " label=" << invite.label
        << " user=" << invite.user_id
        << " identity_label=" << invite.identity_label
        << " expires_epoch_ms=" << invite.expires_epoch_ms
        << " permissions=";

    for (std::size_t index = 0; index < invite.permissions.size(); ++index) {
        if (index != 0) {
            std::cout << ',';
        }
        std::cout << invite.permissions[index];
    }
    std::cout << '\n';
}

void PrintIdentity(const Identity& identity) {
    std::cout
        << identity.identity_id
        << " user=" << identity.user_id
        << " label=" << identity.label
        << '\n';
}

void PrintRoleDefinition(const RoleDefinition& role) {
    std::cout << "role " << role.role_name << " permissions=";
    for (std::size_t index = 0; index < role.permissions.size(); ++index) {
        if (index != 0) {
            std::cout << ',';
        }
        std::cout << role.permissions[index];
    }
    std::cout << '\n';
}

void PrintUserRoleAssignment(const UserRoleAssignment& assignment) {
    std::cout << "user " << assignment.user_id << " roles=";
    for (std::size_t index = 0; index < assignment.roles.size(); ++index) {
        if (index != 0) {
            std::cout << ',';
        }
        std::cout << assignment.roles[index];
    }
    std::cout << '\n';
}

void PrintApprovalRecord(const ApprovalRecord& record) {
    std::cout
        << "approval " << record.approval_id
        << " status=" << record.status
        << " subject=" << record.subject
        << " requested_by=" << record.requested_by
        << " approved_by=" << record.approved_by
        << " created_epoch_ms=" << record.created_epoch_ms
        << " decided_epoch_ms=" << record.decided_epoch_ms
        << " reason=" << record.reason
        << '\n';
}

}  // namespace

int RunTrustCommand(
    IdentityManager& identity_manager,
    PairingInviteStore& pairing_invite_store,
    PairingManager& pairing_manager,
    RoleCatalog& role_catalog,
    ApprovalStore& approval_store,
    AuditLogger& audit_logger,
    const int argc,
    char* argv[]) {
    if (argc < 3) {
        PrintTrustUsage();
        return 1;
    }

    const auto command = std::string(argv[2]);
    const auto options = ParseOptionsFromArgs(argc, argv, 3);

    if (command == "identities") {
        for (const auto& identity : identity_manager.list()) {
            PrintIdentity(identity);
        }
        return 0;
    }

    if (command == "identity-add") {
        const auto identity_id = options.contains("identity") ? options.at("identity") : "";
        if (identity_id.empty()) {
            std::cerr << "identity is required\n";
            return 1;
        }

        PrintIdentity(identity_manager.save(Identity{
            .identity_id = identity_id,
            .user_id = options.contains("user") ? options.at("user") : "remote-user",
            .label = options.contains("label") ? options.at("label") : identity_id,
        }));
        audit_logger.record_trust_event("identity-add", identity_id, "", true, "identity saved");
        return 0;
    }

    if (command == "identity-remove") {
        const auto identity_id = options.contains("identity") ? options.at("identity") : "";
        if (identity_id.empty()) {
            std::cerr << "identity is required\n";
            return 1;
        }

        const auto removed = identity_manager.remove(identity_id);
        std::cout << (removed ? "removed " : "not_found ") << identity_id << '\n';
        audit_logger.record_trust_event(
            "identity-remove",
            identity_id,
            "",
            removed,
            removed ? "identity removed" : "identity not found");
        return removed ? 0 : 1;
    }

    if (command == "list") {
        for (const auto& peer : pairing_manager.list()) {
            PrintTrustedPeer(peer);
        }
        return 0;
    }

    if (command == "roles") {
        for (const auto& role : role_catalog.list_roles()) {
            PrintRoleDefinition(role);
        }
        for (const auto& assignment : role_catalog.list_user_roles()) {
            PrintUserRoleAssignment(assignment);
        }
        return 0;
    }

    if (command == "approvals") {
        for (const auto& approval : approval_store.list()) {
            PrintApprovalRecord(approval);
        }
        return 0;
    }

    if (command == "approval-show") {
        const auto approval_id = options.contains("approval") ? options.at("approval") : "";
        if (approval_id.empty()) {
            std::cerr << "approval is required\n";
            return 1;
        }
        const auto record = approval_store.find(approval_id);
        if (!record.has_value()) {
            std::cout << "not_found approval " << approval_id << '\n';
            return 1;
        }
        PrintApprovalRecord(*record);
        return 0;
    }

    if (command == "approval-request") {
        const auto subject = options.contains("subject") ? options.at("subject") : "";
        if (subject.empty()) {
            std::cerr << "subject is required\n";
            return 1;
        }
        try {
            const auto record = approval_store.request(
                subject,
                options.contains("reason") ? options.at("reason") : "",
                options.contains("requested_by") ? options.at("requested_by") : "local-user");
            PrintApprovalRecord(record);
            audit_logger.record_trust_event("approval-request", record.approval_id, "", true, "approval requested");
        } catch (const std::exception& error) {
            std::cerr << "approval error: " << error.what() << '\n';
            return 1;
        }
        return 0;
    }

    if (command == "approval-approve" || command == "approval-revoke") {
        const auto approval_id = options.contains("approval") ? options.at("approval") : "";
        if (approval_id.empty()) {
            std::cerr << "approval is required\n";
            return 1;
        }
        const auto actor = options.contains("approved_by") ? options.at("approved_by") : "local-admin";
        const auto changed = command == "approval-approve"
            ? approval_store.approve(approval_id, actor)
            : approval_store.revoke(approval_id, actor);
        const auto record = approval_store.find(approval_id);
        if (record.has_value()) {
            PrintApprovalRecord(*record);
        } else {
            std::cout << "not_found approval " << approval_id << '\n';
        }
        audit_logger.record_trust_event(
            command,
            approval_id,
            "",
            changed,
            changed ? "approval status changed" : "approval not found");
        return changed ? 0 : 1;
    }

    if (command == "invites") {
        for (const auto& invite : pairing_invite_store.list_active()) {
            PrintPairingInvite(invite);
        }
        return 0;
    }

    if (command == "role-set") {
        const auto role = options.contains("role") ? options.at("role") : "";
        const auto permissions = options.contains("permissions")
            ? SplitCommaList(options.at("permissions"))
            : std::vector<std::string>{};
        if (role.empty()) {
            std::cerr << "role is required\n";
            return 1;
        }
        try {
            role_catalog.save_role(RoleDefinition{
                .role_name = role,
                .permissions = permissions,
            });
        } catch (const std::exception& error) {
            std::cerr << "role error: " << error.what() << '\n';
            return 1;
        }
        PrintRoleDefinition(RoleDefinition{.role_name = role, .permissions = permissions});
        audit_logger.record_trust_event("role-set", role, "", true, "role permissions saved");
        return 0;
    }

    if (command == "role-show") {
        const auto role_name = options.contains("role") ? options.at("role") : "";
        if (role_name.empty()) {
            std::cerr << "role is required\n";
            return 1;
        }
        for (const auto& role : role_catalog.list_roles()) {
            if (role.role_name == role_name) {
                PrintRoleDefinition(role);
                return 0;
            }
        }
        std::cout << "not_found role " << role_name << '\n';
        return 1;
    }

    if (command == "role-remove") {
        const auto role = options.contains("role") ? options.at("role") : "";
        if (role.empty()) {
            std::cerr << "role is required\n";
            return 1;
        }
        const auto removed = role_catalog.remove_role(role);
        std::cout << (removed ? "removed role " : "not_found role ") << role << '\n';
        audit_logger.record_trust_event(
            "role-remove",
            role,
            "",
            removed,
            removed ? "role removed" : "role not found");
        return removed ? 0 : 1;
    }

    if (command == "user-role") {
        const auto user = options.contains("user") ? options.at("user") : "";
        const auto roles = options.contains("roles") ? SplitCommaList(options.at("roles")) : std::vector<std::string>{};
        if (user.empty()) {
            std::cerr << "user is required\n";
            return 1;
        }
        try {
            role_catalog.assign_user_roles(UserRoleAssignment{
                .user_id = user,
                .roles = roles,
            });
        } catch (const std::exception& error) {
            std::cerr << "role error: " << error.what() << '\n';
            return 1;
        }
        PrintUserRoleAssignment(UserRoleAssignment{.user_id = user, .roles = roles});
        audit_logger.record_trust_event("user-role", user, "", true, "user roles saved");
        return 0;
    }

    if (command == "user-role-show") {
        const auto user = options.contains("user") ? options.at("user") : "";
        if (user.empty()) {
            std::cerr << "user is required\n";
            return 1;
        }
        for (const auto& assignment : role_catalog.list_user_roles()) {
            if (assignment.user_id == user) {
                PrintUserRoleAssignment(assignment);
                return 0;
            }
        }
        std::cout << "not_found user " << user << '\n';
        return 1;
    }

    if (command == "user-role-remove") {
        const auto user = options.contains("user") ? options.at("user") : "";
        if (user.empty()) {
            std::cerr << "user is required\n";
            return 1;
        }
        const auto removed = role_catalog.remove_user_roles(user);
        std::cout << (removed ? "removed user " : "not_found user ") << user << '\n';
        audit_logger.record_trust_event(
            "user-role-remove",
            user,
            "",
            removed,
            removed ? "user roles removed" : "user roles not found");
        return removed ? 0 : 1;
    }

    const auto identity = options.contains("identity") ? options.at("identity") : "";
    const auto device = options.contains("device") ? options.at("device") : "";

    if (command == "invite-accept") {
        const auto token = options.contains("token") ? options.at("token") : "";
        if (token.empty()) {
            std::cerr << "token is required\n";
            return 1;
        }

        const auto invite = pairing_invite_store.consume(token);
        if (!invite.has_value()) {
            std::cerr << "invite not found, expired, or already consumed\n";
            audit_logger.record_trust_event("invite-accept", "", "", false, "invite not consumable");
            return 1;
        }

        identity_manager.ensure(invite->identity_id, invite->user_id, invite->identity_label);
        PrintTrustedPeer(pairing_manager.pair(
            invite->identity_id,
            invite->device_id,
            invite->label,
            invite->permissions));
        audit_logger.record_trust_event(
            "invite-accept",
            invite->identity_id,
            invite->device_id,
            true,
            "pairing invite accepted");
        return 0;
    }

    if (identity.empty() || device.empty()) {
        std::cerr << "identity and device are required\n";
        return 1;
    }

    if (command == "device-show") {
        const auto peer = pairing_manager.find(identity, device);
        if (!peer.has_value()) {
            std::cout << "not_found " << identity << " device=" << device << '\n';
            return 1;
        }
        PrintTrustedPeer(*peer);
        return 0;
    }

    if (command == "invite-create") {
        const auto permissions = options.contains("permissions")
            ? SplitCommaList(options.at("permissions"))
            : std::vector<std::string>{"task.submit"};
        try {
            const auto invite = pairing_invite_store.create(
                identity,
                device,
                options.contains("label") ? options.at("label") : identity + ":" + device,
                options.contains("user") ? options.at("user") : "remote-user",
                options.contains("identity_label") ? options.at("identity_label") : identity,
                permissions,
                ParseIntOption(options, "ttl_seconds", 600));
            PrintPairingInvite(invite);
        } catch (const std::exception& error) {
            std::cerr << "invite error: " << error.what() << '\n';
            return 1;
        }
        audit_logger.record_trust_event("invite-create", identity, device, true, "pairing invite created");
        return 0;
    }

    if (command == "pair") {
        const auto label = options.contains("label") ? options.at("label") : identity + ":" + device;
        const bool identity_already_exists = identity_manager.find(identity).has_value();
        identity_manager.ensure(
            identity,
            options.contains("user") ? options.at("user") : "remote-user",
            options.contains("identity_label") ? options.at("identity_label") : identity);
        if (!identity_already_exists) {
            audit_logger.record_trust_event("identity-auto-create", identity, "", true, "identity created during pairing");
        }
        const auto permissions = options.contains("permissions")
            ? SplitCommaList(options.at("permissions"))
            : std::vector<std::string>{"task.submit"};
        PrintTrustedPeer(pairing_manager.pair(identity, device, label, permissions));
        audit_logger.record_trust_event("pair", identity, device, true, "peer paired");
        return 0;
    }

    if (command == "block") {
        pairing_manager.block(identity, device);
        std::cout << "blocked " << identity << " device=" << device << '\n';
        audit_logger.record_trust_event("block", identity, device, true, "peer blocked");
        return 0;
    }

    if (command == "unblock") {
        const auto changed = pairing_manager.unblock(identity, device);
        std::cout << (changed ? "unblocked " : "not_found ") << identity << " device=" << device << '\n';
        audit_logger.record_trust_event(
            "unblock",
            identity,
            device,
            changed,
            changed ? "peer unblocked" : "peer not found");
        return changed ? 0 : 1;
    }

    if (command == "device-label") {
        const auto label = options.contains("label") ? options.at("label") : "";
        if (label.empty()) {
            std::cerr << "label is required\n";
            return 1;
        }
        const auto changed = pairing_manager.rename_device(identity, device, label);
        std::cout << (changed ? "renamed " : "not_found ") << identity << " device=" << device << " label=" << label << '\n';
        audit_logger.record_trust_event(
            "device-label",
            identity,
            device,
            changed,
            changed ? "device label updated" : "peer not found");
        return changed ? 0 : 1;
    }

    if (command == "device-seen") {
        const auto changed = pairing_manager.mark_seen(identity, device);
        std::cout << (changed ? "seen " : "not_found ") << identity << " device=" << device << '\n';
        audit_logger.record_trust_event(
            "device-seen",
            identity,
            device,
            changed,
            changed ? "device last_seen updated" : "peer not found");
        return changed ? 0 : 1;
    }

    if (command == "remove") {
        pairing_manager.remove(identity, device);
        std::cout << "removed " << identity << " device=" << device << '\n';
        audit_logger.record_trust_event("remove", identity, device, true, "peer removed");
        return 0;
    }

    PrintTrustUsage();
    return 1;
}

}  // namespace agentos
