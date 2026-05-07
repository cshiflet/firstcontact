#pragma once

#include <QString>

namespace fc::auth { class OAuthClient; class TokenStore; class ClientConfig; }
namespace fc::api  { class RestClient; class GmailClient; }
namespace fc::sync { class SyncService; }

namespace fc::account {

// Per-account API + sync stack. Step 4 introduces the shape; step 6
// wires Bootstrap to construct one of these per signed-in account and
// retire the single global instances. v1's workers (OutboxWorker /
// PendingOpsWorker / DraftSync) stay shared at the cross-account
// drain layer because they only need a GmailClient pinned to the
// row's account_id at dispatch time, which AccountContext::gmail
// supplies.
//
// Ownership: the `AccountManager` holds a QHash<QString, AccountContext*>
// keyed by account id. Each context owns its members raw and deletes
// them via QObject::deleteLater on destruction. The context's lifetime
// matches "this account is signed in" — sign-out destroys the context.
struct AccountContext {
    QString accountId;
    fc::auth::OAuthClient*  auth   = nullptr;
    fc::api::RestClient*    rest   = nullptr;
    fc::api::GmailClient*   gmail  = nullptr;
    fc::sync::SyncService*  sync   = nullptr;

    // Convenience flags. `degraded` flips on after a refresh-token
    // failure: the cache is still valid but the account can't sync /
    // send; UI surfaces a "Re-sign-in" affordance.
    bool degraded = false;
};

}  // namespace fc::account
