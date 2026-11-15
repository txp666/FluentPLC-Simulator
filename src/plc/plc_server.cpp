#include "plc/plc_server.h"

#include "core/project_plan.h"
#include "snap7.h"

#include <QtCore/QPointer>
#include <QtCore/QStringList>
#include <QtCore/QThread>
#include <QtCore/QTimer>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <new>
#include <vector>

namespace fplc {
namespace {
QString serverError(int code)
{
    char text[512] = {};
    Srv_ErrorText(code, text, int(sizeof text));
    return QString::fromLocal8Bit(text) + QStringLiteral(" (0x%1)").arg(quint32(code), 8, 16, QLatin1Char('0'));
}

bool fail(QString *error, const QString &message)
{
    if (error) *error = message;
    return false;
}

struct AreaBuffer {
    int nativeArea = srvAreaDB;
    word dbNumber = 0;
    // Never resize, copy or detach a buffer while registered with native Snap7.
    std::vector<char> bytes;
};

using AreaBuffers = std::map<QString, std::unique_ptr<AreaBuffer>>;

int nativeArea(MemoryArea area)
{
    switch (area) {
    case MemoryArea::DataBlock: return srvAreaDB;
    case MemoryArea::Marker: return srvAreaMK;
    case MemoryArea::Input: return srvAreaPE;
    case MemoryArea::Output: return srvAreaPA;
    }
    return srvAreaDB;
}

// The same locks protect native client reads/writes and local editor/generator
// updates. Multiple locks are always acquired in area-key order. No signal or
// server stop is performed while holding one of these locks.
class AreaLocks {
public:
    AreaLocks(S7Object server, const AreaBuffers &areas) : m_server(server)
    {
        if (!server) return;
        for (const auto &entry : areas) {
            AreaBuffer *area = entry.second.get();
            if (Srv_LockArea(server, area->nativeArea, area->dbNumber) != 0) {
                m_ok = false;
                break;
            }
            m_locked.push_back(area);
        }
    }
    ~AreaLocks()
    {
        for (auto it = m_locked.rbegin(); it != m_locked.rend(); ++it)
            Srv_UnlockArea(m_server, (*it)->nativeArea, (*it)->dbNumber);
    }
    bool ok() const { return m_ok; }
    AreaLocks(const AreaLocks &) = delete;
    AreaLocks &operator=(const AreaLocks &) = delete;

private:
    S7Object m_server;
    std::vector<AreaBuffer *> m_locked;
    bool m_ok = true;
};

QByteArray readBytes(const AreaBuffer &area, const PreparedTag &tag)
{
    const char *data = area.bytes.data() + tag.address.byteOffset;
    if (tag.type.isBool()) {
        const bool bit = (quint8(static_cast<unsigned char>(*data)) >> tag.address.bit) & 1U;
        return QByteArray(1, bit ? 1 : 0);
    }
    return QByteArray(data, tag.type.byteCount);
}

void writeBytes(AreaBuffer &area, const PreparedTag &tag, const QByteArray &encoded)
{
    char *destination = area.bytes.data() + tag.address.byteOffset;
    if (tag.type.isBool()) {
        const quint8 mask = quint8(1U << tag.address.bit);
        const quint8 before = quint8(static_cast<unsigned char>(*destination));
        *destination = char(encoded.at(0) ? (before | mask) : (before & quint8(~mask)));
    } else {
        std::memcpy(destination, encoded.constData(), size_t(tag.type.byteCount));
    }
}
} // namespace

struct PlcServer::Impl {
    Project project;
    ProjectPlan plan;
    AreaBuffers areas;
    QList<AreaBuffer *> tagAreas;
    QList<QByteArray> observed;
    S7Object native = 0;
    QTimer *timer = nullptr;
    bool running = false;
    int clients = 0;

    ~Impl() { shutdown(); }

    void shutdown()
    {
        // Stop joins Snap7's listener and workers before Destroy disposes the
        // registered-area metadata. The user buffers outlive both operations.
        if (native) {
            Srv_Stop(native);
            Srv_Destroy(&native);
        }
        running = false;
        clients = 0;
    }

    QList<QByteArray> snapshotUnlocked() const
    {
        QList<QByteArray> values;
        values.reserve(plan.tags.size());
        for (int i = 0; i < plan.tags.size(); ++i) values.append(readBytes(*tagAreas.at(i), plan.tags.at(i)));
        return values;
    }

    bool refresh()
    {
        AreaLocks locks(native, areas);
        if (!locks.ok()) return false;
        QList<QByteArray> current = snapshotUnlocked();
        if (current == observed) return false;
        observed = std::move(current);
        return true;
    }

    QStringList drainEvents()
    {
        QStringList lines;
        if (!native) return lines;
        int reads = 0;
        int writes = 0;
        // Native's bounded queue is polled on the owning Qt thread. No native
        // callback ever captures a QObject or survives its destruction.
        for (int i = 0; i < 1500; ++i) {
            TSrvEvent event{};
            int ready = 0;
            if (Srv_PickEvent(native, &event, &ready) != 0 || !ready) break;
            if (event.EvtCode == evcDataRead && event.EvtRetCode == 0) { ++reads; continue; }
            if (event.EvtCode == evcDataWrite && event.EvtRetCode == 0) { ++writes; continue; }
            char text[512] = {};
            if (Srv_EventText(&event, text, int(sizeof text)) == 0)
                lines.append(QString::fromLocal8Bit(text));
        }
        if (reads || writes) lines.append(QStringLiteral("S7 数据交换：读取 %1 次，写入 %2 次").arg(reads).arg(writes));
        return lines;
    }
};

PlcServer::PlcServer(QObject *parent) : QObject(parent), d(std::make_unique<Impl>())
{
    d->timer = new QTimer(this);
    d->timer->setInterval(50);
    connect(d->timer, &QTimer::timeout, this, [this]() {
        if (!d->native || !d->running) return;
        const QStringList lines = d->drainEvents();
        int status = 0;
        int cpuStatus = 0;
        int count = 0;
        Srv_GetStatus(d->native, &status, &cpuStatus, &count);
        const bool clientsChangedNow = count != d->clients;
        d->clients = count;
        const bool changed = d->refresh();
        // All native calls and memory locks finish before delivery to UI slots.
        QPointer<PlcServer> alive(this);
        if (clientsChangedNow) { emit clientsChanged(count); if (!alive) return; }
        if (changed) { emit valuesChanged(); if (!alive) return; }
        for (const QString &line : lines) { emit logMessage(line); if (!alive) return; }
    });
    QString ignored;
    setProject(Project::demo(), &ignored);
}

PlcServer::~PlcServer()
{
    d->timer->stop();
    // Do not emit application signals from a partially destroyed QObject.
    d->shutdown();
}

bool PlcServer::setProject(const Project &project, QString *error)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (d->running) return fail(error, QStringLiteral("请先停止模拟 PLC 再修改工程"));
    ProjectPlan plan;
    if (!prepareProject(project, &plan, error)) return false;
    AreaBuffers areas;
    QList<AreaBuffer *> tagAreas;
    try {
        const auto addArea = [&](const QString &key, int code, int dbNumber) {
            if (areas.find(key) != areas.end()) return;
            auto area = std::make_unique<AreaBuffer>();
            area->nativeArea = code;
            area->dbNumber = word(dbNumber);
            area->bytes.resize(size_t(project.memorySize), 0);
            areas.emplace(key, std::move(area));
        };
        addArea(QStringLiteral("M"), srvAreaMK, 0);
        addArea(QStringLiteral("I"), srvAreaPE, 0);
        addArea(QStringLiteral("Q"), srvAreaPA, 0);
        tagAreas.reserve(plan.tags.size());
        for (const PreparedTag &tag : plan.tags) {
            addArea(tag.address.areaKey(), nativeArea(tag.address.area), tag.address.dbNumber);
            AreaBuffer *area = areas.at(tag.address.areaKey()).get();
            writeBytes(*area, tag, tag.initialBytes);
            tagAreas.append(area);
        }
    } catch (const std::bad_alloc &) {
        return fail(error, QStringLiteral("无法分配工程内存"));
    }
    // Prepare everything first: a bad project never replaces the current one.
    d->shutdown();
    d->project = project;
    for (int i = 0; i < d->project.tags.size(); ++i) {
        Tag &tag = d->project.tags[i];
        tag.address = plan.tags.at(i).address.canonical;
        tag.dataType = plan.tags.at(i).type.name;
        tag.mode = tag.mode.trimmed().toLower();
        if (plan.tags.at(i).sourceIndex >= 0) tag.source = plan.tags.at(plan.tags.at(i).sourceIndex).address.canonical;
    }
    d->plan = std::move(plan);
    d->areas = std::move(areas);
    d->tagAreas = std::move(tagAreas);
    d->observed = d->snapshotUnlocked();
    if (error) error->clear();
    emit valuesChanged();
    return true;
}

const Project &PlcServer::project() const { return d->project; }

bool PlcServer::start(QString *error)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (d->running) { if (error) error->clear(); return true; }
    if (!d->project.validate(error)) return false;
    d->shutdown();
    try { d->native = Srv_Create(); }
    catch (const std::bad_alloc &) { return fail(error, QStringLiteral("无法创建 Snap7 服务器")); }
    if (!d->native) return fail(error, QStringLiteral("无法创建 Snap7 服务器"));
    const auto failed = [&](int code, const QString &action) {
        const QString message = action + QStringLiteral("：") + serverError(code);
        d->shutdown();
        emit logMessage(message);
        return fail(error, message);
    };
    word port = d->project.port;
    int code = Srv_SetParam(d->native, p_u16_LocalPort, &port);
    if (code) return failed(code, QStringLiteral("设置监听端口失败"));
    int maxClients = 16;
    code = Srv_SetParam(d->native, p_i32_MaxClients, &maxClients);
    if (code) return failed(code, QStringLiteral("设置客户端上限失败"));
    int workInterval = 25;
    code = Srv_SetParam(d->native, p_i32_WorkInterval, &workInterval);
    if (code) return failed(code, QStringLiteral("设置工作间隔失败"));
    const longword mask = evcServerStarted | evcServerStopped | evcListenerCannotStart | evcClientAdded
                          | evcClientRejected | evcClientNoRoom | evcClientException | evcClientDisconnected
                          | evcClientTerminated | evcClientsDropped | evcDataRead | evcDataWrite | evcNegotiatePDU;
    Srv_SetMask(d->native, mkLog, mask);
    Srv_SetMask(d->native, mkEvent, evcNone);
    for (const auto &entry : d->areas) {
        AreaBuffer &area = *entry.second;
        code = Srv_RegisterArea(d->native, area.nativeArea, area.dbNumber, area.bytes.data(), int(area.bytes.size()));
        if (code) return failed(code, QStringLiteral("注册 %1 内存失败").arg(entry.first));
    }
    code = Srv_StartTo(d->native, d->project.bindAddress.toLatin1().constData());
    if (code) return failed(code, QStringLiteral("无法监听 %1:%2").arg(d->project.bindAddress).arg(d->project.port));
    d->running = true;
    d->clients = 0;
    d->timer->start();
    if (error) error->clear();
    emit stateChanged(true);
    return true;
}

void PlcServer::stop()
{
    Q_ASSERT(QThread::currentThread() == thread());
    d->timer->stop();
    const bool wasRunning = d->running;
    const int clients = d->clients;
    d->shutdown();
    QPointer<PlcServer> alive(this);
    if (clients) { emit clientsChanged(0); if (!alive) return; }
    if (wasRunning) { emit stateChanged(false); if (!alive) return; emit logMessage(QStringLiteral("模拟 PLC 已停止，内存值已保留")); }
}

bool PlcServer::isRunning() const { return d->running; }
int PlcServer::clientCount() const { return d->clients; }

QVariant PlcServer::value(int tagIndex) const
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (tagIndex < 0 || tagIndex >= d->plan.tags.size()) return {};
    AreaLocks locks(d->native, d->areas);
    if (!locks.ok()) return {};
    const PreparedTag &tag = d->plan.tags.at(tagIndex);
    const QByteArray bytes = readBytes(*d->tagAreas.at(tagIndex), tag);
    return decodeValue(bytes.constData(), int(bytes.size()), tag.type);
}

bool PlcServer::setValue(int tagIndex, const QVariant &value, QString *error)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (tagIndex < 0 || tagIndex >= d->plan.tags.size()) return fail(error, QStringLiteral("标签索引无效"));
    const PreparedTag &tag = d->plan.tags.at(tagIndex);
    QByteArray encoded;
    if (!encodeValue(value, tag.type, &encoded, error)) return false;
    bool changed = false;
    {
        AreaLocks locks(d->native, d->areas);
        if (!locks.ok()) return fail(error, QStringLiteral("无法锁定 PLC 内存"));
        AreaBuffer &area = *d->tagAreas.at(tagIndex);
        writeBytes(area, tag, encoded);
        QList<QByteArray> current = d->snapshotUnlocked();
        changed = current != d->observed;
        d->observed = std::move(current);
    }
    if (error) error->clear();
    if (changed) emit valuesChanged();
    return true;
}

void PlcServer::tick(double elapsedSeconds)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (!d->running || !std::isfinite(elapsedSeconds) || elapsedSeconds < 0) return;
    bool changed = false;
    {
        AreaLocks locks(d->native, d->areas);
        if (!locks.ok()) return;
        for (int index : d->plan.evaluationOrder) {
            const Tag &rule = d->project.tags.at(index);
            const PreparedTag &tag = d->plan.tags.at(index);
            AreaBuffer &area = *d->tagAreas.at(index);
            if (rule.mode == QLatin1String("manual")) continue; // External writes are authoritative.
            QByteArray encoded;
            if (rule.mode == QLatin1String("follow")) {
                encoded = readBytes(*d->tagAreas.at(tag.sourceIndex), d->plan.tags.at(tag.sourceIndex));
            } else {
                // fmod before dividing avoids overflow for a very small period.
                const double phase = std::fmod(elapsedSeconds, rule.periodSeconds) / rule.periodSeconds;
                QVariant generated;
                if (rule.mode == QLatin1String("toggle")) {
                    const bool initial = tag.initialBytes.at(0) != 0;
                    generated = initial != (phase >= 0.5);
                } else {
                    constexpr double tau = 6.283185307179586476925286766559;
                    const double proportion = rule.mode == QLatin1String("sine") ? (std::sin(tau * phase) + 1) * 0.5 : phase;
                    double number = rule.minimum + (rule.maximum - rule.minimum) * proportion;
                    number = std::clamp(number, rule.minimum, rule.maximum);
                    if (tag.type.isInteger()) number = std::round(number);
                    generated = number;
                }
                if (!encodeValue(generated, tag.type, &encoded)) continue;
            }
            writeBytes(area, tag, encoded);
        }
        QList<QByteArray> current = d->snapshotUnlocked();
        changed = current != d->observed;
        d->observed = std::move(current);
    }
    if (changed) emit valuesChanged();
}
} // namespace fplc
