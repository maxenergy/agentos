#pragma once

namespace agentos {

class ApprovalStore;
class AuditLogger;
class IdentityManager;
class PairingInviteStore;
class PairingManager;
class RoleCatalog;

int RunTrustCommand(
    IdentityManager& identity_manager,
    PairingInviteStore& pairing_invite_store,
    PairingManager& pairing_manager,
    RoleCatalog& role_catalog,
    ApprovalStore& approval_store,
    AuditLogger& audit_logger,
    int argc,
    char* argv[]);

}  // namespace agentos
