#include "app/main_window.h"
#include "core/i18n.h"
#include "plc/plc_server.h"

#include <FluentQtWidgets/Theme.h>
#include <FluentQtWidgets/ThemeListener.h>
#include <FluentQtWidgets/Widgets/ComboBox.h>

#include <snap7.h>
#include <QtCore/QFile>
#include <QtCore/QPointer>
#include <QtCore/QSettings>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtCore/QtEndian>
#include <QtNetwork/QTcpServer>
#include <QtTest/QTest>
#include <QtTest/QSignalSpy>
#include <QtWidgets/QApplication>
#include <QtWidgets/QDialog>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QScrollBar>
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
    void languageSelectionIsImmediateAndPersistent();
    void themeSelectionIsImmediateAndPersistent();
    void openVariableDialogKeepsInputWhenLanguageChanges();
    void repositoryCardIsTranslatedAndReachable();
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
    auto *language = window.findChild<FluentQt::ComboBox *>(QStringLiteral("languageCombo"));
    auto *theme = window.findChild<FluentQt::ComboBox *>(QStringLiteral("themeCombo"));
    QVERIFY(language && theme);
    QCOMPARE(fplc::I18n::setting(), QStringLiteral("Auto"));
    QCOMPARE(language->currentData().toString(), QStringLiteral("Auto"));
    QCOMPARE(theme->currentData().toString(), QStringLiteral("Auto"));
    QCOMPARE(FluentQt::ThemeManager::instance()->theme(), FluentQt::Theme::Auto);
    QVERIFY(FluentQt::ThemeManager::instance()->systemThemeListener()->isRunning());
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

    auto *language = window.findChild<FluentQt::ComboBox *>(QStringLiteral("languageCombo"));
    auto *theme = window.findChild<FluentQt::ComboBox *>(QStringLiteral("themeCombo"));
    QVERIFY(language && theme);
    auto *server = window.server();
    const auto *projectReference = &window.project();
    const auto *tagDefinitions = window.project().tags.constData();
    const QString projectName = window.project().name;
    QVERIFY2(server->setValue(1, 321, &error), qPrintable(error));
    QSignalSpy stateChanges(server, &fplc::PlcServer::stateChanged);
    for (const QString &locale : {QStringLiteral("zh_CN"), QStringLiteral("en_US"), QStringLiteral("Auto")}) {
        language->setCurrentIndex(language->findData(locale));
        QTRY_COMPARE(fplc::I18n::setting(), locale);
        QCOMPARE(window.server(), server);
        QCOMPARE(&window.project(), projectReference);
        QCOMPARE(window.project().tags.constData(), tagDefinitions);
        QCOMPARE(window.project().name, projectName);
        QCOMPARE(server->value(0).toInt(), 77);
        QCOMPARE(server->value(1).toInt(), 321);
        QCOMPARE(client.DBRead(1, 0, 2, bytes.data()), 0);
        QCOMPARE(qFromBigEndian<qint16>(bytes.constData()), qint16(77));
        QCOMPARE(stateChanges.count(), 0);
        QVERIFY(!window.isWindowModified());
    }
    for (const QString &setting : {QStringLiteral("Light"), QStringLiteral("Dark"), QStringLiteral("Auto")}) {
        theme->setCurrentIndex(theme->findData(setting));
        QCOMPARE(QSettings().value(QStringLiteral("ui/theme")).toString(), setting);
        QVERIFY(server->isRunning());
        QCOMPARE(server->clientCount(), 1);
        QCOMPARE(server->value(0).toInt(), 77);
        QCOMPARE(server->value(1).toInt(), 321);
        QCOMPARE(client.DBRead(1, 0, 2, bytes.data()), 0);
        QCOMPARE(qFromBigEndian<qint16>(bytes.constData()), qint16(77));
        QCOMPARE(window.project().tags.constData(), tagDefinitions);
        QCOMPARE(stateChanges.count(), 0);
        QVERIFY(!window.isWindowModified());
    }

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

void UiTest::languageSelectionIsImmediateAndPersistent()
{
    QVERIFY(fplc::I18n::setLanguage(QStringLiteral("en_US"), false));
    fplc::MainWindow window;
    auto *language = window.findChild<FluentQt::ComboBox *>(QStringLiteral("languageCombo"));
    auto *start = window.findChild<QPushButton *>(QStringLiteral("startServerButton"));
    auto *table = window.findChild<QTableWidget *>(QStringLiteral("tagTable"));
    QVERIFY(language && start && table);
    QCOMPARE(start->text(), QStringLiteral("Start server"));
    QCOMPARE(table->horizontalHeaderItem(3)->text(), QStringLiteral("Current value"));
    QCOMPARE(language->itemData(0).toString(), QStringLiteral("Auto"));
    QCOMPARE(language->itemData(1).toString(), QStringLiteral("en_US"));
    QCOMPARE(language->itemData(2).toString(), QStringLiteral("zh_CN"));
    const QString userName = window.project().tags.first().name;
    const QString projectName = window.project().name;
    const auto *tagDefinitions = window.project().tags.constData();

    language->setCurrentIndex(language->findData(QStringLiteral("zh_CN")));
    QTRY_COMPARE(fplc::I18n::language(), QStringLiteral("zh_CN"));
    QTRY_COMPARE(start->text(), QStringLiteral("启动服务"));
    QCOMPARE(table->horizontalHeaderItem(3)->text(), QStringLiteral("当前值"));
    QCOMPARE(table->item(0, 0)->text(), userName);
    QCOMPARE(window.project().name, projectName);
    QCOMPARE(window.project().tags.constData(), tagDefinitions);
    QCOMPARE(QSettings().value(QStringLiteral("ui/language")).toString(), QStringLiteral("zh_CN"));
    QVERIFY(!window.isWindowModified());

    // Recreate startup selection from the stored preference, not the current process override.
    QVERIFY(fplc::I18n::setLanguage(QStringLiteral("en_US"), false));
    fplc::I18n::initialize(qApp);
    QTRY_COMPARE(fplc::I18n::setting(), QStringLiteral("zh_CN"));
    QTRY_COMPARE(start->text(), QStringLiteral("启动服务"));
    QCOMPARE(language->currentData().toString(), QStringLiteral("zh_CN"));
    language->setCurrentIndex(language->findData(QStringLiteral("en_US")));
    QTRY_COMPARE(start->text(), QStringLiteral("Start server"));
    QCOMPARE(table->horizontalHeaderItem(3)->text(), QStringLiteral("Current value"));
    QCOMPARE(QSettings().value(QStringLiteral("ui/language")).toString(), QStringLiteral("en_US"));
    language->setCurrentIndex(language->findData(QStringLiteral("Auto")));
    QTRY_COMPARE(fplc::I18n::setting(), QStringLiteral("Auto"));
    QCOMPARE(language->currentData().toString(), QStringLiteral("Auto"));
    QCOMPARE(QSettings().value(QStringLiteral("ui/language")).toString(), QStringLiteral("Auto"));
}

void UiTest::themeSelectionIsImmediateAndPersistent()
{
    fplc::MainWindow window;
    auto *theme = window.findChild<FluentQt::ComboBox *>(QStringLiteral("themeCombo"));
    auto *manager = FluentQt::ThemeManager::instance();
    QVERIFY(theme);
    QCOMPARE(theme->itemData(0).toString(), QStringLiteral("Auto"));
    QCOMPARE(theme->itemData(1).toString(), QStringLiteral("Light"));
    QCOMPARE(theme->itemData(2).toString(), QStringLiteral("Dark"));

    theme->setCurrentIndex(theme->findData(QStringLiteral("Dark")));
    QCOMPARE(manager->theme(), FluentQt::Theme::Dark);
    QCOMPARE(manager->effectiveTheme(), FluentQt::Theme::Dark);
    QVERIFY(qApp->palette().color(QPalette::Window).lightness() < 128);
    QCOMPARE(QSettings().value(QStringLiteral("ui/theme")).toString(), QStringLiteral("Dark"));
    {
        fplc::MainWindow restored;
        auto *restoredTheme = restored.findChild<FluentQt::ComboBox *>(QStringLiteral("themeCombo"));
        QVERIFY(restoredTheme);
        QCOMPARE(restoredTheme->currentData().toString(), QStringLiteral("Dark"));
        QCOMPARE(manager->effectiveTheme(), FluentQt::Theme::Dark);
    }
    theme->setCurrentIndex(theme->findData(QStringLiteral("Light")));
    QCOMPARE(manager->theme(), FluentQt::Theme::Light);
    QCOMPARE(manager->effectiveTheme(), FluentQt::Theme::Light);
    QVERIFY(qApp->palette().color(QPalette::Window).lightness() >= 128);
    QCOMPARE(QSettings().value(QStringLiteral("ui/theme")).toString(), QStringLiteral("Light"));
    theme->setCurrentIndex(theme->findData(QStringLiteral("Auto")));
    QCOMPARE(manager->theme(), FluentQt::Theme::Auto);
    QVERIFY(manager->systemThemeListener()->isRunning());
    manager->systemThemeListener()->refresh();
    QCOMPARE(manager->effectiveTheme(), manager->systemThemeListener()->systemTheme());
    QCOMPARE(QSettings().value(QStringLiteral("ui/theme")).toString(), QStringLiteral("Auto"));
    QVERIFY(!window.isWindowModified());
}

void UiTest::openVariableDialogKeepsInputWhenLanguageChanges()
{
    QVERIFY(fplc::I18n::setLanguage(QStringLiteral("en_US"), false));
    fplc::MainWindow window;
    window.show();
    auto *add = window.findChild<QPushButton *>(QStringLiteral("addTagButton"));
    QVERIFY(add);
    bool dialogFound = false;
    bool translated = false;
    bool restored = false;
    bool inputPreserved = false;
    QTimer::singleShot(2000, &window, [&] {
        if (auto *dialog = window.findChild<QDialog *>(QStringLiteral("tagDialog"))) dialog->reject();
    });
    QTimer::singleShot(0, &window, [&] {
        auto *dialog = window.findChild<QDialog *>(QStringLiteral("tagDialog"));
        if (!dialog) return;
        dialogFound = true;
        auto *name = dialog->findChild<QLineEdit *>(QStringLiteral("tagNameEdit"));
        auto *initial = dialog->findChild<QLineEdit *>(QStringLiteral("tagInitialValueEdit"));
        auto *save = dialog->findChild<QPushButton *>(QStringLiteral("tagAcceptButton"));
        if (name && initial && save) {
            name->setText(QStringLiteral("User-defined 名称"));
            initial->setText(QStringLiteral("12.75"));
            fplc::I18n::setLanguage(QStringLiteral("zh_CN"), false);
            QCoreApplication::processEvents();
            translated = save->text() == QStringLiteral("保存变量");
            inputPreserved = name->text() == QStringLiteral("User-defined 名称") && initial->text() == QStringLiteral("12.75");
            fplc::I18n::setLanguage(QStringLiteral("en_US"), false);
            QCoreApplication::processEvents();
            restored = save->text() == QStringLiteral("Save variable") && name->text() == QStringLiteral("User-defined 名称")
                && initial->text() == QStringLiteral("12.75");
        }
        dialog->reject();
    });
    QTest::mouseClick(add, Qt::LeftButton);
    QVERIFY(dialogFound);
    QVERIFY(translated);
    QVERIFY(inputPreserved);
    QVERIFY(restored);
    QVERIFY(!window.isWindowModified());
    QVERIFY(fplc::I18n::setLanguage(QStringLiteral("Auto"), false));
}

void UiTest::repositoryCardIsTranslatedAndReachable()
{
    QVERIFY(fplc::I18n::setLanguage(QStringLiteral("en_US"), false));
    const QString previousVersion = QCoreApplication::applicationVersion();
    QCoreApplication::setApplicationVersion(QStringLiteral("9.9.9-ui"));
    fplc::MainWindow window;
    QCoreApplication::setApplicationVersion(previousVersion);
    auto *repository = window.findChild<QWidget *>(QStringLiteral("repositoryCard"));
    auto *title = window.findChild<QLabel *>(QStringLiteral("repositoryTitleLabel"));
    auto *name = window.findChild<QLabel *>(QStringLiteral("repositoryNameLabel"));
    auto *version = window.findChild<QLabel *>(QStringLiteral("repositoryVersionLabel"));
    auto *license = window.findChild<QLabel *>(QStringLiteral("repositoryLicenseLabel"));
    auto *issues = window.findChild<QPushButton *>(QStringLiteral("reportIssueButton"));
    auto *releases = window.findChild<QPushButton *>(QStringLiteral("releasesButton"));
    auto *related = window.findChild<QLabel *>(QStringLiteral("relatedProjectsLabel"));
    auto *link = window.findChild<QPushButton *>(QStringLiteral("repositoryLink"));
    QVERIFY(repository && title && name && version && license && issues && releases && related && link);
    QCOMPARE(title->text(), QStringLiteral("Repository"));
    QCOMPARE(name->text(), QStringLiteral("FluentPLC Simulator"));
    QCOMPARE(version->text(), QStringLiteral("9.9.9-ui"));
    QCOMPARE(license->text(), QStringLiteral("GPL-3.0"));
    auto safeRepositoryUrl = [](const QUrl &url) {
        return url.isValid() && !url.isRelative() && url.scheme() == QStringLiteral("https")
            && url.host() == QStringLiteral("github.com") && url.path().startsWith(QStringLiteral("/txp666/"))
            && url.userName().isEmpty() && url.password().isEmpty() && !url.hasQuery() && !url.hasFragment();
    };
    QVERIFY(safeRepositoryUrl(link->property("url").toUrl()));
    int actionCount = 0;
    for (auto *button : repository->findChildren<QPushButton *>()) {
        if (!button->property("externalUrl").isValid()) continue;
        QVERIFY(safeRepositoryUrl(button->property("externalUrl").toUrl()));
        ++actionCount;
    }
    QCOMPARE(actionCount, 6);

    QVERIFY(window.switchTo(QStringLiteral("settingsPage")));
    window.resize(window.minimumSize());
    window.show();
    auto *page = window.findChild<QScrollArea *>(QStringLiteral("settingsPage"));
    auto *web = window.findChild<QPushButton *>(QStringLiteral("relatedWebButton"));
    auto *language = window.findChild<FluentQt::ComboBox *>(QStringLiteral("languageCombo"));
    QVERIFY(page && web && language);
    QTRY_VERIFY(page->verticalScrollBar()->maximum() > 0);
    page->ensureWidgetVisible(web);
    QTRY_VERIFY(!web->visibleRegion().isEmpty());
    page->ensureWidgetVisible(language);
    QTRY_VERIFY(!language->visibleRegion().isEmpty());

    QVERIFY(fplc::I18n::setLanguage(QStringLiteral("zh_CN"), false));
    QTRY_COMPARE(title->text(), QStringLiteral("项目仓库"));
    QCOMPARE(issues->text(), QStringLiteral("反馈问题"));
    QCOMPARE(releases->text(), QStringLiteral("发行版本"));
    QCOMPARE(related->text(), QStringLiteral("相关项目"));
    QCOMPARE(name->text(), QStringLiteral("FluentPLC Simulator"));
    QCOMPARE(version->text(), QStringLiteral("9.9.9-ui"));
    QVERIFY(!window.isWindowModified());
    QVERIFY(!window.server()->isRunning());
    QVERIFY(fplc::I18n::setLanguage(QStringLiteral("en_US"), false));
    QTRY_COMPARE(title->text(), QStringLiteral("Repository"));
    QVERIFY(fplc::I18n::setLanguage(QStringLiteral("Auto"), false));
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
    fplc::I18n::initialize(&app);
    UiTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_ui.moc"
