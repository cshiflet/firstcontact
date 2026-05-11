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

signals:
    // Emitted when the user clicks "Manage cache…" in the Storage
    // section. The owning window (MainWindow) opens
    // CacheManagerDialog — SettingsDialog itself doesn't take an
    // AccountManager dependency.
    void cacheManagerRequested();
    // Emitted when the user clicks "Recompress now…". MainWindow
    // owns the per-account picker (if there's more than one
    // signed-in account) and the BodyCompressionWorker lifecycle.
    void recompressRequested();

private slots:
    void onThemeChanged(int idx);
    void onHtmlPreviewChanged(int idx);
};

}  // namespace fc::ui
