#include "plc/plc_server.h"
#include <QtCore/QCommandLineParser>
#include <QtCore/QCoreApplication>
#include <QtCore/QElapsedTimer>
#include <QtCore/QTextStream>
#include <QtCore/QTimer>
#include <cmath>
#include <csignal>

namespace { volatile std::sig_atomic_t interrupted = 0; void handleSignal(int) { interrupted = 1; } }

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("FluentPLCServer"));
    app.setApplicationVersion(QStringLiteral("0.1.0"));
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Headless S7 PLC communication simulator"));
    parser.addHelpOption(); parser.addVersionOption();
    parser.addOption({QStringLiteral("project"), QStringLiteral("Load a .plcsim.json project"), QStringLiteral("file")});
    parser.addOption({QStringLiteral("listen"), QStringLiteral("Bind to this IPv4 address"), QStringLiteral("address")});
    parser.addOption({QStringLiteral("port"), QStringLiteral("Listen TCP port (1..65535; default 1102)"), QStringLiteral("port")});
    parser.addOption({QStringLiteral("duration"), QStringLiteral("Exit after this many seconds"), QStringLiteral("seconds")});
    parser.addOption({QStringLiteral("check"), QStringLiteral("Validate project without listening")});
    parser.addOption({QStringLiteral("write-demo"), QStringLiteral("Save the built-in example and exit"), QStringLiteral("file")});
    parser.process(app);
    fplc::Project project = fplc::Project::demo();
    QString error;
    auto fail = [&](const QString &message) { QTextStream(stderr) << message << '\n'; return 2; };
    if (parser.isSet(QStringLiteral("project")) && !fplc::Project::load(parser.value(QStringLiteral("project")), &project, &error)) return fail(error);
    if (parser.isSet(QStringLiteral("listen"))) project.bindAddress = parser.value(QStringLiteral("listen"));
    if (parser.isSet(QStringLiteral("port"))) {
        bool ok = false; const uint port = parser.value(QStringLiteral("port")).toUInt(&ok);
        if (!ok || port < 1 || port > 65535) return fail(QStringLiteral("Port must be between 1 and 65535"));
        project.port = quint16(port);
    }
    if (!project.validate(&error)) return fail(error);
    if (parser.isSet(QStringLiteral("write-demo"))) return project.save(parser.value(QStringLiteral("write-demo")), &error) ? 0 : fail(error);
    if (parser.isSet(QStringLiteral("check"))) { QTextStream(stdout) << "Project valid\n"; return 0; }
    double seconds = 0;
    if (parser.isSet(QStringLiteral("duration"))) {
        bool ok = false; seconds = parser.value(QStringLiteral("duration")).toDouble(&ok);
        if (!ok || !std::isfinite(seconds) || seconds <= 0 || seconds > 86400) return fail(QStringLiteral("Duration must be in (0, 86400] seconds"));
    }
    fplc::PlcServer server;
    QObject::connect(&server, &fplc::PlcServer::logMessage, &app, [](const QString &message) { QTextStream(stdout) << message << Qt::endl; });
    if (!server.setProject(project, &error) || !server.start(&error)) return fail(error);
    QTextStream(stdout) << "S7 simulator listening on " << project.bindAddress << ':' << project.port << Qt::endl;
    std::signal(SIGINT, handleSignal); std::signal(SIGTERM, handleSignal);
    QElapsedTimer elapsed; elapsed.start();
    QTimer timer;
    QObject::connect(&timer, &QTimer::timeout, &app, [&] {
        if (interrupted) { app.quit(); return; }
        server.tick(elapsed.elapsed() / 1000.0);
    });
    QObject::connect(&app, &QCoreApplication::aboutToQuit, &server, &fplc::PlcServer::stop);
    timer.start(50);
    if (seconds > 0) QTimer::singleShot(int(seconds * 1000), &app, &QCoreApplication::quit);
    return app.exec();
}
