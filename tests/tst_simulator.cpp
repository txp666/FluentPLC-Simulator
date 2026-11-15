#include "core/project.h"
#include "plc/plc_server.h"
#include <snap7.h>

#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QTemporaryDir>
#include <QtCore/QtEndian>
#include <QtNetwork/QTcpServer>
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>

#include <cmath>
#include <cstring>

Q_DECLARE_METATYPE(fplc::Project)

namespace {

fplc::Tag tag(const QString &name, const QString &address, const QString &type,
              const QVariant &initial, const QString &mode = QStringLiteral("manual"),
              const QString &source = {})
{
    fplc::Tag result;
    result.name = name;
    result.address = address;
    result.dataType = type;
    result.initialValue = initial;
    result.mode = mode;
    result.source = source;
    return result;
}

fplc::Project fixture()
{
    fplc::Project project;
    project.name = QStringLiteral("QtTest S7 fixture");
    project.tags = {
        tag("Temperature", "DB1.DBD0", "REAL", 21.5),
        tag("Setpoint", "DB1.DBW4", "INT", -25),
        tag("Command", "DB1.DBX6.0", "BOOL", false),
        tag("Neighbour", "DB1.DBX6.1", "BOOL", true),
        tag("Feedback", "DB1.DBX6.2", "BOOL", false, "follow", "DB1.DBX6.0"),
        tag("Marker", "MW0", "WORD", 1234),
    };
    return project;
}

bool writeFile(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        && file.write(bytes) == bytes.size();
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

bool startOnFreePort(fplc::PlcServer &server, fplc::Project project, QString *error)
{
    // Let the OS choose a high unprivileged port. The tiny reservation-to-start
    // race is retried so simultaneous CI jobs need no fixed port coordination.
    for (int attempt = 0; attempt < 5; ++attempt) {
        QTcpServer reservation;
        if (!reservation.listen(QHostAddress::LocalHost, 0)) {
            *error = reservation.errorString();
            return false;
        }
        project.port = reservation.serverPort();
        reservation.close();
        if (!server.setProject(project, error))
            return false;
        if (server.start(error))
            return true;
    }
    return false;
}

int connectClient(TS7Client &client, quint16 port)
{
    word remotePort = port;
    int timeout = 1000;
    int result = client.SetParam(p_u16_RemotePort, &remotePort);
    if (result == 0)
        result = client.SetParam(p_i32_SendTimeout, &timeout);
    if (result == 0)
        result = client.SetParam(p_i32_RecvTimeout, &timeout);
    return result == 0 ? client.ConnectTo("127.0.0.1", 0, 1) : result;
}

QByteArray realBytes(float value)
{
    quint32 bits = 0;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    QByteArray data(4, '\0');
    qToBigEndian(bits, data.data());
    return data;
}

float readReal(const QByteArray &bytes)
{
    const quint32 bits = qFromBigEndian<quint32>(bytes.constData());
    float value = 0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

} // namespace

class SimulatorTest final : public QObject {
    Q_OBJECT
private slots:
    void projectRoundTrip();
    void bundledExamplesAreValid();
    void invalidProjects_data();
    void invalidProjects();
    void failedLoadPreservesDestination();
    void failedSavePreservesExistingFile();
    void realClientReadsAndWritesTypedValues();
    void typedWireVectors_data();
    void typedWireVectors();
    void memoryAreasAreIndependent();
    void realBitWritePreservesNeighbourAndFollow();
    void manualValuesSurviveTicksAndUiChangesReachNetwork();
    void deterministicGeneratorsAndFollow();
    void followChainIsTopological();
    void invalidNetworkAreaDoesNotBreakHealthyReads();
    void stopRestartAndProjectReplacement();
    void repeatedClientDisconnectsAndActiveServerStops();
};

void SimulatorTest::projectRoundTrip()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    auto original = fixture();
    original.name = QStringLiteral("温度 \"S7\" 测试台");
    original.port = 21102;
    original.tags[0].mode = "sine";
    original.tags[0].minimum = -12.5;
    original.tags[0].maximum = 47.25;
    original.tags[0].periodSeconds = 3.75;
    QString error;
    const auto path = directory.filePath("roundtrip.plcsim.json");
    QVERIFY2(original.save(path, &error), qPrintable(error));
    const auto document = QJsonDocument::fromJson(readFile(path));
    QCOMPARE(document.object().value("format").toString(), QString("FluentPLC-Simulator"));
    QCOMPARE(document.object().value("version").toInt(), 1);
    fplc::Project loaded;
    QVERIFY2(fplc::Project::load(path, &loaded, &error), qPrintable(error));
    QCOMPARE(loaded.name, original.name);
    QCOMPARE(loaded.bindAddress, original.bindAddress);
    QCOMPARE(loaded.port, original.port);
    QCOMPARE(loaded.memorySize, original.memorySize);
    QCOMPARE(loaded.tags.size(), original.tags.size());
    QCOMPARE(loaded.tags[0].mode, original.tags[0].mode);
    QCOMPARE(loaded.tags[0].minimum, original.tags[0].minimum);
    QCOMPARE(loaded.tags[0].maximum, original.tags[0].maximum);
    QCOMPARE(loaded.tags[0].periodSeconds, original.tags[0].periodSeconds);
    QCOMPARE(loaded.tags[4].source, original.tags[4].source);
    const auto second = directory.filePath("second.plcsim.json");
    QVERIFY2(loaded.save(second, &error), qPrintable(error));
    QCOMPARE(QJsonDocument::fromJson(readFile(second)), document);
}

void SimulatorTest::bundledExamplesAreValid()
{
    QString error;
    QVERIFY2(fplc::Project::demo().validate(&error), qPrintable(error));
    const auto path = QFINDTESTDATA("../examples/hmi-demo.plcsim.json");
    QVERIFY(!path.isEmpty());
    fplc::Project project;
    QVERIFY2(fplc::Project::load(path, &project, &error), qPrintable(error));
    QCOMPARE(project.tags.size(), 7);
    QCOMPARE(project.tags[0].address, QString("DB1.DBX0.0"));
    QCOMPARE(project.tags[0].source, QString("DB1.DBX0.1"));
}

void SimulatorTest::invalidProjects_data()
{
    QTest::addColumn<fplc::Project>("project");
    auto add = [](const char *name, const fplc::Project &project) {
        QTest::newRow(name) << project;
    };
    auto project = fixture();
    project.memorySize = 0;
    add("zero-memory", project);
    project = fixture();
    project.memorySize = 65536;
    add("oversized-memory", project);
    project = fixture();
    project.tags[0].address = "DB1.DBD510";
    add("real-crosses-area-boundary", project);
    project = fixture();
    project.tags[2].address = "DB1.DBX6.8";
    add("invalid-bit-index", project);
    project = fixture();
    project.tags[0].dataType = "TEXT";
    add("unsupported-type", project);
    project = fixture();
    project.tags[0].address = "DB1.DBW0";
    add("address-type-width-mismatch", project);
    project = fixture();
    project.tags[2].address = "DB1.DBX1.0";
    add("bit-overlaps-real", project);
    project = fixture();
    project.tags[3].address = "db01.dbx006.0";
    add("canonical-duplicate-bit", project);
    project = fixture();
    project.tags[1].initialValue = 40000;
    add("int-initial-value-overflow", project);
    project = fixture();
    project.tags[0].mode = "ladder";
    add("unsupported-rule", project);
    project = fixture();
    project.tags[0].mode = "sine";
    project.tags[0].periodSeconds = 0;
    add("zero-rule-period", project);
    project = fixture();
    project.tags[4].source = "DB9.DBX0.0";
    add("missing-follow-source", project);
    project = fixture();
    project.tags[4].source = "DB1.DBD0";
    add("follow-type-mismatch", project);
    project = fixture();
    project.tags[2].mode = "follow";
    project.tags[2].source = "DB1.DBX6.2";
    add("follow-cycle", project);
    project = fixture();
    project.tags[4].source = "DB1.DBX6.2";
    add("follow-self-cycle", project);
}

void SimulatorTest::invalidProjects()
{
    QFETCH(fplc::Project, project);
    QString error;
    QVERIFY(!project.validate(&error));
    QVERIFY(!error.isEmpty());
}

void SimulatorTest::failedLoadPreservesDestination()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QString error;
    const auto path = directory.filePath("invalid.plcsim.json");
    QVERIFY2(fixture().save(path, &error), qPrintable(error));
    const auto baseline = QJsonDocument::fromJson(readFile(path)).object();
    QList<QByteArray> invalid = {"{", "[]"};
    auto root = baseline;
    root["format"] = "Other-Application";
    invalid << QJsonDocument(root).toJson();
    root = baseline;
    root["version"] = 2;
    invalid << QJsonDocument(root).toJson();
    root = baseline;
    auto server = root["server"].toObject();
    server["port"] = "1102";
    root["server"] = server;
    invalid << QJsonDocument(root).toJson();
    root = baseline;
    auto tags = root["tags"].toArray();
    auto first = tags[0].toObject();
    first["dataType"] = "INVALID";
    tags[0] = first;
    root["tags"] = tags;
    invalid << QJsonDocument(root).toJson();
    for (const auto &bytes : invalid) {
        QVERIFY(writeFile(path, bytes));
        auto destination = fixture();
        destination.name = "sentinel-preserved";
        destination.port = 31002;
        error.clear();
        QVERIFY(!fplc::Project::load(path, &destination, &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(destination.name, QString("sentinel-preserved"));
        QCOMPARE(destination.port, quint16(31002));
        QCOMPARE(destination.tags.size(), fixture().tags.size());
        QCOMPARE(destination.tags[0].address, QString("DB1.DBD0"));
        QCOMPARE(destination.tags[0].initialValue.toDouble(), 21.5);
    }
}

void SimulatorTest::failedSavePreservesExistingFile()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QString error;
    const auto path = directory.filePath("keep.plcsim.json");
    QVERIFY2(fixture().save(path, &error), qPrintable(error));
    const auto before = readFile(path);
    auto invalid = fixture();
    invalid.memorySize = 0;
    QVERIFY(!invalid.save(path, &error));
    QCOMPARE(readFile(path), before);
}

void SimulatorTest::realClientReadsAndWritesTypedValues()
{
    fplc::PlcServer server;
    QString error;
    QVERIFY2(startOnFreePort(server, fixture(), &error), qPrintable(error));
    QVERIFY(server.isRunning());
    TS7Client client;
    QCOMPARE(connectClient(client, server.project().port), 0);
    QByteArray value(4, '\0');
    QCOMPARE(client.DBRead(1, 0, value.size(), value.data()), 0);
    QCOMPARE(readReal(value), 21.5f);
    auto newValue = realBytes(-13.25f);
    QCOMPARE(client.DBWrite(1, 0, newValue.size(), newValue.data()), 0);
    QCOMPARE(server.value(0).toDouble(), -13.25);
    QByteArray integer(2, '\0');
    QCOMPARE(client.DBRead(1, 4, integer.size(), integer.data()), 0);
    QCOMPARE(qFromBigEndian<qint16>(integer.constData()), qint16(-25));
    qToBigEndian<qint16>(-1234, integer.data());
    QCOMPARE(client.DBWrite(1, 4, integer.size(), integer.data()), 0);
    QCOMPARE(server.value(1).toInt(), -1234);
    QCOMPARE(client.Disconnect(), 0);
}

void SimulatorTest::typedWireVectors_data()
{
    QTest::addColumn<QString>("type");
    QTest::addColumn<QString>("address");
    QTest::addColumn<QVariant>("initial");
    QTest::addColumn<QByteArray>("initialBytes");
    QTest::addColumn<QByteArray>("writtenBytes");
    QTest::addColumn<double>("writtenValue");
    auto add = [](const char *name, const char *type, const char *address, double initial,
                  const char *initialHex, const char *writtenHex, double writtenValue) {
        QTest::newRow(name) << QString(type) << QString(address) << QVariant(initial)
                           << QByteArray::fromHex(initialHex) << QByteArray::fromHex(writtenHex)
                           << writtenValue;
    };
    add("BYTE-max", "BYTE", "DB3.DBB0", 255, "ff", "00", 0);
    add("SINT-min", "SINT", "DB3.DBB0", -128, "80", "7f", 127);
    add("WORD-max", "WORD", "DB3.DBW0", 65535, "ffff", "8000", 32768);
    add("INT-min", "INT", "DB3.DBW0", -32768, "8000", "7fff", 32767);
    add("DWORD-max", "DWORD", "DB3.DBD0", 4294967295.0, "ffffffff", "80000000", 2147483648.0);
    add("DINT-min", "DINT", "DB3.DBD0", -2147483648.0, "80000000", "7fffffff", 2147483647.0);
    add("REAL-negative", "REAL", "DB3.DBD0", -13.25, "c1540000", "3f800000", 1.0);
    add("uint16-alias", "uint16", "DB3.DBW0", 4660, "1234", "ffff", 65535);
    add("float32-alias", "float32", "DB3.DBD0", 1.5, "3fc00000", "c1540000", -13.25);
}

void SimulatorTest::typedWireVectors()
{
    QFETCH(QString, type);
    QFETCH(QString, address);
    QFETCH(QVariant, initial);
    QFETCH(QByteArray, initialBytes);
    QFETCH(QByteArray, writtenBytes);
    QFETCH(double, writtenValue);
    auto project = fixture();
    project.tags = {tag("Vector", address, type, initial)};
    fplc::PlcServer server;
    QString error;
    QVERIFY2(startOnFreePort(server, project, &error), qPrintable(error));
    TS7Client client;
    QCOMPARE(connectClient(client, server.project().port), 0);
    QByteArray actual(initialBytes.size(), '\0');
    QCOMPARE(client.DBRead(3, 0, actual.size(), actual.data()), 0);
    QCOMPARE(actual, initialBytes);
    QCOMPARE(client.DBWrite(3, 0, writtenBytes.size(), writtenBytes.data()), 0);
    QCOMPARE(server.value(0).toDouble(), writtenValue);
    QCOMPARE(client.DBRead(3, 0, actual.size(), actual.data()), 0);
    QCOMPARE(actual, writtenBytes);
}

void SimulatorTest::memoryAreasAreIndependent()
{
    auto project = fixture();
    project.tags = {
        tag("DB", "DB1.DBB0", "BYTE", 17),
        tag("M", "MB0", "BYTE", 34),
        tag("I", "IB0", "BYTE", 51),
        tag("Q", "QB0", "BYTE", 68),
    };
    fplc::PlcServer server;
    QString error;
    QVERIFY2(startOnFreePort(server, project, &error), qPrintable(error));
    TS7Client client;
    QCOMPARE(connectClient(client, server.project().port), 0);
    const int areas[] = {S7AreaDB, S7AreaMK, S7AreaPE, S7AreaPA};
    for (int i = 0; i < 4; ++i) {
        byte actual = 0;
        QCOMPARE(client.ReadArea(areas[i], i == 0 ? 1 : 0, 0, 1, S7WLByte, &actual), 0);
        QCOMPARE(actual, byte((i + 1) * 17));
    }
    byte marker = 0xaa;
    QCOMPARE(client.WriteArea(S7AreaMK, 0, 0, 1, S7WLByte, &marker), 0);
    QCOMPARE(server.value(1).toUInt(), 0xaau);
    QCOMPARE(server.value(0).toInt(), 17);
    QCOMPARE(server.value(2).toInt(), 51);
    QCOMPARE(server.value(3).toInt(), 68);
}

void SimulatorTest::realBitWritePreservesNeighbourAndFollow()
{
    fplc::PlcServer server;
    QString error;
    QVERIFY2(startOnFreePort(server, fixture(), &error), qPrintable(error));
    TS7Client client;
    QCOMPARE(connectClient(client, server.project().port), 0);
    byte bit = 1;
    QCOMPARE(client.WriteArea(S7AreaDB, 1, 6 * 8, 1, S7WLBit, &bit), 0);
    byte data = 0;
    QCOMPARE(client.DBRead(1, 6, 1, &data), 0);
    QCOMPARE(data & 0x03, 0x03);
    QCOMPARE(server.value(2).toBool(), true);
    QCOMPARE(server.value(3).toBool(), true);
    server.tick(1.0);
    QCOMPARE(server.value(4).toBool(), true);
    QCOMPARE(client.DBRead(1, 6, 1, &data), 0);
    QCOMPARE(data & 0x07, 0x07);
    bit = 0;
    QCOMPARE(client.WriteArea(S7AreaDB, 1, 6 * 8, 1, S7WLBit, &bit), 0);
    server.tick(1.5);
    QCOMPARE(client.DBRead(1, 6, 1, &data), 0);
    QCOMPARE(data & 0x07, 0x02);
}

void SimulatorTest::manualValuesSurviveTicksAndUiChangesReachNetwork()
{
    fplc::PlcServer server;
    QString error;
    QVERIFY2(startOnFreePort(server, fixture(), &error), qPrintable(error));
    TS7Client client;
    QCOMPARE(connectClient(client, server.project().port), 0);
    auto networkValue = realBytes(72.125f);
    QCOMPARE(client.DBWrite(1, 0, networkValue.size(), networkValue.data()), 0);
    server.tick(100.0);
    QCOMPARE(server.value(0).toDouble(), 72.125);
    QSignalSpy changes(&server, &fplc::PlcServer::valuesChanged);
    QVERIFY2(server.setValue(0, 36.5, &error), qPrintable(error));
    QVERIFY(!changes.isEmpty());
    QByteArray value(4, '\0');
    QCOMPARE(client.DBRead(1, 0, value.size(), value.data()), 0);
    QCOMPARE(readReal(value), 36.5f);
    QVERIFY(!server.setValue(1, 40000, &error));
    QCOMPARE(server.value(1).toInt(), -25);
}

void SimulatorTest::deterministicGeneratorsAndFollow()
{
    auto project = fixture();
    project.tags = {
        tag("Sine", "DB2.DBD0", "REAL", 0, "sine"),
        tag("Ramp", "DB2.DBD4", "REAL", 0, "ramp"),
        tag("Toggle", "DB2.DBX8.0", "BOOL", false, "toggle"),
        tag("Copy", "DB2.DBD12", "REAL", 0, "follow", "DB2.DBD4"),
    };
    project.tags[0].minimum = 10;
    project.tags[0].maximum = 20;
    for (auto &item : project.tags)
        item.periodSeconds = 4;
    fplc::PlcServer server;
    QString error;
    QVERIFY2(server.setProject(project, &error), qPrintable(error));
    server.tick(1.0);
    QCOMPARE(server.value(0).toDouble(), 0.0); // stopped clocks do not change memory
    QVERIFY2(startOnFreePort(server, project, &error), qPrintable(error));
    server.tick(1.0);
    QVERIFY(std::abs(server.value(0).toDouble() - 20.0) < 0.0001);
    QVERIFY(std::abs(server.value(1).toDouble() - 25.0) < 0.0001);
    QCOMPARE(server.value(2).toBool(), false);
    QCOMPARE(server.value(3).toDouble(), server.value(1).toDouble());
    server.tick(2.0);
    QVERIFY(std::abs(server.value(0).toDouble() - 15.0) < 0.0001);
    QVERIFY(std::abs(server.value(1).toDouble() - 50.0) < 0.0001);
    QCOMPARE(server.value(2).toBool(), true);
    QCOMPARE(server.value(3).toDouble(), 50.0);
    server.tick(4.0);
    QVERIFY(std::abs(server.value(0).toDouble() - 15.0) < 0.0001);
    QCOMPARE(server.value(1).toDouble(), 0.0);
    QCOMPARE(server.value(2).toBool(), false);
}

void SimulatorTest::followChainIsTopological()
{
    auto project = fixture();
    // Dependents deliberately precede their sources in document order.
    project.tags = {
        tag("Last", "DB1.DBD0", "REAL", 0, "follow", "DB1.DBD4"),
        tag("Middle", "DB1.DBD4", "REAL", 0, "follow", "DB1.DBD8"),
        tag("Source", "DB1.DBD8", "REAL", 12.5),
    };
    fplc::PlcServer server;
    QString error;
    QVERIFY2(startOnFreePort(server, project, &error), qPrintable(error));
    QVERIFY2(server.setValue(2, 77.5, &error), qPrintable(error));
    server.tick(1.0);
    QCOMPARE(server.value(0).toDouble(), 77.5);
    QCOMPARE(server.value(1).toDouble(), 77.5);
    QCOMPARE(server.value(2).toDouble(), 77.5);
}

void SimulatorTest::invalidNetworkAreaDoesNotBreakHealthyReads()
{
    fplc::PlcServer server;
    QString error;
    QVERIFY2(startOnFreePort(server, fixture(), &error), qPrintable(error));
    TS7Client client;
    QCOMPARE(connectClient(client, server.project().port), 0);
    QByteArray bytes(4, '\0');
    QVERIFY(client.DBRead(999, 0, bytes.size(), bytes.data()) != 0);
    QVERIFY(client.DBRead(1, 511, bytes.size(), bytes.data()) != 0);
    QCOMPARE(client.DBRead(1, 0, bytes.size(), bytes.data()), 0);
    QCOMPARE(readReal(bytes), 21.5f);
}

void SimulatorTest::stopRestartAndProjectReplacement()
{
    fplc::PlcServer server;
    QString error;
    QVERIFY2(startOnFreePort(server, fixture(), &error), qPrintable(error));
    TS7Client client;
    QCOMPARE(connectClient(client, server.project().port), 0);
    QVERIFY2(server.setValue(0, 37.25, &error), qPrintable(error));
    auto replacement = server.project();
    replacement.name = "Replacement";
    replacement.tags[0].initialValue = -7.5;
    QVERIFY(!server.setProject(replacement, &error));
    QCOMPARE(server.project().name, QString("QtTest S7 fixture"));
    QCOMPARE(server.value(0).toDouble(), 37.25);
    server.stop();
    QVERIFY(!server.isRunning());
    client.Disconnect();
    QVERIFY2(server.start(&error), qPrintable(error));
    QCOMPARE(server.value(0).toDouble(), 37.25);
    QCOMPARE(connectClient(client, server.project().port), 0);
    QByteArray bytes(4, '\0');
    QCOMPARE(client.DBRead(1, 0, bytes.size(), bytes.data()), 0);
    QCOMPARE(readReal(bytes), 37.25f);
    client.Disconnect();
    server.stop();
    auto invalid = replacement;
    invalid.memorySize = 0;
    QVERIFY(!server.setProject(invalid, &error));
    QCOMPARE(server.value(0).toDouble(), 37.25);
    QVERIFY2(server.setProject(replacement, &error), qPrintable(error));
    QCOMPARE(server.value(0).toDouble(), -7.5);
    QVERIFY2(server.start(&error), qPrintable(error));
    QCOMPARE(connectClient(client, server.project().port), 0);
    QCOMPARE(client.DBRead(1, 0, bytes.size(), bytes.data()), 0);
    QCOMPARE(readReal(bytes), -7.5f);
}

void SimulatorTest::repeatedClientDisconnectsAndActiveServerStops()
{
    fplc::PlcServer server;
    QString error;
    QVERIFY2(startOnFreePort(server, fixture(), &error), qPrintable(error));
    for (int round = 0; round < 20; ++round) {
        TS7Client client;
        QCOMPARE(connectClient(client, server.project().port), 0);
        QByteArray bytes(4, '\0');
        QCOMPARE(client.DBRead(1, 0, bytes.size(), bytes.data()), 0);
        QCOMPARE(readReal(bytes), 21.5f);
        byte bit = byte(round % 2);
        QCOMPARE(client.WriteArea(S7AreaDB, 1, 6 * 8, 1, S7WLBit, &bit), 0);
        if (round % 4 == 3) {
            // Stop while the native client is still connected, then reuse the
            // same registered buffers and port with a fresh native server.
            server.stop();
            QVERIFY(!server.isRunning());
            client.Disconnect();
            QVERIFY2(server.start(&error), qPrintable(error));
        } else {
            QCOMPARE(client.Disconnect(), 0);
        }
    }
}

QTEST_GUILESS_MAIN(SimulatorTest)
#include "tst_simulator.moc"
