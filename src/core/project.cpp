#include "core/project.h"
#include "core/i18n.h"
#include "core/project_plan.h"

#include <QtCore/QFile>
#include <QtCore/QHash>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSaveFile>
#include <QtCore/QSet>
#include <QtCore/QStringList>

#include <algorithm>
#include <cmath>

namespace fplc {
namespace {
constexpr int kMaximumTags = 4096;
constexpr qint64 kMaximumMemory = 32LL * 1024 * 1024;

bool fail(QString *error, const QString &message)
{
    if (error) *error = message;
    return false;
}

bool validBindAddress(const QString &address)
{
    const QStringList octets = address.split(QLatin1Char('.'));
    if (octets.size() != 4 || address.size() > 15) return false;
    for (const QString &octet : octets) {
        if (octet.isEmpty() || octet.size() > 3 || (octet.size() > 1 && octet.startsWith(QLatin1Char('0')))) return false;
        for (QChar c : octet) if (c < QLatin1Char('0') || c > QLatin1Char('9')) return false;
        if (octet.toInt() > 255) return false;
    }
    return true;
}

bool finiteNumber(const QJsonValue &value)
{
    return value.isDouble() && std::isfinite(value.toDouble());
}

bool integerIn(const QJsonValue &value, int minimum, int maximum)
{
    return finiteNumber(value) && std::floor(value.toDouble()) == value.toDouble()
           && value.toDouble() >= minimum && value.toDouble() <= maximum;
}

QString modeOf(const Tag &tag) { return tag.mode.trimmed().toLower(); }

Project normalized(const Project &project, const ProjectPlan &plan)
{
    Project out = project;
    for (int i = 0; i < out.tags.size(); ++i) {
        Tag &tag = out.tags[i];
        const PreparedTag &parsed = plan.tags.at(i);
        tag.address = parsed.address.canonical;
        tag.dataType = parsed.type.name;
        tag.mode = modeOf(tag);
        tag.initialValue = decodeValue(parsed.initialBytes.constData(), int(parsed.initialBytes.size()), parsed.type);
        if (tag.mode == QLatin1String("follow")) tag.source = plan.tags.at(parsed.sourceIndex).address.canonical;
    }
    return out;
}
} // namespace

bool prepareProject(const Project &project, ProjectPlan *plan, QString *error)
{
    if (project.name.trimmed().isEmpty() || project.name.size() > 256)
        return fail(error, I18n::tr("Project name must contain 1 to 256 characters"));
    if (!validBindAddress(project.bindAddress))
        return fail(error, I18n::tr("Bind address must be an IPv4 address, such as 127.0.0.1 or 0.0.0.0"));
    if (project.port == 0) return fail(error, I18n::tr("Port must be between 1 and 65535"));
    // The native server stores each registered size in a 16-bit word.
    if (project.memorySize < 1 || project.memorySize > 65535)
        return fail(error, I18n::tr("Area memory size must be between 1 and 65535 bytes"));
    if (project.tags.size() > kMaximumTags)
        return fail(error, I18n::tr("A project can contain at most %1 tags").arg(kMaximumTags));

    ProjectPlan result;
    result.tags.reserve(project.tags.size());
    QSet<QString> names;
    QSet<int> dbNumbers;
    QHash<QString, int> addresses;
    QHash<QString, QList<int>> areaTags;
    for (int i = 0; i < project.tags.size(); ++i) {
        const Tag &tag = project.tags.at(i);
        const QString label = I18n::tr("Tag %1 (%2)").arg(i + 1).arg(tag.name);
        const QString name = tag.name.trimmed().toCaseFolded();
        if (name.isEmpty() || tag.name.size() > 128 || names.contains(name))
            return fail(error, label + I18n::tr(": Name must be unique and contain 1 to 128 characters"));
        names.insert(name);
        PreparedTag parsed;
        QString why;
        if (!parseAddress(tag.address, &parsed.address, &why) || !parseDataType(tag.dataType, &parsed.type, &why))
            return fail(error, I18n::tr("%1: %2").arg(label, why));
        if ((parsed.type.isBool() != (parsed.address.bit >= 0)) || parsed.type.byteCount != parsed.address.byteCount)
            return fail(error, label + I18n::tr(": Address width does not match data type %1").arg(parsed.type.name));
        if (parsed.address.byteOffset + parsed.type.byteCount > project.memorySize)
            return fail(error, label + I18n::tr(": Address is out of bounds; each area contains %1 bytes").arg(project.memorySize));
        if (!encodeValue(tag.initialValue, parsed.type, &parsed.initialBytes, &why))
            return fail(error, I18n::tr("%1: Invalid initial value. %2").arg(label, why));
        if (!std::isfinite(tag.minimum) || !std::isfinite(tag.maximum) || tag.minimum > tag.maximum
            || !std::isfinite(tag.periodSeconds) || tag.periodSeconds <= 0)
            return fail(error, label + I18n::tr(": Limits must be finite and ordered, and period must be a finite positive number"));
        const QString mode = modeOf(tag);
        if (mode != QLatin1String("manual") && mode != QLatin1String("sine") && mode != QLatin1String("ramp")
            && mode != QLatin1String("toggle") && mode != QLatin1String("follow"))
            return fail(error, label + I18n::tr(": Unknown generator mode: %1").arg(tag.mode));
        if (mode == QLatin1String("toggle") && !parsed.type.isBool())
            return fail(error, label + I18n::tr(": Toggle mode requires BOOL"));
        if (mode == QLatin1String("sine") || mode == QLatin1String("ramp")) {
            if (parsed.type.isBool()) return fail(error, label + I18n::tr(": Sine and ramp modes require a numeric type"));
            QByteArray ignored;
            if (!encodeValue(tag.minimum, parsed.type, &ignored, &why)
                || !encodeValue(tag.maximum, parsed.type, &ignored, &why))
                return fail(error, label + I18n::tr(": Generator limits are outside the data type range; integer types require integer limits"));
        }
        if (parsed.address.area == MemoryArea::DataBlock) dbNumbers.insert(parsed.address.dbNumber);
        areaTags[parsed.address.areaKey()].append(i);
        addresses.insert(parsed.address.canonical, i);
        result.tags.append(parsed);
    }
    if (dbNumbers.size() > 2048)
        return fail(error, I18n::tr("Snap7 supports at most 2048 data blocks"));
    if (qint64(dbNumbers.size() + 3) * project.memorySize > kMaximumMemory)
        return fail(error, I18n::tr("Registered memory cannot exceed 32 MiB; reduce the data block count or area size"));

    for (auto it = areaTags.begin(); it != areaTags.end(); ++it) {
        QList<int> indexes = it.value();
        std::sort(indexes.begin(), indexes.end(), [&result](int a, int b) {
            return result.tags.at(a).address.firstBit() < result.tags.at(b).address.firstBit();
        });
        int previous = -1;
        for (int index : indexes) {
            const Address &address = result.tags.at(index).address;
            if (previous >= 0) {
                const Address &before = result.tags.at(previous).address;
                if (address.firstBit() < before.firstBit() + before.bitCount())
                    return fail(error, I18n::tr("Tag addresses overlap: %1 and %2").arg(project.tags.at(previous).name, project.tags.at(index).name));
            }
            previous = index;
        }
    }

    for (int i = 0; i < project.tags.size(); ++i) {
        const Tag &tag = project.tags.at(i);
        if (modeOf(tag) != QLatin1String("follow")) continue;
        Address source;
        QString why;
        if (!parseAddress(tag.source, &source, &why))
            return fail(error, I18n::tr("Tag %1 has an invalid follow source: %2").arg(tag.name, why));
        const auto found = addresses.constFind(source.canonical);
        if (found == addresses.constEnd())
            return fail(error, I18n::tr("Tag %1 refers to an undefined follow source: %2").arg(tag.name, source.canonical));
        if (result.tags.at(*found).type.kind != result.tags.at(i).type.kind)
            return fail(error, I18n::tr("Tag %1 must have the same data type as its follow source").arg(tag.name));
        result.tags[i].sourceIndex = *found;
    }
    // Kahn's algorithm keeps a maliciously long dependency chain off the stack.
    QList<int> indegree(result.tags.size(), 0);
    QList<QList<int>> dependents(result.tags.size());
    QList<int> ready;
    for (int i = 0; i < result.tags.size(); ++i) {
        const int source = result.tags.at(i).sourceIndex;
        if (source < 0) ready.append(i);
        else { indegree[i] = 1; dependents[source].append(i); }
    }
    for (int cursor = 0; cursor < ready.size(); ++cursor) {
        const int index = ready.at(cursor);
        result.evaluationOrder.append(index);
        for (int dependent : dependents.at(index)) if (--indegree[dependent] == 0) ready.append(dependent);
    }
    if (result.evaluationOrder.size() != result.tags.size())
        return fail(error, I18n::tr("Follow rules contain a dependency cycle"));
    if (plan) *plan = result;
    if (error) error->clear();
    return true;
}

bool Project::validate(QString *error) const { return prepareProject(*this, nullptr, error); }

Project Project::demo()
{
    Project project;
    const auto tag = [](const QString &name, const QString &address, const QString &type,
                        const QVariant &initial, const QString &mode = QStringLiteral("manual"),
                        double lo = 0, double hi = 100, double period = 10, const QString &source = QString()) {
        return Tag{name, address, type, initial, mode, lo, hi, period, source};
    };
    project.tags = {
        tag(QStringLiteral("Running"), QStringLiteral("DB1.DBX0.0"), QStringLiteral("BOOL"), false,
            QStringLiteral("follow"), 0, 1, 10, QStringLiteral("DB1.DBX0.1")),
        tag(QStringLiteral("Start request"), QStringLiteral("DB1.DBX0.1"), QStringLiteral("BOOL"), false),
        tag(QStringLiteral("Temperature"), QStringLiteral("DB1.DBD4"), QStringLiteral("REAL"), 50.0,
            QStringLiteral("sine"), 20, 80, 10),
        tag(QStringLiteral("Pressure"), QStringLiteral("DB1.DBD8"), QStringLiteral("REAL"), 0.0,
            QStringLiteral("ramp"), 0, 10, 10),
        tag(QStringLiteral("Speed"), QStringLiteral("DB1.DBW12"), QStringLiteral("INT"), 1200),
        tag(QStringLiteral("Heartbeat"), QStringLiteral("M0.0"), QStringLiteral("BOOL"), false,
            QStringLiteral("toggle"), 0, 1, 2),
        tag(QStringLiteral("Output"), QStringLiteral("Q0.0"), QStringLiteral("BOOL"), false,
            QStringLiteral("follow"), 0, 1, 10, QStringLiteral("DB1.DBX0.1")),
    };
    return project;
}

bool Project::save(const QString &path, QString *error) const
{
    ProjectPlan plan;
    if (!prepareProject(*this, &plan, error)) return false;
    const Project clean = normalized(*this, plan);
    QJsonArray items;
    for (const Tag &tag : clean.tags) {
        items.append(QJsonObject{{QStringLiteral("name"), tag.name}, {QStringLiteral("address"), tag.address},
                                 {QStringLiteral("dataType"), tag.dataType},
                                 {QStringLiteral("initialValue"), QJsonValue::fromVariant(tag.initialValue)},
                                 {QStringLiteral("mode"), tag.mode}, {QStringLiteral("minimum"), tag.minimum},
                                 {QStringLiteral("maximum"), tag.maximum}, {QStringLiteral("periodSeconds"), tag.periodSeconds},
                                 {QStringLiteral("source"), tag.source}});
    }
    const QJsonObject document{{QStringLiteral("format"), QStringLiteral("FluentPLC-Simulator")},
                               {QStringLiteral("version"), 1}, {QStringLiteral("name"), name},
                               {QStringLiteral("server"), QJsonObject{{QStringLiteral("bindAddress"), bindAddress},
                                                                      {QStringLiteral("port"), int(port)},
                                                                      {QStringLiteral("memorySize"), memorySize}}},
                               {QStringLiteral("tags"), items}};
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return fail(error, I18n::tr("Cannot save project: %1").arg(file.errorString()));
    const QByteArray json = QJsonDocument(document).toJson(QJsonDocument::Indented);
    if (file.write(json) != json.size() || !file.commit())
        return fail(error, I18n::tr("Cannot write project: %1").arg(file.errorString()));
    if (error) error->clear();
    return true;
}

bool Project::load(const QString &path, Project *project, QString *error)
{
    if (!project) return fail(error, I18n::tr("Project output pointer is null"));
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return fail(error, I18n::tr("Cannot open project: %1").arg(file.errorString()));
    if (file.size() > 8 * 1024 * 1024) return fail(error, I18n::tr("Project file cannot exceed 8 MiB"));
    QJsonParseError jsonError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &jsonError);
    if (jsonError.error != QJsonParseError::NoError || !document.isObject())
        return fail(error, I18n::tr("Invalid project JSON: %1").arg(jsonError.errorString()));
    const QJsonObject root = document.object();
    if (root.value(QStringLiteral("format")) != QJsonValue(QStringLiteral("FluentPLC-Simulator"))
        || !integerIn(root.value(QStringLiteral("version")), 1, 1))
        return fail(error, I18n::tr("Unsupported project format or version"));
    if (!root.value(QStringLiteral("name")).isString() || !root.value(QStringLiteral("server")).isObject()
        || !root.value(QStringLiteral("tags")).isArray())
        return fail(error, I18n::tr("Project must contain valid name, server and tags fields"));
    const QJsonObject server = root.value(QStringLiteral("server")).toObject();
    if (!server.value(QStringLiteral("bindAddress")).isString()
        || !integerIn(server.value(QStringLiteral("port")), 1, 65535)
        || !integerIn(server.value(QStringLiteral("memorySize")), 1, 65535))
        return fail(error, I18n::tr("Server bind address, port or memory size is invalid"));
    Project candidate;
    candidate.name = root.value(QStringLiteral("name")).toString();
    candidate.bindAddress = server.value(QStringLiteral("bindAddress")).toString();
    candidate.port = quint16(server.value(QStringLiteral("port")).toInt());
    candidate.memorySize = server.value(QStringLiteral("memorySize")).toInt();
    const QJsonArray items = root.value(QStringLiteral("tags")).toArray();
    if (items.size() > kMaximumTags) return fail(error, I18n::tr("A project can contain at most %1 tags").arg(kMaximumTags));
    for (int i = 0; i < items.size(); ++i) {
        if (!items.at(i).isObject()) return fail(error, I18n::tr("Tag %1 must be an object").arg(i + 1));
        const QJsonObject object = items.at(i).toObject();
        for (const QString &key : {QStringLiteral("name"), QStringLiteral("address"), QStringLiteral("dataType")})
            if (!object.value(key).isString()) return fail(error, I18n::tr("Tag %1 field %2 must be text").arg(i + 1).arg(key));
        const QJsonValue initial = object.value(QStringLiteral("initialValue"));
        if (!initial.isDouble() && !initial.isBool() && !initial.isString())
            return fail(error, I18n::tr("Tag %1 initialValue must be a number, BOOL or numeric text").arg(i + 1));
        Tag tag;
        tag.name = object.value(QStringLiteral("name")).toString();
        tag.address = object.value(QStringLiteral("address")).toString();
        tag.dataType = object.value(QStringLiteral("dataType")).toString();
        tag.initialValue = initial.toVariant();
        for (const QString &key : {QStringLiteral("mode"), QStringLiteral("source")}) {
            if (!object.value(key).isUndefined() && !object.value(key).isString())
                return fail(error, I18n::tr("Tag %1 field %2 must be text").arg(i + 1).arg(key));
        }
        if (object.contains(QStringLiteral("mode"))) tag.mode = object.value(QStringLiteral("mode")).toString();
        if (object.contains(QStringLiteral("source"))) tag.source = object.value(QStringLiteral("source")).toString();
        for (const QString &key : {QStringLiteral("minimum"), QStringLiteral("maximum"), QStringLiteral("periodSeconds")})
            if (!object.value(key).isUndefined() && !finiteNumber(object.value(key)))
                return fail(error, I18n::tr("Tag %1 field %2 must be a finite number").arg(i + 1).arg(key));
        if (object.contains(QStringLiteral("minimum"))) tag.minimum = object.value(QStringLiteral("minimum")).toDouble();
        if (object.contains(QStringLiteral("maximum"))) tag.maximum = object.value(QStringLiteral("maximum")).toDouble();
        if (object.contains(QStringLiteral("periodSeconds"))) tag.periodSeconds = object.value(QStringLiteral("periodSeconds")).toDouble();
        candidate.tags.append(tag);
    }
    ProjectPlan plan;
    if (!prepareProject(candidate, &plan, error)) return false;
    *project = normalized(candidate, plan);
    if (error) error->clear();
    return true;
}
} // namespace fplc
