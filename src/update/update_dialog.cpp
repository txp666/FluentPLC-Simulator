#include "update/update_dialog.h"

#include "core/i18n.h"
#include "update/update_installer.h"
#include "update/update_service.h"

#include <FluentQtWidgets/Widgets/Button.h>
#include <FluentQtWidgets/Widgets/Label.h>
#include <FluentQtWidgets/Widgets/LineEdit.h>
#include <FluentQtWidgets/Widgets/ProgressBar.h>

#include <QtGui/QDesktopServices>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>

namespace fplc {

UpdateDialog::UpdateDialog(const ReleaseInfo &release, UpdateService *service, UpdateInstaller *installer,
                           bool offerSkip, QWidget *parent)
    : FluentQt::MessageBoxBase(parent), m_release(release), m_service(service), m_installer(installer)
{
    if (!release.hasPackage()) {
        m_cannotInstall = tr("This release has no verified package for this platform");
    } else {
        UpdateInstaller::canInstall(&m_cannotInstall);
    }

    auto *title = new FluentQt::SubtitleLabel(tr("Version %1 is available").arg(release.tag), this);
    auto *versions = new FluentQt::BodyLabel(tr("Current version: v%1").arg(QStringLiteral(FPLC_VERSION)), this);
    auto *notes = new FluentQt::TextBrowser(this);
    const QString text = UpdateRelease::notesFor(release.notes, I18n::language() != QStringLiteral("zh_CN"));
    notes->setMarkdown(text.isEmpty() ? tr("(No release notes provided)") : text);
    notes->setOpenExternalLinks(true);
    notes->setMinimumWidth(460);
    notes->setFixedHeight(220);
    auto *page = new FluentQt::HyperlinkButton(release.page.toString(), tr("View release in browser"), this);
    m_progress = new FluentQt::ProgressBar(this);
    m_progress->setRange(0, 1000);
    m_progress->hide();
    m_status = new FluentQt::CaptionLabel(this);
    m_status->setWordWrap(true);

    viewLayout()->addWidget(title);
    viewLayout()->addWidget(versions);
    viewLayout()->addWidget(notes);
    viewLayout()->addWidget(page, 0, Qt::AlignLeft);
    viewLayout()->addWidget(m_progress);
    viewLayout()->addWidget(m_status);

    if (offerSkip) {
        m_skip = new FluentQt::PushButton(tr("Skip this version"), buttonGroup());
        buttonLayout()->insertWidget(1, m_skip);
        connect(m_skip, &QPushButton::clicked, this, [this]() {
            emit skipRequested();
            reject();
        });
    }
    // The yes button drives the update instead of closing the dialog straight away.
    disconnect(yesButton(), &QPushButton::clicked, this, nullptr);
    connect(yesButton(), &QPushButton::clicked, this, &UpdateDialog::proceed);

    connect(m_service, &UpdateService::progress, this, [this](qint64 received, qint64 total) {
        if (total > 0) {
            m_progress->setValue(int(received * 1000 / total));
            m_status->setText(tr("Downloading %1 / %2 MB")
                                  .arg(double(received) / (1 << 20), 0, 'f', 1)
                                  .arg(double(total) / (1 << 20), 0, 'f', 1));
        }
    });
    connect(m_service, &UpdateService::downloaded, this, [this](const QString &path) {
        setState(State::Working, tr("Checksum verified. Unpacking…"));
        m_installer->prepare(path, m_release.version.toString());
    });
    connect(m_service, &UpdateService::failed, this,
            [this](const QString &message) { setState(State::Failed, tr("Download failed: %1").arg(message)); });
    connect(m_installer, &UpdateInstaller::prepared, this, [this]() {
        m_progress->setValue(1000);
        setState(State::Ready, tr("The update is ready. Restart the simulator to install it, or choose Later to install it when you exit."));
    });
    connect(m_installer, &UpdateInstaller::failed, this,
            [this](const QString &message) { setState(State::Failed, message); });

    setState(State::Offer, m_cannotInstall.isEmpty() ? QString() : tr("Automatic update unavailable: %1").arg(m_cannotInstall));
}

void UpdateDialog::proceed()
{
    switch (m_state) {
    case State::Offer:
    case State::Failed:
        if (!m_cannotInstall.isEmpty()) {
            QDesktopServices::openUrl(m_release.page);
            reject();
            return;
        }
        m_progress->setValue(0);
        setState(State::Working, tr("Connecting…"));
        m_service->download(m_release, UpdateInstaller::stagingDir());
        break;
    case State::Ready:
        accept();
        break;
    case State::Working:
        break;
    }
}

void UpdateDialog::setState(State state, const QString &status)
{
    m_state = state;
    m_status->setText(status);
    m_status->setVisible(!status.isEmpty());
    m_progress->setVisible(state == State::Working || state == State::Ready);
    m_progress->setError(false);
    if (m_skip) {
        m_skip->setVisible(state == State::Offer);
    }
    yesButton()->setEnabled(state != State::Working);
    switch (state) {
    case State::Offer:
        yesButton()->setText(m_cannotInstall.isEmpty() ? tr("Update now") : tr("Open downloads"));
        cancelButton()->setText(tr("Not now"));
        break;
    case State::Working:
        yesButton()->setText(tr("Updating…"));
        cancelButton()->setText(tr("Cancel"));
        break;
    case State::Ready:
        yesButton()->setText(tr("Restart and update"));
        cancelButton()->setText(tr("Later"));
        break;
    case State::Failed:
        yesButton()->setText(tr("Retry"));
        cancelButton()->setText(tr("Close"));
        break;
    }
}

} // namespace fplc
