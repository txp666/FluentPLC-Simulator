#include "app/main_window.h"
#include "core/i18n.h"
#include "plc/plc_server.h"
#include "update/update_controller.h"
#include <FluentQtWidgets/Theme.h>
#include <FluentQtWidgets/Widgets/ComboBox.h>
#include <QtCore/QCommandLineParser>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QTextStream>
#include <QtCore/QSignalBlocker>
#include <QtCore/QTimer>
#include <QtGui/QIcon>
#include <QtWidgets/QApplication>
#include <QtWidgets/QStyleFactory>

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    Q_INIT_RESOURCE(fluentqtwidgets);
    app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    app.setOrganizationName(QStringLiteral("FluentPLC"));
    app.setApplicationName(QStringLiteral("FluentPLC Simulator"));
    app.setApplicationVersion(QStringLiteral(FPLC_VERSION));
    app.setWindowIcon(QIcon(QStringLiteral(":/fplc/icon.svg")));
    FluentQt::ThemeManager::instance()->setAccentColor(QColor(QStringLiteral("#009faa")));
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("FluentPLC Simulator — S7 communication test bench"));
    parser.addHelpOption(); parser.addVersionOption();
    parser.addPositionalArgument(QStringLiteral("project"), QStringLiteral("A .plcsim.json project"));
    parser.addOption({QStringLiteral("screenshot"), QStringLiteral("Save a window screenshot and exit"), QStringLiteral("file")});
    parser.addOption({QStringLiteral("start-server"), QStringLiteral("Start the project's local S7 server")});
    parser.addOption({QStringLiteral("language"), QStringLiteral("Interface language: Auto, en_US or zh_CN"), QStringLiteral("locale")});
    parser.addOption({QStringLiteral("theme"), QStringLiteral("Override this session's theme: Auto, Light or Dark"), QStringLiteral("theme")});
    parser.addOption({QStringLiteral("page"), QStringLiteral("Initial page: monitor, rules, logs or settings"), QStringLiteral("page")});
    parser.addOption({QStringLiteral("update"), QStringLiteral("Check, download and install the latest release without opening a window")});
    parser.process(app);
    if (parser.isSet(QStringLiteral("language"))) {
        const QString choice = parser.value(QStringLiteral("language"));
        if (choice != QStringLiteral("Auto") && choice != QStringLiteral("en_US") && choice != QStringLiteral("zh_CN")) {
            QTextStream(stderr) << "Language must be Auto, en_US or zh_CN\n";
            return 2;
        }
        fplc::I18n::initialize(&app, choice);
    } else {
        fplc::I18n::initialize(&app);
    }
    if (parser.isSet(QStringLiteral("update"))) return fplc::UpdateController::runHeadless();
    fplc::MainWindow window;
    if (parser.isSet(QStringLiteral("theme"))) {
        const QString theme = parser.value(QStringLiteral("theme"));
        if (theme != QStringLiteral("Auto") && theme != QStringLiteral("Light") && theme != QStringLiteral("Dark")) {
            QTextStream(stderr) << "Theme must be Auto, Light or Dark\n";
            return 2;
        }
        FluentQt::ThemeManager::instance()->setTheme(theme == QStringLiteral("Dark") ? FluentQt::Theme::Dark
            : theme == QStringLiteral("Light") ? FluentQt::Theme::Light : FluentQt::Theme::Auto);
        if (auto *combo = window.findChild<FluentQt::ComboBox *>(QStringLiteral("themeCombo"))) {
            const QSignalBlocker blocker(combo);
            combo->setCurrentIndex(combo->findData(theme));
        }
    }
    if (parser.isSet(QStringLiteral("page"))) {
        const QString page = parser.value(QStringLiteral("page"));
        if (!QStringList{QStringLiteral("monitor"), QStringLiteral("rules"), QStringLiteral("logs"), QStringLiteral("settings")}.contains(page)) return 2;
        window.switchTo(page + QStringLiteral("Page"));
    }
    if (!parser.positionalArguments().isEmpty() && !window.openProject(parser.positionalArguments().first())) return 1;
    window.show();
    if (parser.isSet(QStringLiteral("start-server"))) {
        QString error;
        if (!window.server()->start(&error)) { QTextStream(stderr) << error << '\n'; return 1; }
    }
    if (parser.isSet(QStringLiteral("screenshot"))) {
        const QString file = parser.value(QStringLiteral("screenshot"));
        QTimer::singleShot(800, &window, [&] {
            QDir().mkpath(QFileInfo(file).absolutePath());
            app.exit(window.grab().save(file) ? 0 : 1);
        });
    }
    return app.exec();
}
