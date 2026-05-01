#pragma once

#include <QDialog>

namespace fc::ui {

// Application-level settings: theme picker today, more knobs to follow as
// Phase 4 polish lands (sync interval, default reply mode, log level).
//
// Persists via Theme/QSettings so re-opening the app keeps the user's choice.
class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(QWidget* parent = nullptr);

private slots:
    void onThemeChanged(int idx);
    void onHtmlPreviewChanged(int idx);
};

}  // namespace fc::ui
