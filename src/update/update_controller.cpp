#include "update/update_controller.h"

#include <FluentQtWidgets/Widgets/InfoBar.h>
#include "update/update_dialog.h"
#include "update/update_installer.h"
#include "update/update_service.h"

#include <QtCore/QEventLoop>
#include <QtCore/QSettings>
#include <QtCore/QTextStream>
#include <QtCore/QTimer>
#include <QtWidgets/QWidget>

namespace fplc {

static void showInfoBar(QWidget *parent, const QString &title, const QString &content,
                        FluentQt::InfoBarSeverity type, int durationMs)
{
    FluentQt::InfoBar::newInfoBar(type, title, content, Qt::Vertical, true, durationMs,
                                 FluentQt::InfoBarPosition::TopRight, parent);
}

namespace {

const QString CheckAtStartupKey = QStringLiteral("update/checkAtStartup");
const QString SkippedVersionKey = QStringLiteral("update/skippedVersion");

} // namespace

UpdateController::UpdateController(QWidget *window)
    : QObject(window), m_window(window), m_service(new UpdateService(this)), m_installer(new UpdateInstaller(this))
{
    connect(m_service, &UpdateService::checked, this, [this](const ReleaseInfo &release) {
        if (!m_checking) {
            return;
        }
        m_checking = false;
        emit checkingChanged(false);
        offer(release, m_manual);
    });
    // Download failures go to the dialog; only a failed check is reported here.
    connect(m_service, &UpdateService::failed, this, [this](const QString &message) {
        if (!m_checking) {
            return;
        }
        m_checking = false;
        emit checkingChanged(false);
        if (m_manual) {
            showInfoBar(m_window, tr("Update check failed"), message, FluentQt::InfoBarSeverity::Error, 5000);
        }
    });
}

bool UpdateController::checkAtStartup() { return QSettings().value(CheckAtStartupKey, true).toBool(); }

void UpdateController::setCheckAtStartup(bool enabled) { QSettings().setValue(CheckAtStartupKey, enabled); }

void UpdateController::startupCheck()
{
    const QString report = UpdateInstaller::takeFailureReport();
    if (!report.isEmpty()) {
        showInfoBar(m_window, tr("The previous update failed. The original version was kept."), report, FluentQt::InfoBarSeverity::Warning, 8000);
    }
    if (checkAtStartup()) {
        check(false);
    }
}

void UpdateController::checkNow() { check(true); }

void UpdateController::check(bool manual)
{
    if (m_checking) {
        return;
    }
    if (!m_preparedTag.isEmpty()) {
        if (manual) {
            showInfoBar(m_window, tr("Version %1 is downloaded").arg(m_preparedTag), tr("The update will be installed when the simulator exits."),
                        FluentQt::InfoBarSeverity::Info, 4000);
        }
        return;
    }
    m_checking = true;
    m_manual = manual;
    emit checkingChanged(true);
    m_service->check();
}

void UpdateController::offer(const ReleaseInfo &release, bool manual)
{
    if (!UpdateRelease::isNewer(release, QStringLiteral(FPLC_VERSION))) {
        if (manual) {
            showInfoBar(m_window, tr("You are up to date"), tr("Current version: v%1").arg(QStringLiteral(FPLC_VERSION)),
                        FluentQt::InfoBarSeverity::Success, 3000);
        }
        return;
    }
    if (!manual && QSettings().value(SkippedVersionKey).toString() == release.tag) {
        return;
    }
    UpdateDialog dialog(release, m_service, m_installer, !manual, m_window);
    connect(&dialog, &UpdateDialog::skipRequested, this,
            [&release]() { QSettings().setValue(SkippedVersionKey, release.tag); });
    const bool restart = dialog.exec() == QDialog::Accepted;
    if (!m_installer->isPrepared()) {
        // Closed before the update was ready: drop whatever was under way.
        m_service->abort();
        m_installer->discard();
        return;
    }
    m_preparedTag = release.tag;
    if (restart) {
        emit restartRequested();
    } else {
        showInfoBar(m_window, tr("Version %1 is ready").arg(release.tag), tr("The update will be installed when the simulator exits."),
                    FluentQt::InfoBarSeverity::Info, 4000);
    }
}

bool UpdateController::finishOnExit(bool relaunch, QString *error)
{
    if (!m_installer->isPrepared()) {
        return true;
    }
    return m_installer->launch(relaunch, error);
}

int UpdateController::runHeadless()
{
    QTextStream out(stdout);
    QTextStream err(stderr);
    UpdateService service;
    UpdateInstaller installer;
    QString releaseVersion;
    QEventLoop loop;
    const auto failWith = [&](const QString &message) {
        err << message << Qt::endl;
        installer.discard();
        loop.exit(1);
    };
    QObject::connect(&service, &UpdateService::failed, &loop, failWith);
    QObject::connect(&installer, &UpdateInstaller::failed, &loop, failWith);
    QObject::connect(&service, &UpdateService::checked, &loop, [&](const ReleaseInfo &release) {
        if (!UpdateRelease::isNewer(release, QStringLiteral(FPLC_VERSION))) {
            out << "FluentPLC Simulator " FPLC_VERSION " is up to date (latest " << release.tag << ")" << Qt::endl;
            loop.exit(0);
            return;
        }
        if (!release.hasPackage()) {
            failWith(tr("%1 has no verified package for this platform").arg(release.tag));
            return;
        }
        QString reason;
        if (!UpdateInstaller::canInstall(&reason)) {
            failWith(reason);
            return;
        }
        out << "downloading " << release.package.name << Qt::endl;
        releaseVersion = release.version.toString();
        service.download(release, UpdateInstaller::stagingDir());
    });
    QObject::connect(&service, &UpdateService::downloaded, &loop, [&](const QString &path) {
        out << "checksum verified, unpacking" << Qt::endl;
        installer.prepare(path, releaseVersion);
    });
    QObject::connect(&installer, &UpdateInstaller::prepared, &loop, [&]() {
        QString error;
        if (!installer.launch(false, &error)) {
            failWith(error);
            return;
        }
        out << "installing: the new version replaces this one once this process has exited" << Qt::endl;
        loop.exit(0);
    });
    QTimer::singleShot(0, &service, &UpdateService::check);
    return loop.exec();
}

} // namespace fplc
