#pragma once

#include <QWidget>

class QTreeView;

namespace fc { class LabelTreeModel; }

namespace fc::ui {

// Left pane: QTreeView over LabelTreeModel. Emits signals so MainWindow can
// drive the centre pane and run label CRUD.
class SidebarWidget : public QWidget {
    Q_OBJECT
public:
    explicit SidebarWidget(QWidget* parent = nullptr);

    fc::LabelTreeModel* model() const;
    void selectLabel(const QString& id);

signals:
    void labelSelected(const QString& id);
    void requestCreateLabel(const QString& parentLabelId);
    void requestRenameLabel(const QString& labelId);
    void requestDeleteLabel(const QString& labelId);

private slots:
    void onClicked(const QModelIndex& idx);
    void onContextMenu(const QPoint& p);

private:
    QTreeView*           tree_;
    fc::LabelTreeModel*  model_;
};

}  // namespace fc::ui
