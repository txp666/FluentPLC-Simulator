#include "update/update_installer.h"

#include "update/update_release.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QProcess>
#include <QtCore/QStandardPaths>
#include <QtCore/QTemporaryFile>

namespace fplc {

namespace {

const QString FailureFile = QStringLiteral("update-failed.txt");

bool fail(QString *error, const QString &message)
{
    if (error) {
        *error = message;
    }
    return false;
}

#if defined(Q_OS_MACOS)
// $1 pid  $2 installation  $3 new installation  $4 backup  $5 staging  $6 relaunch (1/0)
const char SwapScript[] = R"(pid=$1; root=$2; new=$3; backup=$4; staging=$5; relaunch=$6
log="$staging/update-failed.txt"
n=0
while kill -0 "$pid" 2>/dev/null && [ "$n" -lt 600 ]; do sleep 0.2; n=$((n + 1)); done
if kill -0 "$pid" 2>/dev/null; then echo "simulator did not exit" >>"$log"; exit 1; fi
if mv "$root" "$backup" 2>>"$log"; then
    if mv "$new" "$root" 2>>"$log"; then
        [ "$relaunch" = 1 ] && open "$root"
        rm -rf "$staging"
        exit 0
    fi
    mv "$backup" "$root" 2>>"$log"
fi
echo "could not move the new version into place" >>"$log"
[ "$relaunch" = 1 ] && open "$root"
)";
#elif defined(Q_OS_WIN)
const char SwapScript[] =
    R"(param([int]$ProcessId, [string]$Root, [string]$New, [string]$Backup, [string]$Staging, [int]$Relaunch)
$log = Join-Path $Staging 'update-failed.txt'
# Until the studio exits its files are locked.
try { Wait-Process -Id $ProcessId -Timeout 120 -ErrorAction Stop } catch {}
if (Get-Process -Id $ProcessId -ErrorAction SilentlyContinue) {
    Set-Content -LiteralPath $log -Value 'simulator did not exit' -Encoding UTF8
    exit 1
}
# A virus scanner can still hold a file for a moment after the process ends.
function Invoke-Retried([scriptblock]$Action) {
    for ($i = 0; $i -lt 30; $i++) {
        try { & $Action; return $true } catch { $script:lastError = $_.Exception.Message; Start-Sleep -Milliseconds 500 }
    }
    return $false
}
$done = $false
if (Invoke-Retried { Move-Item -LiteralPath $Root -Destination $Backup -ErrorAction Stop }) {
    if (Invoke-Retried { Move-Item -LiteralPath $New -Destination $Root -ErrorAction Stop }) {
        $done = $true
    } else {
        Invoke-Retried { Move-Item -LiteralPath $Backup -Destination $Root -ErrorAction Stop } | Out-Null
    }
}
if (-not $done) { Set-Content -LiteralPath $log -Value $script:lastError -Encoding UTF8 }
if ($Relaunch) { Start-Process -FilePath (Join-Path $Root 'FluentPLCSimulator.exe') -WorkingDirectory $Root }
if ($done) { Invoke-Retried { Remove-Item -LiteralPath $Staging -Recurse -Force -ErrorAction Stop } | Out-Null }
)";
#elif defined(Q_OS_LINUX)
// Runs as root, through pkexec. It is passed on the command line, so nothing the user could change runs with
// root's rights, and it reads from the user's cache only once: its copy of the package is root's, checked against
// the digest the studio verified, and must be this application for this system. Then it waits for the studio to
// exit and lets apt install the copy.
// $1 pid  $2 package  $3 its SHA-256
const char InstallScript[] = R"SH(set -eu
umask 077
pid=$1; package=$2; digest=$3
fail() { echo "$*" >&2; exit 1; }
case "$pid" in ''|*[!0-9]*) fail 'invalid process id';; esac
case "$digest" in *[!0-9a-f]*) fail 'invalid digest';; esac
[ "${#digest}" -eq 64 ] || fail 'invalid digest'
work=$(mktemp -d /tmp/fluentplc-update.XXXXXX)
trap 'rm -rf "$work"' EXIT
copy="$work/package.deb"
# The package could be a link to a file only root may read: the copy stays private until it has passed.
cp "$package" "$copy"
actual=$(sha256sum < "$copy")
[ "${actual%% *}" = "$digest" ] || fail 'the package does not match the release'
[ "$(dpkg-deb --field "$copy" Package)" = fluentplc-simulator ] || fail 'not a FluentPLC Simulator package'
[ "$(dpkg-deb --field "$copy" Architecture)" = "$(dpkg --print-architecture)" ] || fail 'the package is for another architecture'
# apt reads the package as its own user.
chmod 755 "$work"
chmod 644 "$copy"
n=0
while kill -0 "$pid" 2>/dev/null; do
    [ "$n" -lt 600 ] || fail 'FluentPLC Simulator did not exit'
    sleep 0.2
    n=$((n + 1))
done
DEBIAN_FRONTEND=noninteractive apt-get --yes --no-remove install "$copy"
)SH";

// Runs as the user, detached: asks for root's rights, keeps what the installation printed, leaves a report where
// takeFailureReport() looks when it failed, and starts the studio again if asked, never as root.
// $1 pkexec  $2 pid  $3 package  $4 its SHA-256  $5 staging  $6 relaunch (1/0)  $7 the root script
const char LaunchScript[] = R"SH(pkexec=$1; pid=$2; package=$3; digest=$4; staging=$5; relaunch=$6; script=$7
log="$staging/install.log"
"$pkexec" /bin/sh -c "$script" fluentplc-update "$pid" "$package" "$digest" >"$log" 2>&1
status=$?
n=0
while kill -0 "$pid" 2>/dev/null && [ "$n" -lt 600 ]; do sleep 0.2; n=$((n + 1)); done
if [ "$status" -eq 0 ]; then
    rm -rf "$staging"
else
    { echo "pkexec exit status $status"; tail -n 20 "$log"; } >"$staging/update-failed.txt"
fi
[ "$relaunch" = 1 ] && nohup /usr/bin/fluentplc-simulator >/dev/null 2>&1 &
)SH";
#endif

} // namespace

UpdateInstaller::UpdateInstaller(QObject *parent) : QObject(parent) {}

UpdateInstaller::~UpdateInstaller()
{
    if (m_unpack) {
        m_unpack->disconnect(this);
        m_unpack->kill();
        m_unpack->waitForFinished(2000);
    }
}

QString UpdateInstaller::installRoot()
{
    if (UpdateRelease::packageSuffix().isEmpty()) {
        return QString();
    }
    const QDir appDir(QCoreApplication::applicationDirPath());
#if defined(Q_OS_MACOS)
    // A build-tree .app has no package manifest, so can never replace itself.
    const QString bundle = QDir::cleanPath(appDir.absoluteFilePath(QStringLiteral("../..")));
    if (bundle.endsWith(QStringLiteral(".app")) && isInstallation(bundle)) {
        return bundle;
    }
#elif defined(Q_OS_WIN)
    if (isInstallation(appDir.absolutePath())) {
        return appDir.absolutePath();
    }
#elif defined(Q_OS_LINUX)
    // Where the .deb installs; a build tree or a copied folder is nothing apt would replace.
    if (QFileInfo(appDir.absolutePath()).canonicalFilePath() == QLatin1String("/opt/FluentPLCSimulator")
        && isInstallation(appDir.absolutePath())) {
        return appDir.absolutePath();
    }
#endif
    return QString();
}

bool UpdateInstaller::isInstallation(const QString &root, const QString &version)
{
    QDir directory(root);
#if defined(Q_OS_MACOS)
    if (!root.endsWith(QStringLiteral(".app")) || !directory.cd(QStringLiteral("Contents"))) return false;
    const QString manifest = directory.filePath(QStringLiteral("Resources/fplc-install.json"));
    const QString app = directory.filePath(QStringLiteral("MacOS/FluentPLCSimulator"));
    const QString server = directory.filePath(QStringLiteral("MacOS/FluentPLCServer"));
#else
    const QString manifest = directory.filePath(QStringLiteral("fplc-install.json"));
#if defined(Q_OS_WIN)
    const QString app = directory.filePath(QStringLiteral("FluentPLCSimulator.exe"));
    const QString server = directory.filePath(QStringLiteral("FluentPLCServer.exe"));
#else
    const QString app = directory.filePath(QStringLiteral("FluentPLCSimulator"));
    const QString server = directory.filePath(QStringLiteral("FluentPLCServer"));
#endif
#endif
    QFile file(manifest);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 8192) return false;
    const QJsonObject object = QJsonDocument::fromJson(file.readAll()).object();
    return object.value(QStringLiteral("application")).toString() == QLatin1String("io.github.txp666.FluentPLCSimulator")
        && object.value(QStringLiteral("formatVersion")).toInt() == 1
        && (!object.value(QStringLiteral("version")).toString().isEmpty())
        && (version.isEmpty() || object.value(QStringLiteral("version")).toString() == version)
        && QFileInfo(app).isFile() && QFileInfo(server).isFile();
}

QString UpdateInstaller::stagingDir()
{
    const QString root = installRoot();
#if defined(Q_OS_LINUX)
    // The installation is root's; the download waits in the user's cache until apt installs it.
    return root.isEmpty() ? QString()
                          : QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/update");
#else
    const QString id = QString::fromLatin1(QCryptographicHash::hash(root.toUtf8(), QCryptographicHash::Sha256).toHex().left(12));
    return root.isEmpty() ? QString() : QFileInfo(root).absolutePath() + QStringLiteral("/.fluentplc-update-") + id;
#endif
}

bool UpdateInstaller::canInstall(QString *reason)
{
    const QString root = installRoot();
    if (root.isEmpty()) {
        return fail(reason, UpdateRelease::packageSuffix().isEmpty()
                                ? tr("No update package is available for this platform")
                                : tr("Run a packaged FluentPLC Simulator to install updates. Build directories cannot update themselves."));
    }
#if defined(Q_OS_MACOS)
    if (root.contains(QStringLiteral("/AppTranslocation/"))) {
        return fail(reason, tr("Move the app to Applications before updating it from a macOS temporary location."));
    }
#endif
#if defined(Q_OS_LINUX)
    if (QStandardPaths::findExecutable(QStringLiteral("pkexec")).isEmpty() ||
        !QFileInfo::exists(QStringLiteral("/usr/bin/apt-get"))) {
        return fail(reason, tr("Automatic installation requires pkexec and apt-get. Install the DEB with your system package manager."));
    }
    // The download needs a folder of its own.
    const QString writable = stagingDir();
#else
    // The swap renames the installation and needs a folder beside it.
    const QString writable = QFileInfo(root).absolutePath();
#endif
    QTemporaryFile probe(QDir(writable).filePath(QStringLiteral(".fplc-update-probe-XXXXXX")));
    if (!QDir().mkpath(writable) || !probe.open()) {
        return fail(reason, tr("No permission to write to %1").arg(QDir::toNativeSeparators(writable)));
    }
    return true;
}

QString UpdateInstaller::takeFailureReport()
{
    const QString staging = stagingDir();
    if (staging.isEmpty()) {
        return QString();
    }
    QFile file(QDir(staging).filePath(FailureFile));
    if (!file.open(QIODevice::ReadOnly)) {
        return QString();
    }
    QString report = QString::fromUtf8(file.readAll()).trimmed();
    file.close();
    if (report.startsWith(QChar(0xFEFF))) {
        report.remove(0, 1);
    }
    QDir(staging).removeRecursively();
    return report.isEmpty() ? tr("Could not replace the previous version") : report;
}

void UpdateInstaller::prepare(const QString &package, const QString &version)
{
    cancel();
    if (stagingDir().isEmpty() || version.isEmpty()) {
        emit failed(tr("No packaged installation was found"));
        return;
    }
#if defined(Q_OS_LINUX)
    // apt installs the package as it was downloaded. The root installer checks its own copy against this digest,
    // since the download stays the user's to change until then.
    QProcess metadata;
    metadata.start(QStringLiteral("/usr/bin/dpkg-deb"),
                   {QStringLiteral("--field"), package, QStringLiteral("Package"), QStringLiteral("Version")});
    if (!metadata.waitForFinished(10000) || metadata.exitStatus() != QProcess::NormalExit || metadata.exitCode() != 0
        || metadata.readAllStandardOutput().trimmed() != (QByteArray("Package: fluentplc-simulator\nVersion: ") + version.toUtf8())) {
        emit failed(tr("The package does not contain a valid FluentPLC Simulator installation"));
        return;
    }
    QFile deb(package);
    QCryptographicHash digest(QCryptographicHash::Sha256);
    if (!deb.open(QIODevice::ReadOnly) || !digest.addData(&deb)) {
        emit failed(tr("Could not read %1: %2").arg(package, deb.errorString()));
        return;
    }
    m_packageDigest = digest.result().toHex();
    m_newRoot = package;
    emit prepared();
    return;
#endif
    const QString dir = QDir(stagingDir()).filePath(QStringLiteral("new"));
    QDir(dir).removeRecursively();
    QDir().mkpath(dir);
    m_unpack = new QProcess(this);
    m_unpack->setProcessChannelMode(QProcess::MergedChannels);
#if defined(Q_OS_MACOS)
    m_unpack->setProgram(QStringLiteral("/usr/bin/ditto"));
    m_unpack->setArguments({QStringLiteral("-x"), QStringLiteral("-k"), package, dir});
#else
    // bsdtar, part of Windows 10 since 1803, reads zip archives.
    m_unpack->setProgram(QDir(qEnvironmentVariable("SystemRoot", QStringLiteral("C:/Windows")))
                             .filePath(QStringLiteral("System32/tar.exe")));
    m_unpack->setArguments({QStringLiteral("-xf"), package, QStringLiteral("-C"), dir});
#endif
    QProcess *unpack = m_unpack;
    connect(unpack, &QProcess::errorOccurred, this, [this, unpack](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart && m_unpack == unpack) {
            m_unpack = nullptr;
            unpack->deleteLater();
            emit failed(tr("Could not start the archive extractor %1").arg(QDir::toNativeSeparators(unpack->program())));
        }
    });
    connect(unpack, &QProcess::finished, this, [this, unpack, package, dir, version](int code, QProcess::ExitStatus status) {
        if (m_unpack != unpack) {
            return;
        }
        m_unpack = nullptr;
        unpack->deleteLater();
        if (status != QProcess::NormalExit || code != 0) {
            emit failed(tr("Could not unpack the update: %1").arg(QString::fromLocal8Bit(unpack->readAll()).trimmed()));
            return;
        }
        const QString root = findNewRoot(dir, version);
        if (root.isEmpty()) {
            emit failed(tr("The package does not contain a valid FluentPLC Simulator installation"));
            return;
        }
        // The unpacked folder is what gets installed; the archive is no longer needed.
        QFile::remove(package);
        m_newRoot = root;
        emit prepared();
    });
    unpack->start();
}

QString UpdateInstaller::findNewRoot(const QString &dir, const QString &version) const
{
    const QFileInfoList entries = QDir(dir).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QFileInfo &entry : entries) {
        if (!entry.isSymLink() && isInstallation(entry.absoluteFilePath(), version)) {
            return entry.absoluteFilePath();
        }
    }
    return QString();
}

void UpdateInstaller::cancel()
{
    if (QProcess *unpack = m_unpack) {
        m_unpack = nullptr;
        unpack->disconnect(this);
        unpack->kill();
        unpack->waitForFinished(2000);
        unpack->deleteLater();
    }
    m_newRoot.clear();
    m_packageDigest.clear();
}

void UpdateInstaller::discard()
{
    cancel();
    const QString staging = stagingDir();
    if (!staging.isEmpty()) {
        QDir(staging).removeRecursively();
    }
}

bool UpdateInstaller::launch(bool relaunch, QString *error)
{
    const QString root = installRoot();
    if (root.isEmpty() || m_newRoot.isEmpty()) {
        return fail(error, tr("No update is ready to install"));
    }
    const QString staging = stagingDir();
    const QString backup = QDir(staging).filePath(QStringLiteral("old"));
    const QString pid = QString::number(QCoreApplication::applicationPid());
    QProcess helper;
    // The detached helper must not keep a terminal or --update caller's pipes open.
    helper.setStandardInputFile(QProcess::nullDevice());
    helper.setStandardOutputFile(QProcess::nullDevice());
    helper.setStandardErrorFile(QProcess::nullDevice());
    // Not inside the installation: a working directory there would keep it from being moved.
    helper.setWorkingDirectory(QFileInfo(root).absolutePath());
#if defined(Q_OS_MACOS)
    helper.setProgram(QStringLiteral("/bin/sh"));
    helper.setArguments({QStringLiteral("-c"), QString::fromUtf8(SwapScript), QStringLiteral("fluentplc-update"), pid,
                         root, m_newRoot, backup, staging, relaunch ? QStringLiteral("1") : QStringLiteral("0")});
#elif defined(Q_OS_WIN)
    const QString script = QDir(staging).filePath(QStringLiteral("swap.ps1"));
    QFile file(script);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(SwapScript) < 0) {
        return fail(error, tr("Could not write %1").arg(QDir::toNativeSeparators(script)));
    }
    file.close();
    helper.setProgram(QDir(qEnvironmentVariable("SystemRoot", QStringLiteral("C:/Windows")))
                          .filePath(QStringLiteral("System32/WindowsPowerShell/v1.0/powershell.exe")));
    helper.setArguments({QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"),
                         QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"), QStringLiteral("-File"),
                         QDir::toNativeSeparators(script), QStringLiteral("-ProcessId"), pid, QStringLiteral("-Root"),
                         QDir::toNativeSeparators(root), QStringLiteral("-New"), QDir::toNativeSeparators(m_newRoot),
                         QStringLiteral("-Backup"), QDir::toNativeSeparators(backup), QStringLiteral("-Staging"),
                         QDir::toNativeSeparators(staging), QStringLiteral("-Relaunch"),
                         relaunch ? QStringLiteral("1") : QStringLiteral("0")});
#elif defined(Q_OS_LINUX)
    Q_UNUSED(backup);
    const QString pkexec = QStandardPaths::findExecutable(QStringLiteral("pkexec"));
    if (pkexec.isEmpty()) {
        return fail(error, tr("Automatic installation requires pkexec and apt-get. Install the DEB with your system package manager."));
    }
    helper.setProgram(QStringLiteral("/bin/sh"));
    helper.setArguments({QStringLiteral("-c"), QString::fromUtf8(LaunchScript), QStringLiteral("fluentplc-update"),
                         pkexec, pid, m_newRoot, QString::fromLatin1(m_packageDigest), staging,
                         relaunch ? QStringLiteral("1") : QStringLiteral("0"), QString::fromUtf8(InstallScript)});
#else
    Q_UNUSED(relaunch);
    Q_UNUSED(backup);
    Q_UNUSED(pid);
    return fail(error, tr("No update package is available for this platform"));
#endif
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN) || defined(Q_OS_LINUX)
    if (!helper.startDetached()) {
        return fail(error, tr("Could not start the update helper: %1").arg(helper.errorString()));
    }
    return true;
#endif
}

} // namespace fplc
