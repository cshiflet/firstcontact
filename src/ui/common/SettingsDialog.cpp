#include "SettingsDialog.h"

#include "Theme.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace fc::ui {

SettingsDialog::SettingsDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("FirstContact Settings"));
    resize(420, 220);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(20, 20, 20, 20);
    root->setSpacing(14);

    auto* title = new QLabel(tr("<h3 style='margin:0'>Appearance</h3>"), this);
    title->setTextFormat(Qt::RichText);
    root->addWidget(title);

    auto* form = new QFormLayout;
    auto* themeBox = new QComboBox(this);
    themeBox->addItem(tr("Match system"), int(Theme::Mode::Auto));
    themeBox->addItem(tr("Light"),        int(Theme::Mode::Light));
    themeBox->addItem(tr("Dark"),         int(Theme::Mode::Dark));
    const int currentIdx = themeBox->findData(int(Theme::currentMode()));
    if (currentIdx >= 0) themeBox->setCurrentIndex(currentIdx);
    connect(themeBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this,      &SettingsDialog::onThemeChanged);
    form->addRow(tr("Theme:"), themeBox);

    auto* hint = new QLabel(tr(
        "Changes apply immediately. The choice is remembered next time "
        "FirstContact starts."), this);
    hint->setObjectName(QStringLiteral("FormHint"));
    hint->setWordWrap(true);
    form->addRow(QString(), hint);

    root->addLayout(form);
    root->addStretch(1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    buttons->button(QDialogButtonBox::Close)->setObjectName(QStringLiteral("primary"));
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    root->addWidget(buttons);
}

void SettingsDialog::onThemeChanged(int idx) {
    auto* box = qobject_cast<QComboBox*>(sender());
    if (!box) return;
    const auto mode = static_cast<Theme::Mode>(box->itemData(idx).toInt());
    Theme::apply(mode);
}

}  // namespace fc::ui
