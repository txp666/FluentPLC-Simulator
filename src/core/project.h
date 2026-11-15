#pragma once

#include <QtCore/QList>
#include <QtCore/QString>
#include <QtCore/QVariant>
#include <QtCore/QtGlobal>

namespace fplc {

struct Tag {
    QString name;
    QString address;
    QString dataType = QStringLiteral("REAL");
    QVariant initialValue = 0;
    QString mode = QStringLiteral("manual"); // manual, sine, ramp, toggle, follow
    double minimum = 0;
    double maximum = 100;
    double periodSeconds = 10;
    QString source; // address of the tag a follow rule reads
};

struct Project {
    QString name = QStringLiteral("S7 测试台");
    QString bindAddress = QStringLiteral("127.0.0.1");
    quint16 port = 1102;
    int memorySize = 512; // bytes per registered area / DB
    QList<Tag> tags;

    static Project demo();
    bool validate(QString *error = nullptr) const;
    bool save(const QString &path, QString *error = nullptr) const;
    static bool load(const QString &path, Project *project, QString *error = nullptr);
};

} // namespace fplc
