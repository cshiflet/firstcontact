#pragma once

#include <QDialog>
#include <QPointer>
#include <QString>

class QLabel;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;

namespace fc::cache    { class BodyCompressionWorker; }
namespace fc::account  { class AccountManager; }

namespace fc::ui {

// Hosts a BodyCompressionWorker and surfaces its progress signals
// on top of whatever dialog kicked it off (Settings → "Recompress…"
// or the first-time compressionPromptDue threshold), so errors
// don't get hidden behind the parent dialog.
class CompressionProgressDialog : public QDialog {
    Q_OBJECT
public:
    CompressionProgressDialog(fc::account::AccountManager* accounts,
                              const QString& accountId,
                              QWidget* parent = nullptr);

    // Must be called BEFORE worker->start() — connections after
    // start could miss early progress events.
    void attachWorker(fc::cache::BodyCompressionWorker* worker);

private slots:
    void onProgress(const QString& aid, int done, int total);
    void onFinished(const QString& aid, int rewroteCount, qint64 savedBytes);
    void onFailed(const QString& aid, const QString& reason);

private:
    fc::account::AccountManager* accounts_ = nullptr;
    QString accountId_;
    qint64  beforeBytes_ = 0;   // captured at construction

    QLabel*         headerLabel_  = nullptr;
    QLabel*         beforeLabel_  = nullptr;
    QLabel*         afterLabel_   = nullptr;
    QLabel*         statusLabel_  = nullptr;
    QProgressBar*   progressBar_  = nullptr;
    QPlainTextEdit* errorText_    = nullptr;
    QPushButton*    closeBtn_     = nullptr;

    QPointer<fc::cache::BodyCompressionWorker> worker_;
    bool finished_ = false;

    void markFinished();
};

}  // namespace fc::ui
