#include "app/main_window.h"
#include "core/i18n.h"
#include "plc/plc_server.h"
#include "update/update_controller.h"

#include <FluentQtWidgets/Dialogs/Dialog.h>
#include <FluentQtWidgets/FluentIcon.h>
#include <FluentQtWidgets/Layout/FlowLayout.h>
#include <FluentQtWidgets/Navigation/NavigationInterface.h>
#include <FluentQtWidgets/Navigation/NavigationTreeWidget.h>
#include <FluentQtWidgets/Navigation/NavigationWidget.h>
#include <FluentQtWidgets/Theme.h>
#include <FluentQtWidgets/Views/ItemViews.h>
#include <FluentQtWidgets/Widgets/Button.h>
#include <FluentQtWidgets/Widgets/CardWidget.h>
#include <FluentQtWidgets/Widgets/ComboBox.h>
#include <FluentQtWidgets/Widgets/InfoBar.h>
#include <FluentQtWidgets/Widgets/Label.h>
#include <FluentQtWidgets/Widgets/LineEdit.h>
#include <FluentQtWidgets/Widgets/ScrollArea.h>
#include <FluentQtWidgets/Widgets/SpinBox.h>
#include <FluentQtWidgets/Widgets/SwitchButton.h>

#include <QtCore/QDateTime>
#include <QtCore/QEvent>
#include <QtCore/QFileInfo>
#include <QtCore/QSettings>
#include <QtCore/QSignalBlocker>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtGui/QAction>
#include <QtGui/QCloseEvent>
#include <QtGui/QDesktopServices>
#include <QtGui/QFontDatabase>
#include <QtGui/QKeySequence>
#include <QtGui/QTextDocument>
#include <QtWidgets/QApplication>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QVBoxLayout>

#include <cmath>
#include <functional>

namespace fplc {
namespace {

using namespace FluentQt;

QString translateUi(const char *source, const QStringList &arguments = {})
{
    QString text = QCoreApplication::translate("SimulatorUi", source);
    for (const QString &argument : arguments) text = text.arg(argument);
    return text;
}

void bindTranslation(QObject *object, const char *property, const char *source, const QStringList &arguments = {})
{
    const QByteArray sourceKey = QByteArrayLiteral("_fplc_source_") + property;
    const QByteArray argumentsKey = QByteArrayLiteral("_fplc_arguments_") + property;
    object->setProperty(sourceKey.constData(), QByteArray(source));
    object->setProperty(argumentsKey.constData(), arguments);
    object->setProperty(property, translateUi(source, arguments));
}

void retranslateBindings(QWidget *root)
{
    QList<QWidget *> widgets = root->findChildren<QWidget *>();
    widgets.prepend(root);
    for (QWidget *widget : widgets) {
        for (const char *property : {"text", "toolTip", "windowTitle", "placeholderText", "suffix"}) {
            const QByteArray sourceKey = QByteArrayLiteral("_fplc_source_") + property;
            const QVariant source = widget->property(sourceKey.constData());
            if (!source.isValid()) continue;
            const QByteArray argumentsKey = QByteArrayLiteral("_fplc_arguments_") + property;
            widget->setProperty(property, translateUi(source.toByteArray().constData(), widget->property(argumentsKey.constData()).toStringList()));
        }
        auto *combo = qobject_cast<ComboBox *>(widget);
        if (!combo) continue;
        const QStringList sources = combo->property("_fplc_item_sources").toStringList();
        const QSignalBlocker blocker(combo);
        for (int index = 0; index < sources.size(); ++index) combo->setItemText(index, translateUi(sources.at(index).toUtf8().constData()));
    }
}

template<class Label>
Label *translatedLabel(const char *source, QWidget *parent = nullptr)
{
    auto *label = new Label(parent);
    bindTranslation(label, "text", source);
    return label;
}

template<class Button>
Button *translatedButton(const QIcon &icon, const char *source, QWidget *parent)
{
    auto *button = new Button(icon, translateUi(source), parent);
    bindTranslation(button, "text", source);
    return button;
}

void translatedFormRow(QFormLayout *form, const char *source, QWidget *field)
{
    form->addRow(translatedLabel<BodyLabel>(source), field);
}

void translatedComboItem(ComboBox *combo, const char *source, const QVariant &data)
{
    combo->addItem(translateUi(source), {}, data);
    QStringList sources = combo->property("_fplc_item_sources").toStringList();
    sources.append(QString::fromUtf8(source));
    combo->setProperty("_fplc_item_sources", sources);
}

QString modeLabel(const QString &mode)
{
    if (mode == QStringLiteral("sine")) return QObject::tr("Sine wave");
    if (mode == QStringLiteral("ramp")) return QObject::tr("Ramp");
    if (mode == QStringLiteral("toggle")) return QObject::tr("Toggle");
    if (mode == QStringLiteral("follow")) return QObject::tr("Follow");
    return QObject::tr("Manual");
}

QString valueText(const QVariant &value, const QString &type)
{
    if (!value.isValid()) return QStringLiteral("—");
    if (type == QStringLiteral("BOOL")) return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    if (type == QStringLiteral("REAL")) return QString::number(value.toDouble(), 'g', 9);
    return value.toString();
}

bool parseValue(const QString &type, const QString &text, QVariant *value, QString *error)
{
    const QString clean = text.trimmed();
    bool ok = false;
    if (type == QStringLiteral("BOOL")) {
        if (clean.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0 || clean == QStringLiteral("1")) {
            *value = true;
            return true;
        }
        if (clean.compare(QStringLiteral("false"), Qt::CaseInsensitive) == 0 || clean == QStringLiteral("0")) {
            *value = false;
            return true;
        }
        *error = QObject::tr("Enter true, false, 1 or 0 for BOOL.");
        return false;
    }
    if (type == QStringLiteral("REAL")) {
        const double number = clean.toDouble(&ok);
        if (ok && std::isfinite(number)) {
            *value = number;
            return true;
        }
        *error = QObject::tr("Enter a finite REAL value, for example 23.5.");
        return false;
    }
    const bool hex = clean.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive)
        || clean.startsWith(QStringLiteral("-0x"), Qt::CaseInsensitive)
        || clean.startsWith(QStringLiteral("+0x"), Qt::CaseInsensitive);
    const qlonglong number = clean.toLongLong(&ok, hex ? 16 : 10);
    if (ok) {
        *value = number;
        return true;
    }
    *error = QObject::tr("Enter an integer, or a hexadecimal value starting with 0x.");
    return false;
}

BodyLabel *description(const QString &text, QWidget *parent)
{
    auto *label = new BodyLabel(text, parent);
    label->setWordWrap(true);
    label->setTextColor(QColor(QStringLiteral("#5b6873")), QColor(QStringLiteral("#adbcc4")));
    label->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    return label;
}

BodyLabel *description(const char *source, QWidget *parent)
{
    auto *label = description(translateUi(source), parent);
    bindTranslation(label, "text", source);
    return label;
}

QWidget *scrollPage(const QString &name, QVBoxLayout **layout)
{
    auto *scroll = new ScrollArea;
    scroll->setObjectName(name);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *body = new QWidget(scroll);
    body->setObjectName(name + QStringLiteral("Content"));
    body->setAutoFillBackground(false);
    *layout = new QVBoxLayout(body);
    (*layout)->setContentsMargins(28, 22, 28, 28);
    (*layout)->setSpacing(16);
    scroll->setWidget(body);
    return scroll;
}

QVBoxLayout *cardLayout(CardWidget *card)
{
    card->setBorderRadius(10);
    auto *layout = new QVBoxLayout(card);
    layout->setContentsMargins(20, 18, 20, 18);
    layout->setSpacing(12);
    return layout;
}

void pageHeading(QVBoxLayout *layout, const char *title, const char *subtitle)
{
    auto *titleLabel = translatedLabel<TitleLabel>(title);
    layout->addWidget(titleLabel);
    layout->addWidget(description(subtitle, titleLabel->parentWidget()));
}

class TagDialog final : public Dialog {
public:
    TagDialog(const Project &project, int index, QWidget *parent)
        : Dialog(index < 0 ? tr("Add variable") : tr("Edit variable"),
                 tr("Set the S7 address, initial value and rule. Saving reloads the project initial values."), parent),
          m_project(project), m_index(index)
    {
        setObjectName(QStringLiteral("tagDialog"));
        const char *titleSource = index < 0 ? QT_TR_NOOP("Add variable") : QT_TR_NOOP("Edit variable");
        bindTranslation(this, "windowTitle", titleSource);
        bindTranslation(titleLabel(), "text", titleSource);
        bindTranslation(windowTitleLabel(), "text", titleSource);
        bindTranslation(contentLabel(), "text", QT_TR_NOOP("Set the S7 address, initial value and rule. Saving reloads the project initial values."));
        setMinimumWidth(560);
        Tag tag;
        if (index >= 0) {
            tag = project.tags.at(index);
        } else {
            tag.name = tr("Tag %1").arg(project.tags.size() + 1);
            tag.address = QStringLiteral("DB1.DBD%1").arg(project.tags.size() * 4);
            if (!project.tags.isEmpty()) tag.source = project.tags.first().address;
        }
        auto *form = new QFormLayout;
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        form->setHorizontalSpacing(20);
        form->setVerticalSpacing(11);
        m_name = new LineEdit(tag.name, view());
        m_name->setObjectName(QStringLiteral("tagNameEdit"));
        m_address = new LineEdit(tag.address, view());
        m_address->setObjectName(QStringLiteral("tagAddressEdit"));
        m_address->setPlaceholderText(QStringLiteral("DB1.DBD0 / DB1.DBX8.0 / MW0"));
        m_type = new ComboBox(view());
        m_type->setObjectName(QStringLiteral("tagTypeCombo"));
        m_type->addItems({QStringLiteral("BOOL"), QStringLiteral("BYTE"), QStringLiteral("SINT"), QStringLiteral("WORD"),
                          QStringLiteral("DWORD"), QStringLiteral("INT"), QStringLiteral("DINT"), QStringLiteral("REAL")});
        m_type->setCurrentText(tag.dataType);
        m_initial = new LineEdit(valueText(tag.initialValue, tag.dataType), view());
        m_initial->setObjectName(QStringLiteral("tagInitialValueEdit"));
        m_mode = new ComboBox(view());
        m_mode->setObjectName(QStringLiteral("tagModeCombo"));
        for (const QString &mode : {QStringLiteral("manual"), QStringLiteral("sine"), QStringLiteral("ramp"),
                                    QStringLiteral("toggle"), QStringLiteral("follow")}) {
            m_mode->addItem(QStringLiteral("%1 · %2").arg(modeLabel(mode), mode), {}, mode);
        }
        m_mode->setCurrentIndex(m_mode->findData(tag.mode));
        auto numberSpin = [this](const QString &name, double value) {
            auto *spin = new DoubleSpinBox(view());
            spin->setObjectName(name);
            spin->setRange(-1e38, 1e38);
            spin->setDecimals(9);
            spin->setValue(value);
            spin->setMinimumWidth(180);
            return spin;
        };
        m_minimum = numberSpin(QStringLiteral("tagMinimumSpin"), tag.minimum);
        m_maximum = numberSpin(QStringLiteral("tagMaximumSpin"), tag.maximum);
        m_period = numberSpin(QStringLiteral("tagPeriodSpin"), tag.periodSeconds);
        m_period->setRange(0.001, 1e9);
        bindTranslation(m_period, "suffix", QT_TR_NOOP(" s"));
        m_source = new LineEdit(tag.source, view());
        m_source->setObjectName(QStringLiteral("tagSourceEdit"));
        bindTranslation(m_source, "placeholderText", QT_TR_NOOP("Source address, for example DB1.DBX8.0"));
        translatedFormRow(form, QT_TR_NOOP("Variable name"), m_name);
        translatedFormRow(form, QT_TR_NOOP("S7 address"), m_address);
        translatedFormRow(form, QT_TR_NOOP("Data type"), m_type);
        translatedFormRow(form, QT_TR_NOOP("Initial value"), m_initial);
        translatedFormRow(form, QT_TR_NOOP("Rule"), m_mode);
        translatedFormRow(form, QT_TR_NOOP("Minimum"), m_minimum);
        translatedFormRow(form, QT_TR_NOOP("Maximum"), m_maximum);
        translatedFormRow(form, QT_TR_NOOP("Period"), m_period);
        translatedFormRow(form, QT_TR_NOOP("Follow address"), m_source);
        viewLayout()->addLayout(form);
        m_ruleHint = description(QString(), view());
        m_ruleHint->setMinimumHeight(38);
        viewLayout()->addWidget(m_ruleHint);
        m_error = new BodyLabel(view());
        m_error->setObjectName(QStringLiteral("tagErrorLabel"));
        m_error->setWordWrap(true);
        m_error->setTextColor(QColor(QStringLiteral("#b42318")), QColor(QStringLiteral("#ffb4ab")));
        viewLayout()->addWidget(m_error);
        bindTranslation(yesButton(), "text", QT_TR_NOOP("Save variable"));
        yesButton()->setObjectName(QStringLiteral("tagAcceptButton"));
        bindTranslation(cancelButton(), "text", QT_TR_NOOP("Cancel"));
        // Dialog's stylesheet addresses #cancelButton; keep its framework name.
        connect(m_mode, &ComboBox::currentIndexChanged, this, [this] { updateRuleFields(); });
        updateRuleFields();
    }

    Tag tag() const
    {
        Tag tag;
        tag.name = m_name->text().trimmed();
        tag.address = m_address->text().trimmed().toUpper();
        tag.dataType = m_type->currentText();
        QString ignored;
        parseValue(tag.dataType, m_initial->text(), &tag.initialValue, &ignored);
        tag.mode = m_mode->currentData().toString();
        tag.minimum = m_minimum->value();
        tag.maximum = m_maximum->value();
        tag.periodSeconds = m_period->value();
        tag.source = m_source->text().trimmed().toUpper();
        return tag;
    }

    Project updatedProject() const
    {
        Project candidate = m_project;
        const Tag updated = tag();
        if (m_index < 0) {
            candidate.tags.append(updated);
        } else {
            const QString oldAddress = candidate.tags.at(m_index).address.trimmed().toUpper();
            candidate.tags[m_index] = updated;
            if (oldAddress != updated.address) {
                for (Tag &dependent : candidate.tags) {
                    if (dependent.mode == QStringLiteral("follow")
                        && dependent.source.trimmed().toUpper() == oldAddress) dependent.source = updated.address;
                }
            }
        }
        return candidate;
    }

protected:
    void changeEvent(QEvent *event) override
    {
        Dialog::changeEvent(event);
        if (event->type() != QEvent::LanguageChange || !m_mode) return;
        retranslateBindings(this);
        const QSignalBlocker blocker(m_mode);
        for (int index = 0; index < m_mode->count(); ++index) {
            const QString mode = m_mode->itemData(index).toString();
            m_mode->setItemText(index, QStringLiteral("%1 · %2").arg(modeLabel(mode), mode));
        }
        updateRuleFields();
        if (!m_error->text().isEmpty()) validate();
    }

    bool validate() const override
    {
        QVariant parsed;
        QString error;
        if (!parseValue(m_type->currentText(), m_initial->text(), &parsed, &error)) {
            m_error->setText(error);
            m_initial->setFocus();
            return false;
        }
        const Project candidate = updatedProject();
        if (!candidate.validate(&error)) {
            m_error->setText(error);
            return false;
        }
        m_error->clear();
        return true;
    }

private:
    void updateRuleFields()
    {
        const QString mode = m_mode->currentData().toString();
        const bool range = mode == QStringLiteral("sine") || mode == QStringLiteral("ramp");
        m_minimum->setEnabled(range);
        m_maximum->setEnabled(range);
        m_period->setEnabled(range || mode == QStringLiteral("toggle"));
        m_source->setEnabled(mode == QStringLiteral("follow"));
        if (mode == QStringLiteral("manual")) bindTranslation(m_ruleHint, "text", QT_TR_NOOP("Manual: keep the value written by a client or the table until another write or reset."));
        else if (mode == QStringLiteral("sine")) bindTranslation(m_ruleHint, "text", QT_TR_NOOP("Sine wave: move smoothly between the minimum and maximum over one complete period."));
        else if (mode == QStringLiteral("ramp")) bindTranslation(m_ruleHint, "text", QT_TR_NOOP("Ramp: increase linearly from minimum to maximum, then start again each period."));
        else if (mode == QStringLiteral("toggle")) bindTranslation(m_ruleHint, "text", QT_TR_NOOP("Toggle: BOOL only; invert the initial value every half period."));
        else bindTranslation(m_ruleHint, "text", QT_TR_NOOP("Follow: copy the current value of the source address on each update."));
    }

    Project m_project;
    int m_index;
    LineEdit *m_name = nullptr;
    LineEdit *m_address = nullptr;
    ComboBox *m_type = nullptr;
    LineEdit *m_initial = nullptr;
    ComboBox *m_mode = nullptr;
    DoubleSpinBox *m_minimum = nullptr;
    DoubleSpinBox *m_maximum = nullptr;
    DoubleSpinBox *m_period = nullptr;
    LineEdit *m_source = nullptr;
    BodyLabel *m_ruleHint = nullptr;
    BodyLabel *m_error = nullptr;
};

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : FluentWindow(parent), m_project(Project::demo()), m_server(new PlcServer(this))
{
    setObjectName(QStringLiteral("mainWindow"));
    setWindowIcon(QIcon(QStringLiteral(":/fplc/icon.svg")));
    resize(1280, 800);
    setMinimumSize(940, 680);
    auto *themes = ThemeManager::instance();
    themes->setAccentColor(QColor(QStringLiteral("#099a91")));
    const QString theme = QSettings().value(QStringLiteral("ui/theme"), QStringLiteral("Auto")).toString();
    themes->setTheme(theme == QStringLiteral("Dark") ? Theme::Dark : theme == QStringLiteral("Light") ? Theme::Light : Theme::Auto);
    themes->applyTo(qApp);
    m_updates = new UpdateController(this);

    m_monitorPage = createMonitorPage();
    addSubInterface(m_monitorPage, icon(FluentIcon::View), tr("Variable monitor"), QStringLiteral("monitorPage"));
    addSubInterface(createRulesPage(), icon(FluentIcon::Code), tr("Rules & guide"), QStringLiteral("rulesPage"));
    addSubInterface(createLogsPage(), icon(FluentIcon::History), tr("Server log"), QStringLiteral("logsPage"));
    addSubInterface(createSettingsPage(), icon(FluentIcon::Setting), tr("Settings"), QStringLiteral("settingsPage"), NavigationItemPosition::Bottom);
    createShortcuts();
    connect(m_updates, &UpdateController::checkingChanged, this, [this](bool checking) {
        m_checkUpdateButton->setEnabled(!checking);
        bindTranslation(m_checkUpdateButton, "text", checking ? QT_TR_NOOP("Checking…") : QT_TR_NOOP("Check for updates"));
    });
    connect(m_updates, &UpdateController::restartRequested, this, [this] {
        m_restartForUpdate = true;
        close();
    });
    QTimer::singleShot(1500, m_updates, &UpdateController::startupCheck);

    connect(m_server, &PlcServer::logMessage, this, &MainWindow::appendLog);
    connect(m_server, &PlcServer::stateChanged, this, [this](bool running) {
        if (running) {
            m_elapsedSeconds = 0;
            m_clock.start();
        } else {
            m_clock.invalidate();
        }
        updateState();
        refreshValues();
    });
    connect(m_server, &PlcServer::clientsChanged, this, [this] { updateState(); });
    connect(m_server, &PlcServer::valuesChanged, this, [this] {
        // Network and generator updates are coalesced by the 300 ms view timer.
        if (!m_server->isRunning()) refreshValues();
    });
    m_tickTimer = new QTimer(this);
    m_tickTimer->setObjectName(QStringLiteral("simulationTimer"));
    m_tickTimer->setInterval(100);
    m_tickTimer->setTimerType(Qt::PreciseTimer);
    connect(m_tickTimer, &QTimer::timeout, this, [this] {
        if (!m_server->isRunning() || !m_clock.isValid()) return;
        m_elapsedSeconds += static_cast<double>(m_clock.restart()) / 1000.0;
        m_server->tick(m_elapsedSeconds);
    });
    m_tickTimer->start();
    m_refreshTimer = new QTimer(this);
    m_refreshTimer->setObjectName(QStringLiteral("valueRefreshTimer"));
    m_refreshTimer->setInterval(300);
    connect(m_refreshTimer, &QTimer::timeout, this, &MainWindow::refreshValues);
    m_refreshTimer->start();
    QString error;
    if (!m_server->setProject(m_project, &error)) showError(tr("Could not load demo"), error);
    rebuildTable();
    retranslateUi();
    switchTo(m_monitorPage);
    appendLog(tr("The built-in demo is loaded. The server is stopped until you select Start server."));
}

MainWindow::~MainWindow()
{
    m_server->stop();
}

PlcServer *MainWindow::server() const { return m_server; }
const Project &MainWindow::project() const { return m_project; }

QWidget *MainWindow::createMonitorPage()
{
    QVBoxLayout *layout = nullptr;
    QWidget *page = scrollPage(QStringLiteral("monitorPage"), &layout);
    pageHeading(layout, QT_TR_NOOP("Variable monitor"), QT_TR_NOOP("Connect your HMI to a readable and writable simulated PLC to test displays, controls and charts."));

    auto *projectTools = new QHBoxLayout;
    projectTools->setSpacing(8);
    auto projectButton = [page, projectTools](const char *text, const QString &name, FluentIcon symbol) {
        auto *button = translatedButton<PushButton>(icon(symbol), text, page);
        button->setObjectName(name);
        projectTools->addWidget(button);
        return button;
    };
    m_newButton = projectButton(QT_TR_NOOP("New"), QStringLiteral("newProjectButton"), FluentIcon::Add);
    m_openButton = projectButton(QT_TR_NOOP("Open"), QStringLiteral("openProjectButton"), FluentIcon::Folder);
    auto *save = projectButton(QT_TR_NOOP("Save"), QStringLiteral("saveProjectButton"), FluentIcon::Save);
    auto *saveAs = projectButton(QT_TR_NOOP("Save as"), QStringLiteral("saveAsProjectButton"), FluentIcon::Document);
    projectTools->addStretch();
    m_demoButton = projectButton(QT_TR_NOOP("Load demo"), QStringLiteral("demoProjectButton"), FluentIcon::Play);
    layout->addLayout(projectTools);
    connect(m_newButton, &PushButton::clicked, this, &MainWindow::newProject);
    connect(m_openButton, &PushButton::clicked, this, &MainWindow::chooseProject);
    connect(save, &PushButton::clicked, this, [this] { saveCurrentProject(); });
    connect(saveAs, &PushButton::clicked, this, [this] { saveProjectAs(); });
    connect(m_demoButton, &PushButton::clicked, this, &MainWindow::loadDemo);
    m_projectLabel = description(QString(), page);
    m_projectLabel->setObjectName(QStringLiteral("projectPathLabel"));
    m_projectLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_projectLabel);

    auto *serviceCard = new CardWidget(page);
    serviceCard->setObjectName(QStringLiteral("serverCard"));
    auto *service = cardLayout(serviceCard);
    auto *serviceHead = new QHBoxLayout;
    serviceHead->addWidget(translatedLabel<StrongBodyLabel>(QT_TR_NOOP("S7 server"), serviceCard));
    serviceHead->addStretch();
    m_stateLabel = new BodyLabel(serviceCard);
    m_stateLabel->setObjectName(QStringLiteral("serverStateLabel"));
    serviceHead->addWidget(m_stateLabel);
    service->addLayout(serviceHead);
    auto *connection = new QHBoxLayout;
    connection->setSpacing(12);
    auto *addressColumn = new QVBoxLayout;
    addressColumn->setSpacing(6);
    addressColumn->addWidget(translatedLabel<CaptionLabel>(QT_TR_NOOP("Listen address"), serviceCard));
    m_addressEdit = new LineEdit(m_project.bindAddress, serviceCard);
    m_addressEdit->setObjectName(QStringLiteral("serverAddressEdit"));
    m_addressEdit->setPlaceholderText(QStringLiteral("127.0.0.1"));
    addressColumn->addWidget(m_addressEdit);
    connection->addLayout(addressColumn, 1);
    auto *portColumn = new QVBoxLayout;
    portColumn->setSpacing(6);
    portColumn->addWidget(translatedLabel<CaptionLabel>(QT_TR_NOOP("TCP port"), serviceCard));
    m_portSpin = new SpinBox(serviceCard);
    m_portSpin->setObjectName(QStringLiteral("serverPortSpin"));
    m_portSpin->setRange(1, 65535);
    m_portSpin->setValue(m_project.port);
    m_portSpin->setFixedWidth(130);
    portColumn->addWidget(m_portSpin);
    connection->addLayout(portColumn);
    m_startButton = translatedButton<PrimaryPushButton>(icon(FluentIcon::Play), QT_TR_NOOP("Start server"), serviceCard);
    m_startButton->setObjectName(QStringLiteral("startServerButton"));
    connection->addWidget(m_startButton, 0, Qt::AlignBottom);
    m_stopButton = translatedButton<PushButton>(icon(FluentIcon::Pause), QT_TR_NOOP("Stop"), serviceCard);
    m_stopButton->setObjectName(QStringLiteral("stopServerButton"));
    connection->addWidget(m_stopButton, 0, Qt::AlignBottom);
    service->addLayout(connection);
    auto *connectionFoot = new QHBoxLayout;
    connectionFoot->addWidget(description(QT_TR_NOOP("Local connections by default · S7comm · No physical PLC required"), serviceCard), 1);
    m_clientLabel = new BodyLabel(serviceCard);
    m_clientLabel->setObjectName(QStringLiteral("clientCountLabel"));
    connectionFoot->addWidget(m_clientLabel);
    service->addLayout(connectionFoot);
    layout->addWidget(serviceCard);
    connect(m_startButton, &PushButton::clicked, this, &MainWindow::startServer);
    connect(m_stopButton, &PushButton::clicked, this, &MainWindow::stopServer);
    connect(m_addressEdit, &QLineEdit::textChanged, this, [this](const QString &text) {
        m_project.bindAddress = text.trimmed();
        setDirty();
    });
    connect(m_portSpin, &QSpinBox::valueChanged, this, [this](int port) {
        m_project.port = static_cast<quint16>(port);
        setDirty();
    });

    auto *tagsCard = new CardWidget(page);
    tagsCard->setObjectName(QStringLiteral("tagsCard"));
    auto *tags = cardLayout(tagsCard);
    auto *tagTools = new QHBoxLayout;
    tagTools->setSpacing(8);
    tagTools->addWidget(translatedLabel<StrongBodyLabel>(QT_TR_NOOP("Variables"), tagsCard));
    m_tagCountLabel = description(QString(), tagsCard);
    m_tagCountLabel->setObjectName(QStringLiteral("tagCountLabel"));
    tagTools->addWidget(m_tagCountLabel);
    tagTools->addStretch();
    auto tagButton = [tagsCard, tagTools](const char *text, const QString &name, FluentIcon symbol) {
        auto *button = translatedButton<PushButton>(icon(symbol), text, tagsCard);
        button->setObjectName(name);
        tagTools->addWidget(button);
        return button;
    };
    m_addButton = tagButton(QT_TR_NOOP("Add"), QStringLiteral("addTagButton"), FluentIcon::Add);
    m_editButton = tagButton(QT_TR_NOOP("Edit"), QStringLiteral("editTagButton"), FluentIcon::Edit);
    m_deleteButton = tagButton(QT_TR_NOOP("Delete"), QStringLiteral("deleteTagButton"), FluentIcon::Delete);
    m_resetButton = tagButton(QT_TR_NOOP("Reset values"), QStringLiteral("resetValuesButton"), FluentIcon::History);
    tags->addLayout(tagTools);
    tags->addWidget(description(QT_TR_NOOP("Double-click a current value to write it temporarily. Use Edit for initial values and rules; stop the server to change the project structure."), tagsCard));
    m_table = new TableWidget(tagsCard);
    m_table->setObjectName(QStringLiteral("tagTable"));
    m_table->setColumnCount(5);
    m_table->setHorizontalHeaderLabels({tr("Variable name"), tr("S7 address"), tr("Type"), tr("Current value"), tr("Rule")});
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    m_table->setAlternatingRowColors(false);
    m_table->setBorderVisible(false);
    m_table->setShowGrid(false);
    m_table->setMinimumHeight(230);
    m_table->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_table->verticalHeader()->hide();
    m_table->verticalHeader()->setDefaultSectionSize(42);
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setMinimumSectionSize(65);
    tags->addWidget(m_table, 1);
    layout->addWidget(tagsCard, 1);
    connect(m_addButton, &PushButton::clicked, this, &MainWindow::addTag);
    connect(m_editButton, &PushButton::clicked, this, &MainWindow::editTag);
    connect(m_deleteButton, &PushButton::clicked, this, &MainWindow::deleteTag);
    connect(m_resetButton, &PushButton::clicked, this, &MainWindow::resetValues);
    connect(m_table, &QTableWidget::itemSelectionChanged, this, &MainWindow::updateState);
    connect(m_table, &QTableWidget::cellDoubleClicked, this, [this](int, int column) {
        if (column != 3) editTag();
    });
    connect(m_table, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *item) {
        if (m_refreshing || item->column() != 3 || item->row() >= m_project.tags.size()) return;
        const int row = item->row();
        QVariant value;
        QString error;
        if (!parseValue(m_project.tags.at(row).dataType, item->text(), &value, &error)
            || !m_server->setValue(row, value, &error)) {
            showError(tr("Could not write value"), error);
        } else {
            appendLog(tr("Local write %1 = %2").arg(m_project.tags.at(row).address, valueText(value, m_project.tags.at(row).dataType)));
        }
        const QSignalBlocker blocker(m_table);
        item->setText(valueText(m_server->value(row), m_project.tags.at(row).dataType));
    });
    return page;
}

QWidget *MainWindow::createRulesPage()
{
    QVBoxLayout *layout = nullptr;
    QWidget *page = scrollPage(QStringLiteral("rulesPage"), &layout);
    pageHeading(layout, QT_TR_NOOP("Rules and connection guide"), QT_TR_NOOP("Start with a readable and writable S7 address, then test your HMI without hardware."));
    const QList<QPair<const char *, const char *>> rules = {
        {QT_TR_NOOP("Manual · manual"), QT_TR_NOOP("Keep the value written by a client or the table. Use it for commands, setpoints and manual fault injection.")},
        {QT_TR_NOOP("Sine wave · sine"), QT_TR_NOOP("Move smoothly between minimum and maximum. Use it for temperature, pressure, level and chart tests.")},
        {QT_TR_NOOP("Ramp · ramp"), QT_TR_NOOP("Increase linearly from minimum to maximum, then start again. Use it for range, threshold and animation tests.")},
        {QT_TR_NOOP("Toggle · toggle"), QT_TR_NOOP("BOOL only: invert the initial value every half period to simulate a heartbeat or running state.")},
        {QT_TR_NOOP("Follow · follow"), QT_TR_NOOP("Copy the current value of a source address. The demo maps a start command to running feedback.")}
    };
    for (const auto &rule : rules) {
        auto *card = new CardWidget(page);
        auto *body = cardLayout(card);
        body->addWidget(translatedLabel<StrongBodyLabel>(rule.first, card));
        body->addWidget(description(rule.second, card));
        layout->addWidget(card);
    }
    auto *guide = new CardWidget(page);
    auto *body = cardLayout(guide);
    body->addWidget(translatedLabel<SubtitleLabel>(QT_TR_NOOP("Connect in three steps"), guide));
    body->addWidget(description(QT_TR_NOOP("1. Load the demo in Variable monitor and start the server.\n2. Select classic S7comm in your HMI: 127.0.0.1, port 1102, rack 0 and slot 1.\n3. Read temperature and pressure using the listed addresses and types, then write the start command and observe feedback."), guide));
    body->addWidget(description(QT_TR_NOOP("Port 1102 is intended for development; the client must support a custom port. If your client requires port 102, stop the server and change the port; your OS may require permission. To connect from another computer, listen on this computer's LAN IP address."), guide));
    body->addWidget(description(QT_TR_NOOP("This tool provides S7 memory access and simple rules. It does not run Siemens ladder logic or PLC programs and does not provide S7CommPlus."), guide));
    layout->addWidget(guide);
    layout->addStretch();
    return page;
}

QWidget *MainWindow::createLogsPage()
{
    QVBoxLayout *layout = nullptr;
    QWidget *page = scrollPage(QStringLiteral("logsPage"), &layout);
    pageHeading(layout, QT_TR_NOOP("Server log"), QT_TR_NOOP("View server events, client connections and value operations. The most recent 2,000 lines are kept."));
    auto *tools = new QHBoxLayout;
    tools->addStretch();
    auto *clear = translatedButton<PushButton>(icon(FluentIcon::Delete), QT_TR_NOOP("Clear log"), page);
    clear->setObjectName(QStringLiteral("clearLogButton"));
    tools->addWidget(clear);
    layout->addLayout(tools);
    m_logEdit = new PlainTextEdit(page);
    m_logEdit->setObjectName(QStringLiteral("serverLogEdit"));
    m_logEdit->setReadOnly(true);
    m_logEdit->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_logEdit->document()->setMaximumBlockCount(2000);
    m_logEdit->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_logEdit->setMinimumHeight(380);
    layout->addWidget(m_logEdit, 1);
    connect(clear, &PushButton::clicked, m_logEdit, &QPlainTextEdit::clear);
    return page;
}

QWidget *MainWindow::createSettingsPage()
{
    QVBoxLayout *layout = nullptr;
    QWidget *page = scrollPage(QStringLiteral("settingsPage"), &layout);
    pageHeading(layout, QT_TR_NOOP("Settings"), QT_TR_NOOP("Project parameters are saved with the project. Appearance and language preferences are saved on this computer."));
    auto *projectCard = new CardWidget(page);
    auto *projectLayout = cardLayout(projectCard);
    projectLayout->addWidget(translatedLabel<SubtitleLabel>(QT_TR_NOOP("Current project"), projectCard));
    auto *form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setVerticalSpacing(14);
    m_projectNameEdit = new LineEdit(m_project.name, projectCard);
    m_projectNameEdit->setObjectName(QStringLiteral("projectNameEdit"));
    m_memorySpin = new SpinBox(projectCard);
    m_memorySpin->setObjectName(QStringLiteral("memorySizeSpin"));
    m_memorySpin->setRange(1, 65535);
    m_memorySpin->setSingleStep(256);
    m_memorySpin->setValue(m_project.memorySize);
    bindTranslation(m_memorySpin, "suffix", QT_TR_NOOP(" bytes"));
    translatedFormRow(form, QT_TR_NOOP("Project name"), m_projectNameEdit);
    translatedFormRow(form, QT_TR_NOOP("Bytes per DB / area"), m_memorySpin);
    projectLayout->addLayout(form);
    projectLayout->addWidget(description(QT_TR_NOOP("Memory must cover every variable address. Changes apply on the next start or reset and are locked while running."), projectCard));
    layout->addWidget(projectCard);
    connect(m_projectNameEdit, &QLineEdit::textChanged, this, [this](const QString &text) {
        m_project.name = text.trimmed();
        setDirty();
    });
    connect(m_memorySpin, &QSpinBox::valueChanged, this, [this](int value) {
        m_project.memorySize = value;
        setDirty();
    });
    auto *appearance = new CardWidget(page);
    appearance->setObjectName(QStringLiteral("appearanceCard"));
    auto *appearanceLayout = cardLayout(appearance);
    appearanceLayout->addWidget(translatedLabel<SubtitleLabel>(QT_TR_NOOP("Appearance"), appearance));
    auto *themeRow = new QHBoxLayout;
    themeRow->addWidget(translatedLabel<BodyLabel>(QT_TR_NOOP("App theme"), appearance));
    themeRow->addStretch();
    m_themeCombo = new ComboBox(appearance);
    m_themeCombo->setObjectName(QStringLiteral("themeCombo"));
    translatedComboItem(m_themeCombo, QT_TR_NOOP("Follow system"), QStringLiteral("Auto"));
    translatedComboItem(m_themeCombo, QT_TR_NOOP("Light"), QStringLiteral("Light"));
    translatedComboItem(m_themeCombo, QT_TR_NOOP("Dark"), QStringLiteral("Dark"));
    m_themeCombo->setCurrentIndex(m_themeCombo->findData(ThemeManager::instance()->theme() == Theme::Dark ? QStringLiteral("Dark") : ThemeManager::instance()->theme() == Theme::Light ? QStringLiteral("Light") : QStringLiteral("Auto")));
    m_themeCombo->setMinimumWidth(170);
    themeRow->addWidget(m_themeCombo);
    appearanceLayout->addLayout(themeRow);
    auto *languageRow = new QHBoxLayout;
    languageRow->addWidget(translatedLabel<BodyLabel>(QT_TR_NOOP("Language"), appearance));
    languageRow->addStretch();
    m_languageCombo = new ComboBox(appearance);
    m_languageCombo->setObjectName(QStringLiteral("languageCombo"));
    translatedComboItem(m_languageCombo, QT_TR_NOOP("Follow system"), QStringLiteral("Auto"));
    m_languageCombo->addItem(QStringLiteral("English"), {}, QStringLiteral("en_US"));
    m_languageCombo->addItem(QStringLiteral("简体中文"), {}, QStringLiteral("zh_CN"));
    m_languageCombo->setCurrentIndex(m_languageCombo->findData(I18n::setting()));
    m_languageCombo->setMinimumWidth(170);
    languageRow->addWidget(m_languageCombo);
    appearanceLayout->addLayout(languageRow);
    appearanceLayout->addWidget(description(QT_TR_NOOP("Language changes apply immediately and are remembered for the next launch."), appearance));
    appearanceLayout->addWidget(description(QT_TR_NOOP("Teal accent · Fluent desktop controls"), appearance));
    layout->addWidget(appearance);
    connect(m_themeCombo, &ComboBox::currentIndexChanged, this, [this](int) {
        const QString setting = m_themeCombo->currentData().toString();
        ThemeManager::instance()->setTheme(setting == QStringLiteral("Dark") ? Theme::Dark : setting == QStringLiteral("Light") ? Theme::Light : Theme::Auto);
        QSettings().setValue(QStringLiteral("ui/theme"), setting);
    });
    connect(m_languageCombo, &ComboBox::currentIndexChanged, this, [this](int) {
        if (!I18n::setLanguage(m_languageCombo->currentData().toString())) {
            const QSignalBlocker blocker(m_languageCombo);
            m_languageCombo->setCurrentIndex(m_languageCombo->findData(I18n::setting()));
        }
    });
    auto *updates = new CardWidget(page);
    updates->setObjectName(QStringLiteral("updatesCard"));
    auto *updatesLayout = cardLayout(updates);
    updatesLayout->addWidget(translatedLabel<SubtitleLabel>(QT_TR_NOOP("Application updates"), updates));
    updatesLayout->addWidget(description(QT_TR_NOOP("Download verified releases from GitHub. A prepared update is installed when the simulator exits. Project files and preferences are kept."), updates));
    auto *checkRow = new QHBoxLayout;
    checkRow->addWidget(translatedLabel<BodyLabel>(QT_TR_NOOP("Check at startup"), updates));
    checkRow->addStretch();
    auto *startup = new SwitchButton(updates);
    startup->setObjectName(QStringLiteral("checkAtStartupSwitch"));
    startup->setOnText(QString());
    startup->setOffText(QString());
    startup->setChecked(UpdateController::checkAtStartup());
    connect(startup, &SwitchButton::checkedChanged, this, &UpdateController::setCheckAtStartup);
    checkRow->addWidget(startup);
    updatesLayout->addLayout(checkRow);
    m_checkUpdateButton = translatedButton<PrimaryPushButton>(icon(FluentIcon::Update), QT_TR_NOOP("Check for updates"), updates);
    m_checkUpdateButton->setObjectName(QStringLiteral("checkUpdateButton"));
    connect(m_checkUpdateButton, &PushButton::clicked, m_updates, &UpdateController::checkNow);
    updatesLayout->addWidget(m_checkUpdateButton, 0, Qt::AlignLeft);
    layout->addWidget(updates);
    auto *repository = new CardWidget(page);
    repository->setObjectName(QStringLiteral("repositoryCard"));
    auto *repositoryLayout = cardLayout(repository);
    auto *repositoryTitle = translatedLabel<SubtitleLabel>(QT_TR_NOOP("Repository"), repository);
    repositoryTitle->setObjectName(QStringLiteral("repositoryTitleLabel"));
    repositoryLayout->addWidget(repositoryTitle);
    auto *projectName = translatedLabel<StrongBodyLabel>(QT_TR_NOOP("FluentPLC Simulator"), repository);
    projectName->setObjectName(QStringLiteral("repositoryNameLabel"));
    repositoryLayout->addWidget(projectName);
    repositoryLayout->addWidget(description(QT_TR_NOOP("Classic S7comm server for testing HMI and S7 clients without physical PLC hardware."), repository));

    auto *metadata = new QHBoxLayout;
    metadata->setSpacing(12);
    metadata->addWidget(translatedLabel<CaptionLabel>(QT_TR_NOOP("Version"), repository));
    const QString version = QCoreApplication::applicationVersion();
    auto *versionLabel = version.isEmpty() ? translatedLabel<BodyLabel>(QT_TR_NOOP("Development build"), repository)
                                          : new BodyLabel(version, repository);
    versionLabel->setObjectName(QStringLiteral("repositoryVersionLabel"));
    metadata->addWidget(versionLabel);
    metadata->addSpacing(12);
    metadata->addWidget(translatedLabel<CaptionLabel>(QT_TR_NOOP("License"), repository));
    auto *license = new BodyLabel(QStringLiteral("GPL-3.0"), repository);
    license->setObjectName(QStringLiteral("repositoryLicenseLabel"));
    metadata->addWidget(license);
    metadata->addStretch();
    repositoryLayout->addLayout(metadata);

    const QUrl repositoryUrl(QStringLiteral("https://github.com/txp666/FluentPLC-Simulator"));
    auto *repositoryLink = new HyperlinkLabel(repositoryUrl, QStringLiteral("github.com/txp666/FluentPLC-Simulator"), repository);
    repositoryLink->setObjectName(QStringLiteral("repositoryLink"));
    repositoryLink->setToolTip(repositoryUrl.toString());
    repositoryLayout->addWidget(repositoryLink, 0, Qt::AlignLeft);
    auto externalButton = [this, repository](const char *source, const QString &name, const QUrl &url) {
        auto *button = translatedButton<PushButton>(icon(FluentIcon::Document), source, repository);
        button->setObjectName(name);
        button->setProperty("externalUrl", url);
        button->setToolTip(url.toString());
        connect(button, &PushButton::clicked, this, [url] { QDesktopServices::openUrl(url); });
        return button;
    };
    auto *actions = new FlowLayout(0, 8, 8);
    actions->addWidget(externalButton(QT_TR_NOOP("Repository"), QStringLiteral("repositoryButton"), repositoryUrl));
    actions->addWidget(externalButton(QT_TR_NOOP("Report issue"), QStringLiteral("reportIssueButton"), QUrl(QStringLiteral("https://github.com/txp666/FluentPLC-Simulator/issues/new"))));
    actions->addWidget(externalButton(QT_TR_NOOP("Releases"), QStringLiteral("releasesButton"), QUrl(QStringLiteral("https://github.com/txp666/FluentPLC-Simulator/releases"))));
    repositoryLayout->addLayout(actions);

    auto *relatedTitle = translatedLabel<StrongBodyLabel>(QT_TR_NOOP("Related projects"), repository);
    relatedTitle->setObjectName(QStringLiteral("relatedProjectsLabel"));
    repositoryLayout->addWidget(relatedTitle);
    repositoryLayout->addWidget(description(QT_TR_NOOP("Explore the related HMI designers and browser tools."), repository));
    auto *related = new FlowLayout(0, 8, 8);
    related->addWidget(externalButton(QT_TR_NOOP("FluentHMI Studio"), QStringLiteral("relatedHmiButton"), QUrl(QStringLiteral("https://github.com/txp666/FluentHMI-Studio"))));
    related->addWidget(externalButton(QT_TR_NOOP("PyQt UI Designer"), QStringLiteral("relatedPythonButton"), QUrl(QStringLiteral("https://github.com/txp666/PyQt-UI-Designer"))));
    related->addWidget(externalButton(QT_TR_NOOP("modusignal"), QStringLiteral("relatedWebButton"), QUrl(QStringLiteral("https://github.com/txp666/modusignal"))));
    repositoryLayout->addLayout(related);
    layout->addWidget(repository);
    layout->addStretch();
    return page;
}

void MainWindow::createShortcuts()
{
    auto add = [this](const QKeySequence &shortcut, const std::function<void()> &callback) {
        auto *action = new QAction(this);
        action->setShortcut(shortcut);
        connect(action, &QAction::triggered, this, callback);
        addAction(action);
    };
    add(QKeySequence::New, [this] { newProject(); });
    add(QKeySequence::Open, [this] { chooseProject(); });
    add(QKeySequence::Save, [this] { saveCurrentProject(); });
    add(QKeySequence::SaveAs, [this] { saveProjectAs(); });
}

void MainWindow::retranslateUi()
{
    // Translate existing widgets in place. Never reload a project or touch a PLC
    // connection, a running generator clock, table editors or user-entered names.
    retranslateBindings(this);
    const QList<QPair<QString, QString>> navigation = {
        {QStringLiteral("monitorPage"), tr("Variable monitor")},
        {QStringLiteral("rulesPage"), tr("Rules & guide")},
        {QStringLiteral("logsPage"), tr("Server log")},
        {QStringLiteral("settingsPage"), tr("Settings")},
    };
    for (const auto &item : navigation) {
        if (auto *widget = qobject_cast<NavigationTreeWidget *>(navigationInterface()->widget(item.first))) widget->setText(item.second);
    }
    const QSignalBlocker tableBlocker(m_table);
    bool editingValue = false;
    for (auto *editor : m_table->findChildren<QLineEdit *>()) editingValue = editingValue || editor->isVisible();
    m_table->setHorizontalHeaderLabels({tr("Variable name"), tr("S7 address"), tr("Type"), tr("Current value"), tr("Rule")});
    for (int row = 0; row < m_project.tags.size(); ++row) {
        const Tag &tag = m_project.tags.at(row);
        if (auto *rule = m_table->item(row, 4)) {
            rule->setText(modeLabel(tag.mode));
            rule->setToolTip(tag.mode == QStringLiteral("follow") ? tr("Follow %1").arg(tag.source) : tag.mode);
        }
        if (!editingValue) {
            if (auto *value = m_table->item(row, 3)) value->setToolTip(tr("Double-click to write a temporary value; initial value: %1").arg(valueText(tag.initialValue, tag.dataType)));
        }
    }
    m_tagCountLabel->setText(tr("%1 variables").arg(m_project.tags.size()));
    const QSignalBlocker languageBlocker(m_languageCombo);
    m_languageCombo->setCurrentIndex(m_languageCombo->findData(I18n::setting()));
    updateState();
    updateTitle();
}

void MainWindow::changeEvent(QEvent *event)
{
    FluentWindow::changeEvent(event);
    if (event->type() == QEvent::LanguageChange && m_table && m_languageCombo && m_stateLabel) retranslateUi();
}

bool MainWindow::applyProject(const Project &project, const QString &path, bool dirty)
{
    QString error;
    if (!project.validate(&error) || !m_server->setProject(project, &error)) {
        showError(tr("Could not load project"), error);
        return false;
    }
    m_project = m_server->project();
    m_projectPath = path;
    m_dirty = dirty;
    m_elapsedSeconds = 0;
    const QSignalBlocker addressBlocker(m_addressEdit);
    const QSignalBlocker portBlocker(m_portSpin);
    const QSignalBlocker nameBlocker(m_projectNameEdit);
    const QSignalBlocker memoryBlocker(m_memorySpin);
    m_addressEdit->setText(project.bindAddress);
    m_portSpin->setValue(project.port);
    m_projectNameEdit->setText(project.name);
    m_memorySpin->setValue(project.memorySize);
    rebuildTable();
    updateState();
    updateTitle();
    return true;
}

bool MainWindow::openProject(const QString &path)
{
    if (m_server->isRunning()) {
        showError(tr("Server is running"), tr("Stop the server before opening another project."));
        return false;
    }
    Project loaded;
    QString error;
    if (!Project::load(path, &loaded, &error)) {
        showError(tr("Could not open project"), error);
        return false;
    }
    if (!confirmSaveChanges()) return false;
    if (!applyProject(loaded, QFileInfo(path).absoluteFilePath(), false)) return false;
    appendLog(tr("Opened project: %1").arg(path));
    showSuccess(tr("Project opened"), loaded.name);
    return true;
}

bool MainWindow::saveProject(const QString &path)
{
    if (path.trimmed().isEmpty()) return false;
    QString error;
    if (!m_project.validate(&error) || !m_project.save(path, &error)) {
        showError(tr("Could not save project"), error);
        return false;
    }
    m_projectPath = QFileInfo(path).absoluteFilePath();
    setDirty(false);
    appendLog(tr("Saved project: %1").arg(m_projectPath));
    showSuccess(tr("Project saved"), tr("Variable definitions and initial values were saved. Temporary runtime values are not written to the project."));
    return true;
}

bool MainWindow::confirmSaveChanges()
{
    if (!m_dirty) return true;
    QMessageBox box(QMessageBox::Question, tr("Save project changes"), tr("Save changes to \"%1\" before continuing?").arg(m_project.name),
                    QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, this);
    box.setObjectName(QStringLiteral("unsavedChangesDialog"));
    bindTranslation(&box, "windowTitle", QT_TR_NOOP("Save project changes"));
    bindTranslation(&box, "text", QT_TR_NOOP("Save changes to \"%1\" before continuing?"), {m_project.name});
    box.setDefaultButton(QMessageBox::Save);
    bindTranslation(box.button(QMessageBox::Save), "text", QT_TR_NOOP("Save"));
    bindTranslation(box.button(QMessageBox::Discard), "text", QT_TR_NOOP("Discard changes"));
    bindTranslation(box.button(QMessageBox::Cancel), "text", QT_TR_NOOP("Cancel"));
    const int result = box.exec();
    return result == QMessageBox::Discard || (result == QMessageBox::Save && saveCurrentProject());
}

bool MainWindow::saveCurrentProject()
{
    return m_projectPath.isEmpty() ? saveProjectAs() : saveProject(m_projectPath);
}

bool MainWindow::saveProjectAs()
{
    QString path = QFileDialog::getSaveFileName(this, tr("Save FluentPLC project"),
                    m_projectPath.isEmpty() ? m_project.name + QStringLiteral(".plcsim.json") : m_projectPath,
                    tr("FluentPLC project (*.plcsim.json);;JSON files (*.json);;All files (*)"));
    if (path.isEmpty()) return false;
    if (QFileInfo(path).suffix().isEmpty()) path += QStringLiteral(".plcsim.json");
    return saveProject(path);
}

void MainWindow::chooseProject()
{
    if (m_server->isRunning()) return;
    const QString path = QFileDialog::getOpenFileName(this, tr("Open FluentPLC project"), m_projectPath,
                    tr("FluentPLC project (*.plcsim.json *.json);;All files (*)"));
    if (!path.isEmpty()) openProject(path);
}

void MainWindow::newProject()
{
    if (m_server->isRunning() || !confirmSaveChanges()) return;
    Project project;
    project.name = tr("New S7 test bench");
    if (applyProject(project, {}, true)) appendLog(tr("Created a project. Add variables, then start the server."));
}

void MainWindow::loadDemo()
{
    if (m_server->isRunning() || !confirmSaveChanges()) return;
    if (applyProject(Project::demo(), {}, false)) {
        appendLog(tr("Loaded the temperature, pressure and start-feedback demo."));
        showSuccess(tr("Demo ready"), tr("Select Start server, then connect from your HMI."));
    }
}

void MainWindow::addTag()
{
    if (m_server->isRunning()) return;
    TagDialog dialog(m_project, -1, this);
    if (dialog.exec() != QDialog::Accepted) return;
    const Project next = dialog.updatedProject();
    if (applyProject(next, m_projectPath, true)) m_table->selectRow(next.tags.size() - 1);
}

void MainWindow::editTag()
{
    const int row = m_table->currentRow();
    if (m_server->isRunning() || row < 0 || row >= m_project.tags.size()) return;
    TagDialog dialog(m_project, row, this);
    if (dialog.exec() != QDialog::Accepted) return;
    const Project next = dialog.updatedProject();
    if (applyProject(next, m_projectPath, true)) m_table->selectRow(row);
}

void MainWindow::deleteTag()
{
    const int row = m_table->currentRow();
    if (m_server->isRunning() || row < 0 || row >= m_project.tags.size()) return;
    const Tag tag = m_project.tags.at(row);
    Dialog confirm(tr("Delete variable"), tr("Delete \"%1\" (%2)? Update any rules that follow this address first.").arg(tag.name, tag.address), this);
    confirm.setObjectName(QStringLiteral("deleteTagDialog"));
    bindTranslation(confirm.titleLabel(), "text", QT_TR_NOOP("Delete variable"));
    bindTranslation(confirm.contentLabel(), "text", QT_TR_NOOP("Delete \"%1\" (%2)? Update any rules that follow this address first."), {tag.name, tag.address});
    bindTranslation(confirm.yesButton(), "text", QT_TR_NOOP("Delete"));
    confirm.yesButton()->setObjectName(QStringLiteral("confirmDeleteTagButton"));
    bindTranslation(confirm.cancelButton(), "text", QT_TR_NOOP("Cancel"));
    if (confirm.exec() != QDialog::Accepted) return;
    Project next = m_project;
    next.tags.removeAt(row);
    if (applyProject(next, m_projectPath, true) && !next.tags.isEmpty()) m_table->selectRow(qMin(row, int(next.tags.size()) - 1));
}

void MainWindow::rebuildTable()
{
    const QSignalBlocker blocker(m_table);
    m_refreshing = true;
    m_table->setRowCount(m_project.tags.size());
    const QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    for (int row = 0; row < m_project.tags.size(); ++row) {
        const Tag &tag = m_project.tags.at(row);
        const QStringList fields = {tag.name, tag.address, tag.dataType,
                                    valueText(m_server->value(row), tag.dataType), modeLabel(tag.mode)};
        for (int column = 0; column < fields.size(); ++column) {
            auto *item = new QTableWidgetItem(fields.at(column));
            Qt::ItemFlags flags = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
            if (column == 3) flags |= Qt::ItemIsEditable;
            item->setFlags(flags);
            if (column == 1 || column == 3) item->setFont(mono);
            if (column == 3) item->setToolTip(tr("Double-click to write a temporary value; initial value: %1").arg(valueText(tag.initialValue, tag.dataType)));
            if (column == 4) item->setToolTip(tag.mode == QStringLiteral("follow") ? tr("Follow %1").arg(tag.source) : tag.mode);
            m_table->setItem(row, column, item);
        }
    }
    m_tagCountLabel->setText(tr("%1 variables").arg(m_project.tags.size()));
    m_refreshing = false;
}

void MainWindow::refreshValues()
{
    if (!m_table || m_table->rowCount() != m_project.tags.size()) return;
    // Updating model data while a delegate editor is active destroys its pending input.
    for (auto *editor : m_table->findChildren<QLineEdit *>()) if (editor->isVisible()) return;
    const QSignalBlocker blocker(m_table);
    m_refreshing = true;
    for (int row = 0; row < m_project.tags.size(); ++row) {
        const Tag &tag = m_project.tags.at(row);
        const QString text = valueText(m_server->value(row), tag.dataType);
        if (auto *item = m_table->item(row, 3)) {
            if (item->text() != text) item->setText(text);
            const QString tooltip = tr("Double-click to write a temporary value; initial value: %1").arg(valueText(tag.initialValue, tag.dataType));
            if (item->toolTip() != tooltip) item->setToolTip(tooltip);
        }
    }
    m_refreshing = false;
}

void MainWindow::updateState()
{
    const bool running = m_server->isRunning();
    m_startButton->setEnabled(!running);
    m_stopButton->setEnabled(running);
    m_addressEdit->setEnabled(!running);
    m_portSpin->setEnabled(!running);
    m_newButton->setEnabled(!running);
    m_openButton->setEnabled(!running);
    m_demoButton->setEnabled(!running);
    m_addButton->setEnabled(!running);
    m_resetButton->setEnabled(!running);
    if (m_projectNameEdit) m_projectNameEdit->setEnabled(!running);
    if (m_memorySpin) m_memorySpin->setEnabled(!running);
    const bool selected = m_table->currentRow() >= 0 && m_table->currentRow() < m_project.tags.size();
    m_editButton->setEnabled(!running && selected);
    m_deleteButton->setEnabled(!running && selected);
    m_stateLabel->setText(running ? tr("● Running") : tr("● Stopped"));
    m_stateLabel->setTextColor(running ? QColor(QStringLiteral("#087e74")) : QColor(QStringLiteral("#687780")),
                             running ? QColor(QStringLiteral("#55d6bd")) : QColor(QStringLiteral("#a6b6bf")));
    m_clientLabel->setText(tr("Clients %1").arg(m_server->clientCount()));
}

void MainWindow::updateTitle()
{
    // The custom Fluent title bar displays the title literally, so render the
    // unsaved marker instead of Qt's native-window [*] placeholder.
    setWindowTitle(QStringLiteral("%1%2 — FluentPLC Simulator")
                       .arg(m_project.name, m_dirty ? QStringLiteral(" *") : QString()));
    setWindowModified(m_dirty);
    const QString file = m_projectPath.isEmpty() ? tr("Not saved to a file") : m_projectPath;
    m_projectLabel->setText(QStringLiteral("%1  ·  %2%3").arg(m_project.name, file, m_dirty ? tr("  ·  Unsaved") : QString()));
}

void MainWindow::setDirty(bool dirty)
{
    m_dirty = dirty;
    updateTitle();
}

void MainWindow::startServer()
{
    if (m_server->isRunning()) return;
    m_project.bindAddress = m_addressEdit->text().trimmed();
    m_project.port = static_cast<quint16>(m_portSpin->value());
    QString error;
    // setProject is only needed when configuration changed; restarting preserves current memory.
    const Project &loaded = m_server->project();
    bool same = loaded.name == m_project.name && loaded.bindAddress == m_project.bindAddress
        && loaded.port == m_project.port && loaded.memorySize == m_project.memorySize;
    if (!m_project.validate(&error) || (!same && !m_server->setProject(m_project, &error)) || !m_server->start(&error)) {
        showError(tr("Could not start server"), error);
    }
    updateState();
}

void MainWindow::stopServer()
{
    m_server->stop();
    m_elapsedSeconds = 0;
    updateState();
    refreshValues();
}

void MainWindow::resetValues()
{
    if (m_server->isRunning()) return;
    QString error;
    if (!m_server->setProject(m_project, &error)) {
        showError(tr("Could not reset values"), error);
        return;
    }
    m_elapsedSeconds = 0;
    refreshValues();
    appendLog(tr("All variables were reset to the project initial values."));
    showSuccess(tr("Values reset"), tr("Project initial values restored."));
}

void MainWindow::showError(const QString &title, const QString &message)
{
    InfoBar::error(title, message, Qt::Horizontal, true, 5500, InfoBarPosition::TopRight, this);
    appendLog(tr("Error · %1: %2").arg(title, message));
}

void MainWindow::showSuccess(const QString &title, const QString &message)
{
    InfoBar::success(title, message, Qt::Horizontal, true, 2400, InfoBarPosition::TopRight, this);
}

void MainWindow::appendLog(const QString &message)
{
    if (m_logEdit) m_logEdit->appendPlainText(QStringLiteral("%1  %2").arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz")), message));
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (!confirmSaveChanges()) {
        m_restartForUpdate = false;
        event->ignore();
        return;
    }
    QString error;
    if (!m_updates->finishOnExit(m_restartForUpdate, &error) && m_restartForUpdate) {
        m_restartForUpdate = false;
        showError(tr("Could not start the update"), error);
        event->ignore();
        return;
    }
    m_server->stop();
    event->accept();
}

} // namespace fplc
