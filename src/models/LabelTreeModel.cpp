#include "LabelTreeModel.h"

#include <QHash>
#include <QStringList>

#include <functional>

namespace fc {

struct LabelTreeModel::Node {
    QString id;
    QString name;            // pretty label segment ("Booking" not "Travel/Booking")
    QString fullName;        // full Gmail label name
    QString type;            // "system" | "user" | "section"
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
    reload();
}

QString LabelTreeModel::accountId() const { return accountId_; }

void LabelTreeModel::reload() {
    auto rows = accountId_.isEmpty()
        ? std::vector<fc::cache::LabelRow>{}
        : fc::cache::LabelRepository::all(accountId_);

    // Fast path: if the label set is identical to what's already in
    // the tree, just refresh counts in place (dataChanged on each
    // touched node) and skip the beginResetModel reset. The
    // structural reset collapses every expanded branch and drops the
    // current selection, both of which Qt+SidebarWidget then have to
    // recover via signal handlers — and the reset itself rebuilds 144
    // nodes, allocates Node owners, and re-runs the aggregate pass.
    // For top-up bursts on a busy account this used to fire 30+ times
    // per scroll, each a no-op structural reset just to land a few
    // tweaked unread counts.
    if (sameLabelSet(rows)) {
        refreshCountsInPlace(rows);
        return;
    }

    captureShadow();
    beginResetModel();
    root_->children.clear();

    qInfo("LabelTreeModel::reload: accountId='%s' rows=%zu (structural)",
          qUtf8Printable(accountId_), rows.size());

    // v2: a synthetic "All Inboxes" node at the top of the tree.
    // Clicking it asks MainWindow to switch to the cross-account view.
    {
        auto allInboxes = std::make_unique<Node>();
        allInboxes->id       = QStringLiteral("__all_inboxes");
        allInboxes->name     = QStringLiteral("All Inboxes");
        allInboxes->fullName = QStringLiteral("__all_inboxes");
        allInboxes->type     = QStringLiteral("synthetic");
        allInboxes->parent   = root_;
        root_->children.push_back(std::move(allInboxes));
    }

    // Two synthetic section nodes at the top — "Folders" wraps the
    // canonical Gmail system labels, "Labels" wraps everything the user
    // created. Modeling them as real tree nodes (instead of painting
    // banner rows) means QTreeView's built-in expand / collapse plus
    // the click handler in SidebarWidget can toggle entire sections,
    // and the aggregateCounts pass naturally rolls each section's
    // unread / total counts up to the synthetic parent.
    auto folders = std::make_unique<Node>();
    folders->name     = QStringLiteral("Folders");
    folders->fullName = QStringLiteral("__folders");
    folders->type     = QStringLiteral("section");
    folders->parent   = root_;

    auto labels = std::make_unique<Node>();
    labels->name      = QStringLiteral("Labels");
    labels->fullName  = QStringLiteral("__labels");
    labels->type      = QStringLiteral("section");
    labels->parent    = root_;

    // System labels first, in canonical order, parented under "Folders".
    QHash<QString, fc::cache::LabelRow*> byId;
    for (auto& r : rows) byId.insert(r.id, &r);

    for (const auto& s : kSystem) {
        const QString id = QString::fromLatin1(s.id);
        if (auto it = byId.constFind(id); it != byId.constEnd()) {
            auto n = std::make_unique<Node>();
            n->id          = id;
            n->name        = QString::fromLatin1(s.display);
            n->fullName    = id;
            n->type        = QStringLiteral("system");
            n->unreadCount = (*it)->unreadCount;
            n->totalCount  = (*it)->totalCount;
            n->parent      = folders.get();
            folders->children.push_back(std::move(n));
        }
    }

    // Synthetic "All Mail" entry under Folders — Gmail's All Mail is
    // a virtual view (not a real label), so we surface it as a
    // synthetic node here. MainWindow handles the "__all_mail" id by
    // routing through MessageListModel's all-mail source +
    // SyncService::topUpLabel("__all_mail") for server-side
    // pagination. No server-reported counts available, so unread /
    // total stay at the model's defaults (0).
    {
        auto allMail = std::make_unique<Node>();
        allMail->id        = QStringLiteral("__all_mail");
        allMail->name      = QStringLiteral("All Mail");
        allMail->fullName  = QStringLiteral("__all_mail");
        allMail->type      = QStringLiteral("system");
        allMail->parent    = folders.get();
        folders->children.push_back(std::move(allMail));
    }

    // User labels: build tree by splitting on '/', parented under "Labels".
    QHash<QString, Node*> pathIndex;  // fullName -> Node
    for (auto& r : rows) {
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
            n->fullName  = accum;
            n->type      = QStringLiteral("user");
            n->parent    = parentNode;
            // Only the leaf carries a real id + counts + colours. The
            // synthetic intermediate nodes (e.g. "Travel" when the user
            // only created "Travel/Booking") have no Gmail-side identity
            // so there's nothing to colour.
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

    root_->children.push_back(std::move(folders));
    root_->children.push_back(std::move(labels));

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
        default:            return {};
    }
}

QHash<int, QByteArray> LabelTreeModel::roleNames() const {
    return {
        {IdRole,     "id"},
        {NameRole,   "name"},
        {TypeRole,   "type"},
        {UnreadRole, "unread"},
        {TotalRole,  "total"},
    };
}

QString LabelTreeModel::labelIdAt(const QModelIndex& idx) const {
    if (!idx.isValid()) return {};
    return static_cast<Node*>(idx.internalPointer())->id;
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
        const std::vector<fc::cache::LabelRow>& rows) const {
    // Compare the set of label ids in the incoming rows to the set
    // already in the tree. Walks the tree once and collects every
    // leaf with a non-empty id (system rows under "Folders" + each
    // user-label leaf under "Labels"). Synthetic intermediate user
    // nodes (the "Travel" in "Travel/Booking") have empty ids, so
    // they don't participate.
    QSet<QString> treeIds;
    std::function<void(Node*)> collect = [&](Node* n) {
        if (!n) return;
        if (!n->id.isEmpty()) treeIds.insert(n->id);
        for (auto& c : n->children) collect(c.get());
    };
    collect(root_);

    // "__all_mail" is a synthetic id we add unconditionally — exclude
    // it from the comparison so it doesn't cause spurious mismatches
    // (it's never in `rows`).
    treeIds.remove(QStringLiteral("__all_mail"));
    treeIds.remove(QStringLiteral("__all_inboxes"));

    QSet<QString> rowIds;
    for (const auto& r : rows) rowIds.insert(r.id);

    return treeIds == rowIds;
}

void LabelTreeModel::refreshCountsInPlace(
        const std::vector<fc::cache::LabelRow>& rows) {
    // Update self-counts on each leaf node from the fresh rows, then
    // rebuild the aggregate (parent = self + recursive descendants)
    // pass, then emit dataChanged on every node so the QTreeView
    // repaints the "(N)" suffix and any colour-tracked role.
    QHash<QString, const fc::cache::LabelRow*> byId;
    for (const auto& r : rows) byId.insert(r.id, &r);

    std::function<void(Node*)> applyLeaf = [&](Node* n) {
        if (!n) return;
        if (!n->id.isEmpty()) {
            if (auto it = byId.constFind(n->id); it != byId.constEnd()) {
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
