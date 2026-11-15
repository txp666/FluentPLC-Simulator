#pragma once

#include "core/project.h"
#include <QtCore/QObject>
#include <memory>

namespace fplc {

// Owns real Snap7 server memory. All public methods run on its owning Qt thread;
// Snap7 manages network threads, synchronized with its registered-area locks.
class PlcServer : public QObject {
    Q_OBJECT
public:
    explicit PlcServer(QObject *parent = nullptr);
    ~PlcServer() override;
    bool setProject(const Project &project, QString *error = nullptr);
    const Project &project() const;
    bool start(QString *error = nullptr);
    void stop();
    bool isRunning() const;
    int clientCount() const;
    QVariant value(int tagIndex) const;
    bool setValue(int tagIndex, const QVariant &value, QString *error = nullptr);
    // Advance running generators using cumulative seconds since start. The UI /
    // headless runner owns the clock. Manual tags retain client writes.
    void tick(double elapsedSeconds);

signals:
    void valuesChanged();
    void stateChanged(bool running);
    void clientsChanged(int count);
    void logMessage(const QString &message);

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace fplc
