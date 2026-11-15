#include "update/update_service.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

namespace fplc {

namespace {

constexpr int CheckTimeoutMs = 15000;
// Downloads may be slow; this is how long no data may arrive, not the whole transfer.
constexpr int DownloadStallMs = 60000;

} // namespace

UpdateService::UpdateService(QObject *parent)
    : QObject(parent), m_network(new QNetworkAccessManager(this)), m_releaseUrl(defaultReleaseUrl()),
      m_packageSuffix(UpdateRelease::packageSuffix())
{
    qRegisterMetaType<fplc::ReleaseInfo>();
}

UpdateService::~UpdateService() { abort(); }

QUrl UpdateService::defaultReleaseUrl()
{
    const QString overridden = qEnvironmentVariable("FPLC_UPDATE_URL");
    return QUrl(overridden.isEmpty()
                    ? QStringLiteral("https://api.github.com/repos/txp666/FluentPLC-Simulator/releases/latest")
                    : overridden);
}

QNetworkReply *UpdateService::get(const QUrl &url, int timeoutMs)
{
    QNetworkRequest request(url);
    request.setRawHeader("Accept", "application/vnd.github+json");
    // GitHub refuses API requests without one.
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("FluentPLC-Simulator/" FPLC_VERSION));
    request.setTransferTimeout(timeoutMs);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply *reply = m_network->get(request);
    m_reply = reply;
    return reply;
}

QString UpdateService::replyError(QNetworkReply *reply) const
{
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status == 404) {
        return tr("No published release was found");
    }
    if (status == 403 && reply->rawHeader("x-ratelimit-remaining") == "0") {
        return tr("GitHub rate limit reached. Try again later.");
    }
    if (status >= 400) {
        return tr("The server returned HTTP %1").arg(status);
    }
    if (reply->error() == QNetworkReply::OperationCanceledError || reply->error() == QNetworkReply::TimeoutError) {
        return tr("The connection timed out");
    }
    return tr("Network error: %1").arg(reply->errorString());
}

void UpdateService::check()
{
    abort();
    QNetworkReply *reply = get(m_releaseUrl, CheckTimeoutMs);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        m_reply.clear();
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            emit failed(replyError(reply));
            return;
        }
        ReleaseInfo release;
        QString error;
        if (!UpdateRelease::parse(reply->readAll(), m_packageSuffix, &release, &error)) {
            emit failed(error);
            return;
        }
        emit checked(release);
    });
}

void UpdateService::download(const ReleaseInfo &release, const QString &dir)
{
    abort();
    m_release = release;
    m_target = QDir(dir).filePath(release.package.name);
    if (!release.hasPackage() || QFileInfo(release.package.name).fileName() != release.package.name) {
        emit failed(tr("This release has no verified package for this platform"));
        return;
    }
    // The checksum first: it is small, and without it the package cannot be trusted.
    QNetworkReply *reply = get(release.checksum.url, CheckTimeoutMs);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        m_reply.clear();
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            emit failed(replyError(reply));
            return;
        }
        const QByteArray digest = UpdateRelease::checksumFor(reply->readAll(), m_release.package.name);
        if (digest.isEmpty()) {
            emit failed(tr("The checksum file has no SHA-256 for %1").arg(m_release.package.name));
            return;
        }
        fetchPackage(digest);
    });
}

void UpdateService::fetchPackage(const QByteArray &expectedDigest)
{
    QDir().mkpath(QFileInfo(m_target).absolutePath());
    m_partial.setFileName(m_target + QStringLiteral(".part"));
    if (!m_partial.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        emit failed(tr("Could not write %1").arg(QDir::toNativeSeparators(m_partial.fileName())));
        return;
    }
    m_hash.reset();
    QNetworkReply *reply = get(m_release.package.url, DownloadStallMs);
    connect(reply, &QNetworkReply::readyRead, this, [this, reply]() {
        const QByteArray chunk = reply->readAll();
        m_hash.addData(chunk);
        if (m_partial.write(chunk) != chunk.size()) {
            const QString error = tr("Could not write %1").arg(QDir::toNativeSeparators(m_partial.fileName()));
            abort();
            emit failed(error);
        }
    });
    connect(reply, &QNetworkReply::downloadProgress, this, [this](qint64 received, qint64 total) {
        emit progress(received, total > 0 ? total : m_release.package.size);
    });
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, expectedDigest]() { finishPackage(reply, expectedDigest); });
}

void UpdateService::finishPackage(QNetworkReply *reply, const QByteArray &expectedDigest)
{
    m_reply.clear();
    reply->deleteLater();
    if (reply->error() != QNetworkReply::NoError) {
        discardPartial();
        emit failed(replyError(reply));
        return;
    }
    const QByteArray rest = reply->readAll();
    m_hash.addData(rest);
    const bool written = m_partial.write(rest) == rest.size() && m_partial.flush();
    const qint64 size = m_partial.size();
    m_partial.close();
    if (!written) {
        discardPartial();
        emit failed(tr("Could not write %1").arg(QDir::toNativeSeparators(m_target)));
        return;
    }
    if (m_release.package.size > 0 && size != m_release.package.size) {
        discardPartial();
        emit failed(tr("Incomplete download (%1 / %2 bytes)").arg(size).arg(m_release.package.size));
        return;
    }
    if (m_hash.result().toHex() != expectedDigest) {
        discardPartial();
        emit failed(tr("Package verification failed: SHA-256 does not match the published checksum"));
        return;
    }
    QFile::remove(m_target);
    if (!m_partial.rename(m_target)) {
        discardPartial();
        emit failed(tr("Could not write %1").arg(QDir::toNativeSeparators(m_target)));
        return;
    }
    emit downloaded(m_target);
}

void UpdateService::discardPartial()
{
    m_partial.close();
    m_partial.remove();
}

void UpdateService::abort()
{
    if (QNetworkReply *reply = m_reply.data()) {
        m_reply.clear();
        reply->disconnect(this);
        reply->abort();
        reply->deleteLater();
    }
    if (m_partial.isOpen()) {
        discardPartial();
    }
}

} // namespace fplc
