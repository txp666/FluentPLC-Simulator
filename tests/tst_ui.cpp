#include "app/main_window.h"
#include "plc/plc_server.h"

#include <snap7.h>
#include <QtCore/QFile>
#include <QtCore/QPointer>
#include <QtCore/QSettings>
#include <QtCore/QTemporaryDir>
#include <QtCore/QtEndian>
#include <QtNetwork/QTcpServer>
#include <QtTest/QTest>
#include <QtWidgets/QApplication>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QStyleFactory>
#include <QtWidgets/QTableWidget>

namespace {

quint16 freePort()
{
    QTcpServer reservation;
    if (!reservation.listen(QHostAddress::LocalHost, 0)) return 0;
    return reservation.serverPort();
}

fplc::Project fixture()
{
    fplc::Project project;
    project.name = QStringLiteral("UI communication fixture");
    project.tags = {
        {QStringLiteral("设定值"), QStringLiteral("DB1.DBW0"), QStringLiteral("INT"), 17},
        {QStringLiteral("保留值"), QStringLiteral("DB1.DBW2"), QStringLiteral("INT"), 99},
        {QStringLiteral("启动命令"), QStringLiteral("DB1.DBX4.0"), QStringLiteral("BOOL"), false},
    };
    return project;
}

QLineEdit *activeEditor(QTableWidget *table)
{
    for (auto *editor : table->findChildren<QLineEdit *>()) if (editor->isVisible()) return editor;
    return nullptr;
}

int connectClient(TS7Client &client, quint16 port)
{
    word remotePort = port;
    int timeout = 800;
    int result = client.SetParam(p_u16_RemotePort, &remotePort);
    if (result == 0) result = client.SetParam(p_i32_SendTimeout, &timeout);
    if (result == 0) result = client.SetParam(p_i32_RecvTimeout, &timeout);
    return result == 0 ? client.ConnectTo("127.0.0.1", 0, 1) : result;
}

QByteArray contents(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

} // namespace

class UiTest final : public QObject {
    Q_OBJECT
private slots:
    void defaultWindowDoesNotListen();
    void startAndStopButtonsControlRealS7Connection();
    void tableEditWritesCurrentValueWithoutResettingOthers();
    void projectRoundTripAndFailuresPreserveState();
};

void UiTest::defaultWindowDoesNotListen()
{
    fplc::MainWindow window;
    QVERIFY(!window.server()->isRunning());
    QCOMPARE(window.server()->clientCount(), 0);
    const auto *table = window.findChild<QTableWidget *>(QStringLiteral("tagTable"));
    QVERIFY(table);
    QCOMPARE(table->rowCount(), window.project().tags.size());
    QVERIFY(table->rowCount() > 0);
    auto *start = window.findChild<QPushButton *>(QStringLiteral("startServerButton"));
    auto *stop = window.findChild<QPushButton *>(QStringLiteral("stopServerButton"));
    auto *port = window.findChild<QSpinBox *>(QStringLiteral("serverPortSpin"));
    QVERIFY(start && stop && port);
    QVERIFY(start->isEnabled());
    QVERIFY(!stop->isEnabled());
    QCOMPARE(port->value(), 1102);
}

void UiTest::startAndStopButtonsControlRealS7Connection()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    auto project = fixture();
    project.port = freePort();
    QVERIFY(project.port != 0);
    QString error;
    const QString path = directory.filePath(QStringLiteral("network.plcsim.json"));
    QVERIFY2(project.save(path, &error), qPrintable(error));
    fplc::MainWindow window;
    QVERIFY(window.openProject(path));
    window.show();
    QTRY_VERIFY(window.isVisible());
    auto *start = window.findChild<QPushButton *>(QStringLiteral("startServerButton"));
    auto *stop = window.findChild<QPushButton *>(QStringLiteral("stopServerButton"));
    auto *add = window.findChild<QPushButton *>(QStringLiteral("addTagButton"));
    auto *address = window.findChild<QLineEdit *>(QStringLiteral("serverAddressEdit"));
    QVERIFY(start && stop && add && address);
    QTest::mouseClick(start, Qt::LeftButton);
    QVERIFY(window.server()->isRunning());
    QVERIFY(!start->isEnabled());
    QVERIFY(stop->isEnabled());
    QVERIFY(!add->isEnabled());
    QVERIFY(!address->isEnabled());

    TS7Client client;
    const int connected = connectClient(client, project.port);
    QVERIFY2(connected == 0, qPrintable(QStringLiteral("Snap7 ConnectTo: %1").arg(connected)));
    QByteArray bytes(2, '\0');
    QCOMPARE(client.DBRead(1, 0, 2, bytes.data()), 0);
    QCOMPARE(qFromBigEndian<qint16>(bytes.constData()), qint16(17));
    qToBigEndian<qint16>(77, bytes.data());
    QCOMPARE(client.DBWrite(1, 0, 2, bytes.data()), 0);
    QTRY_COMPARE(window.server()->value(0).toInt(), 77);
    QTRY_COMPARE(window.server()->clientCount(), 1);

    QTest::mouseClick(stop, Qt::LeftButton);
    QVERIFY(!window.server()->isRunning());
    QVERIFY(start->isEnabled());
    QVERIFY(add->isEnabled());
    QVERIFY(address->isEnabled());
    QCOMPARE(window.server()->value(0).toInt(), 77);
    QVERIFY(client.DBRead(1, 0, 2, bytes.data()) != 0);
    client.Disconnect();
}

void UiTest::tableEditWritesCurrentValueWithoutResettingOthers()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QString error;
    const QString path = directory.filePath(QStringLiteral("table.plcsim.json"));
    QVERIFY2(fixture().save(path, &error), qPrintable(error));
    fplc::MainWindow window;
    QVERIFY(window.openProject(path));
    QVERIFY2(window.server()->setValue(1, 321, &error), qPrintable(error));
    window.show();
    QTRY_VERIFY(window.isVisible());
    auto *table = window.findChild<QTableWidget *>(QStringLiteral("tagTable"));
    QVERIFY(table);
    table->setCurrentCell(0, 3);
    table->scrollToItem(table->item(0, 3));
    table->setFocus();
    table->editItem(table->item(0, 3));
    QTRY_VERIFY(activeEditor(table));
    QPointer<QLineEdit> editor = activeEditor(table);
    editor->selectAll();
    QTest::keyClicks(editor, QStringLiteral("42"));
    // A refresh happens while the delegate is open; it must not overwrite pending text.
    QTest::qWait(350);
    QCOMPARE(editor->text(), QStringLiteral("42"));
    QTest::keyClick(editor, Qt::Key_Return);
    QTRY_COMPARE(window.server()->value(0).toInt(), 42);
    QCOMPARE(window.server()->value(1).toInt(), 321);
    QCOMPARE(window.project().tags.at(0).initialValue.toInt(), 17);
    QTRY_COMPARE(table->item(0, 3)->text(), QStringLiteral("42"));

    auto *reset = window.findChild<QPushButton *>(QStringLiteral("resetValuesButton"));
    QVERIFY(reset && reset->isEnabled());
    QTest::mouseClick(reset, Qt::LeftButton);
    QCOMPARE(window.server()->value(0).toInt(), 17);
    QCOMPARE(window.server()->value(1).toInt(), 99);
}

void UiTest::projectRoundTripAndFailuresPreserveState()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto project = fixture();
    const QString source = directory.filePath(QStringLiteral("source.plcsim.json"));
    const QString saved = directory.filePath(QStringLiteral("saved.plcsim.json"));
    QString error;
    QVERIFY2(project.save(source, &error), qPrintable(error));
    fplc::MainWindow window;
    QVERIFY(window.openProject(source));
    QVERIFY(window.saveProject(saved));
    fplc::Project reloaded;
    QVERIFY2(fplc::Project::load(saved, &reloaded, &error), qPrintable(error));
    QCOMPARE(reloaded.name, project.name);
    QCOMPARE(reloaded.port, project.port);
    QCOMPARE(reloaded.tags.size(), project.tags.size());
    QCOMPARE(reloaded.tags.at(0).address, project.tags.at(0).address);
    QCOMPARE(reloaded.tags.at(0).initialValue.toInt(), 17);
    const QByteArray before = contents(saved);
    QVERIFY(!before.isEmpty());

    QVERIFY2(window.server()->setValue(0, 444, &error), qPrintable(error));
    const QString invalid = directory.filePath(QStringLiteral("invalid.plcsim.json"));
    QFile file(invalid);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QVERIFY(file.write("{broken") > 0);
    file.close();
    auto *pathLabel = window.findChild<QLabel *>(QStringLiteral("projectPathLabel"));
    QVERIFY(pathLabel);
    const QString originalPathLabel = pathLabel->text();
    QVERIFY(!window.openProject(invalid));
    QCOMPARE(window.project().name, project.name);
    QCOMPARE(window.project().tags.size(), project.tags.size());
    QCOMPARE(window.server()->value(0).toInt(), 444);
    QCOMPARE(pathLabel->text(), originalPathLabel);

    auto *name = window.findChild<QLineEdit *>(QStringLiteral("projectNameEdit"));
    QVERIFY(name);
    name->setText(QString());
    QVERIFY(!window.saveProject(saved));
    QCOMPARE(contents(saved), before);
    QCOMPARE(window.server()->project().name, project.name);
    QCOMPARE(window.server()->value(0).toInt(), 444);
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    Q_INIT_RESOURCE(fluentqtwidgets);
    app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    app.setOrganizationName(QStringLiteral("FluentPLC-Tests"));
    app.setApplicationName(QStringLiteral("UI test"));
    QTemporaryDir settings;
    if (!settings.isValid()) return 1;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    UiTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_ui.moc"
