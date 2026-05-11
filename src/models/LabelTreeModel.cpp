#include "LabelTreeModel.h"

#include <QHash>
#include <QStringList>

#include <functional>

namespace fc {

struct LabelTreeModel::Node {
    QString id;
    QString accountId;       // empty for cross-account synthetic + section rows
    QString name;            // pretty label segment ("Booking" not "Travel/Booking")
    QString fullName;        // full Gmail label name (or "__account/<aid>", "__section/<aid>/folders", ...)
    QString type;            // "system" | "user" | "section" | "synthetic" | "account"
    QString colorBg;         // Gmail-assigned background hex (may be empty)
    QString colorFg;         // Gmail-assigned text hex (may be empty)
    int unreadCount = 0;     // self only — what the API reported for this id
    int totalCount  = 0;
    int aggUnread   = 0;     // self + recursive descendants — for display
    int aggTotal    = 0;
    Node* parent = nullptr;
    std::vector<NodePtr> children;
};

namespace {

// Pretty names + a stable sort order for system labels.
struct SystemEntry { const char* id; const char* display; };
constexpr SystemEntry kSystem[] = {
    {"INBOX",     "Inbox"},
    {"STARRED",   "Starred"},
    {"IMPORTANT", "Important"},
    {"SENT",      "Sent"},
    {"DRAFT",     "Drafts"},
    {"SPAM",      "Spam"},
    {"TRASH",     "Trash"},
};

}  // namespace

LabelTreeModel::LabelTreeModel(QObject* parent)
    : QAbstractItemModel(parent), root_(new Node{}) {
    // accountId_ starts empty; reload() will produce an empty tree
    // until MainWindow wires it via setAccountId() on the first
    // account switch.
    reload();
}

LabelTreeModel::~LabelTreeModel() { delete root_; }

void LabelTreeModel::setAccountId(const QString& accountId) {
    if (accountId_ == accountId) return;
    accountId_ = accountId;
    // In multi-account mode (accounts_ non-empty), the tree contains
    // every account already — just nudge a dataChanged on the visible
    // rows in case any display hinges on "is this the active account"
    // (today nothing does; this is future-proofing).
    if (!accounts_.isEmpty()) return;
    reload();
}

QString LabelTreeModel::accountId() const { return accountId_; }

void LabelTreeModel::setAccounts(const QList<AccountDescriptor>& accounts) {
    accounts_ = accounts;
    reload();
}

void LabelTreeModel::appendAccountSections(
        Node* parent,
        const QString& accountId,
        const std::vector<fc::cache::LabelRow>& rows) {
    auto folders = std::make_unique<Node>();
    folders->accountId = accountId;
    folders->name      = QStringLiteral("Folders");
    folders->fullName  = accountId.isEmpty()
        ? QStringLiteral("__folders")
        : QStringLiteral("__section/") + accountId + QStringLiteral("/folders");
    folders->type      = QStringLiteral("section");
    folders->parent    = parent;

    auto labels = std::make_unique<Node>();
    labels->accountId = accountId;
    labels->name      = QStringLiteral("Labels");
    labels->fullName  = accountId.isEmpty()
        ? QStringLiteral("__labels")
        : QStringLiteral("__section/") + accountId + QStringLiteral("/labels");
    labels->type      = QStringLiteral("section");
    labels->parent    = parent;

    QHash<QString, const fc::cache::LabelRow*> byId;
    for (const auto& r : rows) byId.insert(r.id, &r);

    for (const auto& s : kSystem) {
        const QString id = QString::fromLatin1(s.id);
        if (auto it = byId.constFind(id); it != byId.constEnd()) {
            auto n = std::make_unique<Node>();
            n->id          = id;
            n->accountId   = accountId;
            n->name        = QString::fromLatin1(s.display);
            n->fullName    = id;
            n->type        = QStringLiteral("system");
            n->unreadCount = (*it)->unreadCount;
            n->totalCount  = (*it)->totalCount;
            n->parent      = folders.get();
            folders->children.push_back(std::move(n));
        }
    }

    {
        auto allMail = std::make_unique<Node>();
        allMail->id        = QStringLiteral("__all_mail");
        allMail->accountId = accountId;
        allMail->name      = QStringLiteral("All Mail");
        allMail->fullName  = QStringLiteral("__all_mail");
        allMail->type      = QStringLiteral("system");
        allMail->parent    = folders.get();
        folders->children.push_back(std::move(allMail));
    }

    QHash<QString, Node*> pathIndex;
    for (const auto& r : rows) {
        if (r.type != QLatin1String("user")) continue;
        QStringList parts = r.name.split('/', Qt::SkipEmptyParts);
        QString accum;
        Node* parentNode = labels.get();
        for (int i = 0; i < parts.size(); ++i) {
            if (!accum.isEmpty()) accum += QLatin1Char('/');
            accum += parts[i];

            Node* existing = pathIndex.value(accum, nullptr);
            if (existing) { parentNode = existing; continue; }

            auto n = std::make_unique<Node>();
            n->name      = parts[i];
            n->accountId = accountId;
            n->fullName  = accum;
            n->type      = QStringLiteral("user");
            n->parent    = parentNode;
            const bool isLeaf = (i == parts.size() - 1);
            if (isLeaf) {
                n->id          = r.id;
                n->unreadCount = r.unreadCount;
                n->totalCount  = r.totalCount;
                n->colorBg     = r.colorBg;
                n->colorFg     = r.colorFg;
            }
            Node* raw = n.get();
            parentNode->children.push_back(std::move(n));
            pathIndex.insert(accum, raw);
            parentNode = raw;
        }
    }

    parent->children.push_back(std::move(folders));
    parent->children.push_back(std::move(labels));
}

void LabelTreeModel::reload() {
    // Collect fresh label rows up front — used by sameLabelSet for
    // the fast-path decision AND by the structural rebuild below.
    // Multi-account mode (accounts_ non-empty) queries every account;
    // legacy mode queries just accountId_.
    std::vector<std::pair<QString, std::vector<fc::cache::LabelRow>>> perAccount;
    if (!accounts_.isEmpty()) {
        perAccount.reserve(accounts_.size());
        for (const auto& acc : accounts_) {
            perAccount.emplace_back(acc.id,
                fc::cache::LabelRepository::all(acc.id));
        }
    } else if (!accountId_.isEmpty()) {
        perAccount.emplace_back(accountId_,
            fc::cache::LabelRepository::all(accountId_));
    }

    // Fast path: same label set as currently in the tree means counts
    // can be refreshed in place (no beginResetModel, no collapsed
    // branches, no dropped selection). Big win during top-up bursts
    // where labelsUpdated fires per batch but the set never changes.
    if (sameLabelSet(perAccount)) {
        refreshCountsInPlace(perAccount);
        return;
    }

    captureShadow();
    beginResetModel();
    root_->children.clear();

    qInfo("LabelTreeModel::reload: accounts=%zu activeFocus='%s' (structural)",
          perAccount.size(), qUtf8Printable(accountId_));

    // Cross-account "All Inboxes" stays at the very top of the tree.
    {
        auto allInboxes = std::make_unique<Node>();
        allInboxes->id       = QStringLiteral("__all_inboxes");
        allInboxes->name     = QStringLiteral("All Inboxes");
        allInboxes->fullName = QStringLiteral("__all_inboxes");
        allInboxes->type     = QStringLiteral("synthetic");
        allInboxes->parent   = root_;
        root_->children.push_back(std::move(allInboxes));
    }

    if (!accounts_.isEmpty()) {
        // Multi-account: one expandable branch per signed-in account.
        // Each branch contains its own Folders + Labels sections.
        for (qsizetype i = 0; i < accounts_.size(); ++i) {
            const auto& acc = accounts_[i];
            const auto& rows = perAccount[i].second;

            auto accountNode = std::make_unique<Node>();
            accountNode->accountId = acc.id;
            accountNode->name      = acc.displayName.isEmpty()
                ? acc.email : acc.displayName;
            accountNode->fullName  = QStringLiteral("__account/") + acc.id;
            accountNode->type      = QStringLiteral("account");
            accountNode->parent    = root_;

            appendAccountSections(accountNode.get(), acc.id, rows);
            root_->children.push_back(std::move(accountNode));
        }
    } else if (!perAccount.empty()) {
        // Legacy single-account fallback (used during the brief startup
        // window before AccountManager populates, plus unit tests that
        // drive setAccountId directly).
        appendAccountSections(root_, /*accountId=*/QString(), perAccount.front().second);
    }

    // DFS that rolls each leaf's count up to its ancestors. Mirrors Gmail
    // web: a parent label like "Travel" shows self + every descendant's
    // unread. Done after the tree is built so per-node `unreadCount`
    // (what the API reported for this exact label id) stays untouched.
    std::function<void(Node*)> aggregate = [&](Node* n) {
        n->aggUnread = n->unreadCount;
        n->aggTotal  = n->totalCount;
        for (const auto& c : n->children) {
            aggregate(c.get());
            n->aggUnread += c->aggUnread;
            n->aggTotal  += c->aggTotal;
        }
    };
    for (const auto& top : root_->children) aggregate(top.get());

    endResetModel();
}

QModelIndex LabelTreeModel::index(int row, int column,
                                  const QModelIndex& parent) const {
    if (column != 0) return {};
    Node* p = parent.isValid() ? static_cast<Node*>(parent.internalPointer()) : root_;
    if (row < 0 || row >= int(p->children.size())) return {};
    return createIndex(row, column, p->children[row].get());
}

QModelIndex LabelTreeModel::parent(const QModelIndex& child) const {
    if (!child.isValid()) return {};
    Node* n = static_cast<Node*>(child.internalPointer());
    if (!n || !n->parent || n->parent == root_) return {};
    Node* gp = n->parent->parent;
    if (!gp) return {};
    for (int i = 0; i < int(gp->children.size()); ++i) {
        if (gp->children[i].get() == n->parent) return createIndex(i, 0, n->parent);
    }
    return {};
}

int LabelTreeModel::rowCount(const QModelIndex& parent) const {
    if (parent.column() > 0) return 0;
    Node* p = parent.isValid() ? static_cast<Node*>(parent.internalPointer()) : root_;
    return int(p->children.size());
}

int LabelTreeModel::columnCount(const QModelIndex&) const { return 1; }

QVariant LabelTreeModel::data(const QModelIndex& idx, int role) const {
    if (!idx.isValid()) return {};
    Node* n = static_cast<Node*>(idx.internalPointer());
    switch (role) {
        case Qt::DisplayRole: {
            // Synthetic section rows ("Folders" / "Labels") render plain
            // — adding "(123)" on a banner-style header looks cluttered.
            if (n->type == QLatin1String("section")) return n->name;
            // Show the rolled-up unread count for parent rows and for
            // synthetic intermediate rows that have no real label id of
            // their own. Leaves keep their own count (which equals the
            // aggregate when there are no children).
            int u = n->aggUnread;
            // Sync-in-flight fallback: if a fresh reload has reset this
            // node's count to 0 but we had a positive count before sync
            // started, show the last known value with an ellipsis. Keeps
            // the sidebar steady through the brief "(empty)" gap that
            // reload() introduces between begin/endResetModel.
            if (syncing_ && u == 0 && !n->fullName.isEmpty()) {
                const auto it = shadowUnread_.constFind(n->fullName);
                if (it != shadowUnread_.constEnd()) u = it.value();
            }
            if (u <= 0) return n->name;
            return syncing_
                ? QStringLiteral("%1 (%2…)").arg(n->name).arg(u)
                : QStringLiteral("%1 (%2)").arg(n->name).arg(u);
        }
        case IdRole:        return n->id;
        case NameRole:      return n->fullName;
        case TypeRole:      return n->type;
        case UnreadRole:    return n->aggUnread;
        case TotalRole:     return n->aggTotal;
        case ColorBgRole:   return n->colorBg;
        case ColorFgRole:   return n->colorFg;
        case AccountIdRole: return n->accountId;
        default:            return {};
    }
}

QHash<int, QByteArray> LabelTreeModel::roleNames() const {
    return {
        {IdRole,        "id"},
        {NameRole,      "name"},
        {TypeRole,      "type"},
        {UnreadRole,    "unread"},
        {TotalRole,     "total"},
        {AccountIdRole, "accountId"},
    };
}

QString LabelTreeModel::labelIdAt(const QModelIndex& idx) const {
    if (!idx.isValid()) return {};
    return static_cast<Node*>(idx.internalPointer())->id;
}

QString LabelTreeModel::accountIdAt(const QModelIndex& idx) const {
    if (!idx.isValid()) return {};
    return static_cast<Node*>(idx.internalPointer())->accountId;
}

void LabelTreeModel::setSyncing(bool on) {
    if (syncing_ == on) return;
    syncing_ = on;
    if (on) {
        // Snapshot current counts so the upcoming reload(s) can fall
        // back to them when the cache momentarily reports zero.
        captureShadow();
    }
    // Trigger a re-render of every visible row so the suffix updates.
    // Sidebar trees are tiny — walking each section + child is cheap.
    const int sections = rowCount();
    if (sections > 0) {
        emit dataChanged(index(0, 0), index(sections - 1, 0),
                         {Qt::DisplayRole});
        for (int i = 0; i < sections; ++i) {
            const auto sec = index(i, 0);
            const int kids = rowCount(sec);
            if (kids > 0) {
                emit dataChanged(index(0, 0, sec), index(kids - 1, 0, sec),
                                 {Qt::DisplayRole});
            }
        }
    }
}

bool LabelTreeModel::sameLabelSet(
        const std::vector<std::pair<QString, std::vector<fc::cache::LabelRow>>>& perAccount) const {
    // Compare (accountId, labelId) pairs between the current tree and
    // the fresh rows. The same id (e.g. "INBOX") exists in every
    // account, so the comparison must be account-scoped or we'd
    // falsely accept "moved INBOX between accounts" as no change.
    QSet<QString> treeKeys;
    std::function<void(Node*)> collect = [&](Node* n) {
        if (!n) return;
        if (!n->id.isEmpty()) {
            treeKeys.insert(n->accountId + QLatin1Char('\0') + n->id);
        }
        for (auto& c : n->children) collect(c.get());
    };
    collect(root_);

    // Strip synthetic ids ("__all_mail" / "__all_inboxes") — they're
    // injected unconditionally during the structural build and never
    // appear in LabelRepository::all output.
    for (auto it = treeKeys.begin(); it != treeKeys.end();) {
        if (it->endsWith(QStringLiteral("\0__all_mail"))
            || it->endsWith(QStringLiteral("\0__all_inboxes"))
            || *it == QStringLiteral("\0__all_inboxes")) {
            it = treeKeys.erase(it);
        } else {
            ++it;
        }
    }

    QSet<QString> rowKeys;
    for (const auto& [aid, rows] : perAccount) {
        for (const auto& r : rows) {
            rowKeys.insert(aid + QLatin1Char('\0') + r.id);
        }
    }

    return treeKeys == rowKeys;
}

void LabelTreeModel::refreshCountsInPlace(
        const std::vector<std::pair<QString, std::vector<fc::cache::LabelRow>>>& perAccount) {
    // Update self-counts on each leaf node from the fresh rows, then
    // rebuild the aggregate (parent = self + recursive descendants)
    // pass, then emit dataChanged so the QTreeView repaints visible
    // nodes' "(N)" suffix and any colour-tracked roles. Per-account
    // index keyed on accountId+id so the same Gmail id (INBOX) in two
    // accounts doesn't cross-contaminate.
    QHash<QString, const fc::cache::LabelRow*> byKey;
    for (const auto& [aid, rows] : perAccount) {
        for (const auto& r : rows) {
            byKey.insert(aid + QLatin1Char('\0') + r.id, &r);
        }
    }

    std::function<void(Node*)> applyLeaf = [&](Node* n) {
        if (!n) return;
        if (!n->id.isEmpty()) {
            const QString key = n->accountId + QLatin1Char('\0') + n->id;
            if (auto it = byKey.constFind(key); it != byKey.constEnd()) {
                n->unreadCount = (*it)->unreadCount;
                n->totalCount  = (*it)->totalCount;
                n->colorBg     = (*it)->colorBg;
                n->colorFg     = (*it)->colorFg;
            } else if (n->id == QLatin1String("__all_mail")
                    || n->id == QLatin1String("__all_inboxes")) {
                // synthetic — no server-reported counts
            } else {
                // Label disappeared from the row set. Shouldn't happen
                // under sameLabelSet=true, but be defensive.
                n->unreadCount = 0;
                n->totalCount  = 0;
            }
        }
        for (auto& c : n->children) applyLeaf(c.get());
    };
    applyLeaf(root_);

    std::function<void(Node*)> aggregate = [&](Node* n) {
        n->aggUnread = n->unreadCount;
        n->aggTotal  = n->totalCount;
        for (auto& c : n->children) {
            aggregate(c.get());
            n->aggUnread += c->aggUnread;
            n->aggTotal  += c->aggTotal;
        }
    };
    for (auto& top : root_->children) aggregate(top.get());

    // dataChanged on every visible row. Sidebar trees are small (~150
    // nodes total) and the roles we touch are all on column 0, so a
    // single emit covering the full row range is cheaper than walking
    // per-node emits. The view only repaints currently-visible items
    // either way.
    const int topN = int(root_->children.size());
    if (topN == 0) return;
    const QVector<int> roles{Qt::DisplayRole, UnreadRole, TotalRole,
                              ColorBgRole, ColorFgRole};
    std::function<void(const QModelIndex&)> emitFor = [&](const QModelIndex& parent) {
        const int n = rowCount(parent);
        if (n == 0) return;
        emit dataChanged(index(0, 0, parent), index(n - 1, 0, parent), roles);
        for (int i = 0; i < n; ++i) emitFor(index(i, 0, parent));
    };
    emitFor({});
}

void LabelTreeModel::captureShadow() {
    shadowUnread_.clear();
    std::function<void(Node*)> visit = [&](Node* n) {
        if (!n) return;
        if (!n->fullName.isEmpty()) {
            shadowUnread_.insert(n->fullName, n->aggUnread);
        }
        for (auto& c : n->children) visit(c.get());
    };
    visit(root_);
}

}  // namespace fc
