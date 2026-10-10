#include "ui/RecoveryDialog.h"

#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "ui/Icons.h"

namespace tessel {

namespace {

QString savedAtText(const QDateTime& savedAt)
{
    if (!savedAt.isValid())
        return QCoreApplication::translate("Dialogs", "unknown");
    const QDateTime now = QDateTime::currentDateTime();
    const qint64 minutes = savedAt.secsTo(now) / 60;
    if (minutes < 1)
        return QCoreApplication::translate("Dialogs", "just now");
    if (minutes < 60) {
        return QCoreApplication::translate("Dialogs", "%n minute(s) ago", nullptr,
                                           int(minutes));
    }
    if (savedAt.date() == now.date())
        return savedAt.toString(QStringLiteral("HH:mm"));
    return savedAt.toString(QStringLiteral("d MMM HH:mm"));
}

} // namespace

QString RecoveryDialog::describe(const recovery::Entry& entry)
{
    return QStringLiteral("%1, %2, %3")
        .arg(entry.displayName(), QLocale().formattedDataSize(entry.size),
             savedAtText(entry.savedAt));
}

RecoveryDialog::RecoveryDialog(const std::vector<recovery::Entry>& entries,
                               QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Recover Unsaved Images"));
    setWindowIcon(icons::get(QStringLiteral("open"), 22));
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(10);

    auto* intro = new QLabel(
        tr("Tessel did not close cleanly last time. These images were saved "
           "automatically and have not been opened yet."),
        this);
    intro->setWordWrap(true);
    root->addWidget(intro);

    list_ = new QTreeWidget(this);
    list_->setObjectName(QStringLiteral("recoveryList"));
    list_->setRootIsDecorated(false);
    list_->setUniformRowHeights(true);
    list_->setSelectionMode(QAbstractItemView::NoSelection);
    list_->setHeaderLabels({tr("Image"), tr("Size"),
                            tr("Saved")});
    for (const recovery::Entry& entry : entries) {
        auto* item = new QTreeWidgetItem(list_);
        item->setText(0, entry.displayName());
        item->setText(1, QLocale().formattedDataSize(entry.size));
        item->setText(2, savedAtText(entry.savedAt));
        // The original path is the one thing that tells two images with the
        // same name apart, so it goes in the tooltip rather than nowhere.
        item->setToolTip(0, entry.originalPath.isEmpty()
                                ? tr("Never saved to a file")
                                : QFileInfo(entry.originalPath).absoluteFilePath());
    }
    list_->header()->setStretchLastSection(false);
    list_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    list_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    list_->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    list_->setMinimumSize(460, 180);
    root->addWidget(list_, 1);

    auto* buttons = new QDialogButtonBox(this);
    QPushButton* recover = buttons->addButton(tr("Recover"),
                                              QDialogButtonBox::AcceptRole);
    QPushButton* discard = buttons->addButton(tr("Discard"),
                                              QDialogButtonBox::DestructiveRole);
    QPushButton* later = buttons->addButton(tr("Decide Later"),
                                            QDialogButtonBox::RejectRole);
    recover->setDefault(true);
    recover->setToolTip(tr("Open every image as an unsaved document"));
    discard->setToolTip(tr("Delete the recovery files"));
    later->setToolTip(tr("Leave the files and ask again next time"));
    connect(recover, &QPushButton::clicked, this, [this] { finish(Choice::Recover); });
    connect(discard, &QPushButton::clicked, this, [this] { finish(Choice::Discard); });
    connect(later, &QPushButton::clicked, this, [this] { finish(Choice::Later); });
    root->addWidget(buttons);
}

void RecoveryDialog::finish(Choice choice)
{
    choice_ = choice;
    if (choice == Choice::Later)
        reject();
    else
        accept();
}

void RecoveryDialog::chooseRecover() { finish(Choice::Recover); }
void RecoveryDialog::chooseDiscard() { finish(Choice::Discard); }
void RecoveryDialog::chooseLater() { finish(Choice::Later); }

} // namespace tessel
