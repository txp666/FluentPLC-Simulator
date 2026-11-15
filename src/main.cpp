#include "app/main_window.h"
#include "plc/plc_server.h"
#include <FluentQtWidgets/Theme.h>
#include <QtCore/QCommandLineParser>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QTextStream>
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
    parser.setApplicationDescription(QStringLiteral("FluentPLC Simulator — S7 通信模拟测试台"));
    parser.addHelpOption(); parser.addVersionOption();
    parser.addPositionalArgument(QStringLiteral("project"), QStringLiteral("A .plcsim.json project"));
    parser.addOption({QStringLiteral("screenshot"), QStringLiteral("Save a window screenshot and exit"), QStringLiteral("file")});
    parser.addOption({QStringLiteral("start-server"), QStringLiteral("Start the project's local S7 server")});
    parser.process(app);
    fplc::MainWindow window;
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
