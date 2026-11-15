#pragma once

#include "core/project.h"
#include <FluentQtWidgets/Window/FluentWindow.h>
#include <QtCore/QElapsedTimer>

class QCloseEvent;
class QLabel;
class QTimer;

namespace FluentQt {
class BodyLabel;
class ComboBox;
class LineEdit;
class PlainTextEdit;
class PrimaryPushButton;
class PushButton;
class SpinBox;
class TableWidget;
}

namespace fplc {

class PlcServer;

class MainWindow : public FluentQt::FluentWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    PlcServer *server() const;
    const Project &project() const;
    bool openProject(const QString &path);
    bool saveProject(const QString &path);

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    QWidget *createMonitorPage();
    QWidget *createRulesPage();
    QWidget *createLogsPage();
    QWidget *createSettingsPage();
    void createShortcuts();
    bool applyProject(const Project &project, const QString &path, bool dirty);
    bool confirmSaveChanges();
    bool saveCurrentProject();
    bool saveProjectAs();
    void chooseProject();
    void newProject();
    void loadDemo();
    void addTag();
    void editTag();
    void deleteTag();
    void rebuildTable();
    void refreshValues();
    void updateState();
    void updateTitle();
    void setDirty(bool dirty = true);
    void startServer();
    void stopServer();
    void resetValues();
    void showError(const QString &title, const QString &message);
    void showSuccess(const QString &title, const QString &message);
    void appendLog(const QString &message);

    Project m_project;
    QString m_projectPath;
    bool m_dirty = false;
    bool m_refreshing = false;
    PlcServer *m_server = nullptr;
    QTimer *m_tickTimer = nullptr;
    QTimer *m_refreshTimer = nullptr;
    QElapsedTimer m_clock;
    double m_elapsedSeconds = 0;
    QWidget *m_monitorPage = nullptr;
    FluentQt::LineEdit *m_addressEdit = nullptr;
    FluentQt::SpinBox *m_portSpin = nullptr;
    FluentQt::PrimaryPushButton *m_startButton = nullptr;
    FluentQt::PushButton *m_stopButton = nullptr;
    FluentQt::BodyLabel *m_stateLabel = nullptr;
    FluentQt::BodyLabel *m_clientLabel = nullptr;
    FluentQt::BodyLabel *m_projectLabel = nullptr;
    FluentQt::BodyLabel *m_tagCountLabel = nullptr;
    FluentQt::TableWidget *m_table = nullptr;
    FluentQt::PushButton *m_newButton = nullptr;
    FluentQt::PushButton *m_openButton = nullptr;
    FluentQt::PushButton *m_demoButton = nullptr;
    FluentQt::PushButton *m_addButton = nullptr;
    FluentQt::PushButton *m_editButton = nullptr;
    FluentQt::PushButton *m_deleteButton = nullptr;
    FluentQt::PushButton *m_resetButton = nullptr;
    FluentQt::LineEdit *m_projectNameEdit = nullptr;
    FluentQt::SpinBox *m_memorySpin = nullptr;
    FluentQt::ComboBox *m_themeCombo = nullptr;
    FluentQt::PlainTextEdit *m_logEdit = nullptr;
};

} // namespace fplc
