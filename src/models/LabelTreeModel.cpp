#include "LabelTreeModel.h"

#include <QHash>
#include <QStringList>

namespace fc {

struct LabelTreeModel::Node {
    QString id;
    QString name;            // pretty label segment ("Booking" not "Travel/Booking")
    QString fullName;        // full Gmail label name
    QString type;            // "system" | "user"
    int unreadCount = 0;
    int totalCount  = 0;
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
    reload();
}

LabelTreeModel::~LabelTreeModel() { delete root_; }

void LabelTreeModel::reload() {
    beginResetModel();
    root_->children.clear();

    auto rows = fc::cache::LabelRepository::all();

    // System labels first, in canonical order.
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
            n->parent      = root_;
            root_->children.push_back(std::move(n));
        }
    }

    // User labels: build tree by splitting on '/'.
    QHash<QString, Node*> pathIndex;  // fullName -> Node
    for (auto& r : rows) {
        if (r.type != QLatin1String("user")) continue;
        QStringList parts = r.name.split('/', Qt::SkipEmptyParts);
        QString accum;
        Node* parentNode = root_;
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
            // Only the leaf carries a real id + counts.
            const bool isLeaf = (i == parts.size() - 1);
            if (isLeaf) {
                n->id          = r.id;
                n->unreadCount = r.unreadCount;
                n->totalCount  = r.totalCount;
            }
            Node* raw = n.get();
            parentNode->children.push_back(std::move(n));
            pathIndex.insert(accum, raw);
            parentNode = raw;
        }
    }

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
        case Qt::DisplayRole:
            return n->unreadCount > 0
                ? QStringLiteral("%1 (%2)").arg(n->name).arg(n->unreadCount)
                : n->name;
        case IdRole:        return n->id;
        case NameRole:      return n->fullName;
        case TypeRole:      return n->type;
        case UnreadRole:    return n->unreadCount;
        case TotalRole:     return n->totalCount;
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

}  // namespace fc
