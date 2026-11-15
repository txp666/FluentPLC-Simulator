#include "app/main_window.h"
#include "plc/plc_server.h"

#include <FluentQtWidgets/Dialogs/Dialog.h>
#include <FluentQtWidgets/FluentIcon.h>
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

#include <QtCore/QDateTime>
#include <QtCore/QFileInfo>
#include <QtCore/QSettings>
#include <QtCore/QSignalBlocker>
#include <QtCore/QTimer>
#include <QtGui/QAction>
#include <QtGui/QCloseEvent>
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

QString modeLabel(const QString &mode)
{
    if (mode == QStringLiteral("sine")) return QObject::tr("正弦波");
    if (mode == QStringLiteral("ramp")) return QObject::tr("斜坡");
    if (mode == QStringLiteral("toggle")) return QObject::tr("周期翻转");
    if (mode == QStringLiteral("follow")) return QObject::tr("跟随变量");
    return QObject::tr("手动");
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
        *error = QObject::tr("BOOL 请输入 true、false、1 或 0。");
        return false;
    }
    if (type == QStringLiteral("REAL")) {
        const double number = clean.toDouble(&ok);
        if (ok && std::isfinite(number)) {
            *value = number;
            return true;
        }
        *error = QObject::tr("REAL 请输入有限数值，例如 23.5。");
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
    *error = QObject::tr("请输入整数；也可以使用 0x 开头的十六进制值。");
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

void pageHeading(QVBoxLayout *layout, const QString &title, const QString &subtitle)
{
    auto *titleLabel = new TitleLabel(title);
    layout->addWidget(titleLabel);
    layout->addWidget(description(subtitle, titleLabel->parentWidget()));
}

class TagDialog final : public Dialog {
public:
    TagDialog(const Project &project, int index, QWidget *parent)
        : Dialog(index < 0 ? tr("新增变量") : tr("编辑变量"),
                 tr("定义 S7 地址、初始值和变化规则。保存后将重新装载工程初始值。"), parent),
          m_project(project), m_index(index)
    {
        setObjectName(QStringLiteral("tagDialog"));
        setWindowTitle(index < 0 ? tr("新增变量") : tr("编辑变量"));
        setMinimumWidth(560);
        Tag tag;
        if (index >= 0) {
            tag = project.tags.at(index);
        } else {
            tag.name = tr("变量 %1").arg(project.tags.size() + 1);
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
        m_period->setSuffix(tr(" 秒"));
        m_source = new LineEdit(tag.source, view());
        m_source->setObjectName(QStringLiteral("tagSourceEdit"));
        m_source->setPlaceholderText(tr("来源变量地址，例如 DB1.DBX8.0"));
        form->addRow(tr("变量名称"), m_name);
        form->addRow(tr("S7 地址"), m_address);
        form->addRow(tr("数据类型"), m_type);
        form->addRow(tr("初始值"), m_initial);
        form->addRow(tr("变化规则"), m_mode);
        form->addRow(tr("最小值"), m_minimum);
        form->addRow(tr("最大值"), m_maximum);
        form->addRow(tr("周期"), m_period);
        form->addRow(tr("跟随地址"), m_source);
        viewLayout()->addLayout(form);
        m_ruleHint = description(QString(), view());
        m_ruleHint->setMinimumHeight(38);
        viewLayout()->addWidget(m_ruleHint);
        m_error = new BodyLabel(view());
        m_error->setObjectName(QStringLiteral("tagErrorLabel"));
        m_error->setWordWrap(true);
        m_error->setTextColor(QColor(QStringLiteral("#b42318")), QColor(QStringLiteral("#ffb4ab")));
        viewLayout()->addWidget(m_error);
        yesButton()->setText(tr("保存变量"));
        yesButton()->setObjectName(QStringLiteral("tagAcceptButton"));
        cancelButton()->setText(tr("取消"));
        cancelButton()->setObjectName(QStringLiteral("tagCancelButton"));
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
        if (mode == QStringLiteral("manual")) m_ruleHint->setText(tr("手动：客户端或表格写入后保持数值，直至下次写入或重置。"));
        else if (mode == QStringLiteral("sine")) m_ruleHint->setText(tr("正弦波：在最小值与最大值之间平滑变化，一个周期完成一次往返。"));
        else if (mode == QStringLiteral("ramp")) m_ruleHint->setText(tr("斜坡：从最小值线性增加到最大值，周期结束后重新开始。"));
        else if (mode == QStringLiteral("toggle")) m_ruleHint->setText(tr("周期翻转：仅用于 BOOL，每半个周期相对初始值翻转一次。"));
        else m_ruleHint->setText(tr("跟随变量：每次更新时读取来源地址，将其当前值复制到此变量。"));
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
    const QString theme = QSettings().value(QStringLiteral("ui/theme"), QStringLiteral("Light")).toString();
    themes->setTheme(theme == QStringLiteral("Dark") ? Theme::Dark : theme == QStringLiteral("Auto") ? Theme::Auto : Theme::Light);
    themes->applyTo(qApp);

    m_monitorPage = createMonitorPage();
    addSubInterface(m_monitorPage, icon(FluentIcon::View), tr("变量监视"), QStringLiteral("monitorPage"));
    addSubInterface(createRulesPage(), icon(FluentIcon::Code), tr("动态规则与指南"), QStringLiteral("rulesPage"));
    addSubInterface(createLogsPage(), icon(FluentIcon::History), tr("服务日志"), QStringLiteral("logsPage"));
    addSubInterface(createSettingsPage(), icon(FluentIcon::Setting), tr("设置"), QStringLiteral("settingsPage"), NavigationItemPosition::Bottom);
    createShortcuts();

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
    if (!m_server->setProject(m_project, &error)) showError(tr("示例载入失败"), error);
    rebuildTable();
    updateState();
    updateTitle();
    switchTo(m_monitorPage);
    appendLog(tr("已载入内置示例。服务当前停止，点击“启动服务”后才接受客户端连接。"));
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
    pageHeading(layout, tr("变量监视"), tr("让上位机连接一台可读写的模拟 PLC，实时验证显示、控制和曲线。"));

    auto *projectTools = new QHBoxLayout;
    projectTools->setSpacing(8);
    auto projectButton = [page, projectTools](const QString &text, const QString &name, FluentIcon symbol) {
        auto *button = new PushButton(icon(symbol), text, page);
        button->setObjectName(name);
        projectTools->addWidget(button);
        return button;
    };
    m_newButton = projectButton(tr("新建"), QStringLiteral("newProjectButton"), FluentIcon::Add);
    m_openButton = projectButton(tr("打开"), QStringLiteral("openProjectButton"), FluentIcon::Folder);
    auto *save = projectButton(tr("保存"), QStringLiteral("saveProjectButton"), FluentIcon::Save);
    auto *saveAs = projectButton(tr("另存为"), QStringLiteral("saveAsProjectButton"), FluentIcon::Document);
    projectTools->addStretch();
    m_demoButton = projectButton(tr("载入示例"), QStringLiteral("demoProjectButton"), FluentIcon::Play);
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
    serviceHead->addWidget(new StrongBodyLabel(tr("S7 服务"), serviceCard));
    serviceHead->addStretch();
    m_stateLabel = new BodyLabel(serviceCard);
    m_stateLabel->setObjectName(QStringLiteral("serverStateLabel"));
    serviceHead->addWidget(m_stateLabel);
    service->addLayout(serviceHead);
    auto *connection = new QHBoxLayout;
    connection->setSpacing(12);
    auto *addressColumn = new QVBoxLayout;
    addressColumn->setSpacing(6);
    addressColumn->addWidget(new CaptionLabel(tr("监听地址"), serviceCard));
    m_addressEdit = new LineEdit(m_project.bindAddress, serviceCard);
    m_addressEdit->setObjectName(QStringLiteral("serverAddressEdit"));
    m_addressEdit->setPlaceholderText(QStringLiteral("127.0.0.1"));
    addressColumn->addWidget(m_addressEdit);
    connection->addLayout(addressColumn, 1);
    auto *portColumn = new QVBoxLayout;
    portColumn->setSpacing(6);
    portColumn->addWidget(new CaptionLabel(tr("TCP 端口"), serviceCard));
    m_portSpin = new SpinBox(serviceCard);
    m_portSpin->setObjectName(QStringLiteral("serverPortSpin"));
    m_portSpin->setRange(1, 65535);
    m_portSpin->setValue(m_project.port);
    m_portSpin->setFixedWidth(130);
    portColumn->addWidget(m_portSpin);
    connection->addLayout(portColumn);
    m_startButton = new PrimaryPushButton(icon(FluentIcon::Play), tr("启动服务"), serviceCard);
    m_startButton->setObjectName(QStringLiteral("startServerButton"));
    connection->addWidget(m_startButton, 0, Qt::AlignBottom);
    m_stopButton = new PushButton(icon(FluentIcon::Pause), tr("停止"), serviceCard);
    m_stopButton->setObjectName(QStringLiteral("stopServerButton"));
    connection->addWidget(m_stopButton, 0, Qt::AlignBottom);
    service->addLayout(connection);
    auto *connectionFoot = new QHBoxLayout;
    connectionFoot->addWidget(description(tr("默认只接受本机连接 · S7comm · 无需连接真实 PLC"), serviceCard), 1);
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
    tagTools->addWidget(new StrongBodyLabel(tr("模拟变量"), tagsCard));
    m_tagCountLabel = description(QString(), tagsCard);
    m_tagCountLabel->setObjectName(QStringLiteral("tagCountLabel"));
    tagTools->addWidget(m_tagCountLabel);
    tagTools->addStretch();
    auto tagButton = [tagsCard, tagTools](const QString &text, const QString &name, FluentIcon symbol) {
        auto *button = new PushButton(icon(symbol), text, tagsCard);
        button->setObjectName(name);
        tagTools->addWidget(button);
        return button;
    };
    m_addButton = tagButton(tr("新增"), QStringLiteral("addTagButton"), FluentIcon::Add);
    m_editButton = tagButton(tr("编辑"), QStringLiteral("editTagButton"), FluentIcon::Edit);
    m_deleteButton = tagButton(tr("删除"), QStringLiteral("deleteTagButton"), FluentIcon::Delete);
    m_resetButton = tagButton(tr("重置值"), QStringLiteral("resetValuesButton"), FluentIcon::History);
    tags->addLayout(tagTools);
    tags->addWidget(description(tr("双击“当前值”可临时写入。初始值与动态规则在“编辑”中设置；运行时请先停止再修改结构。"), tagsCard));
    m_table = new TableWidget(tagsCard);
    m_table->setObjectName(QStringLiteral("tagTable"));
    m_table->setColumnCount(5);
    m_table->setHorizontalHeaderLabels({tr("变量名称"), tr("S7 地址"), tr("类型"), tr("当前值"), tr("动态规则")});
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
            showError(tr("数值写入失败"), error);
        } else {
            appendLog(tr("本地写入 %1 = %2").arg(m_project.tags.at(row).address, valueText(value, m_project.tags.at(row).dataType)));
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
    pageHeading(layout, tr("动态规则与使用指南"), tr("从一个能读写的 S7 地址开始，在没有硬件的环境中验证上位机行为。"));
    const QList<QPair<QString, QString>> rules = {
        {tr("手动 · manual"), tr("由客户端或变量表写入，保留当前值。适合按钮命令、设定值和手动故障注入。")},
        {tr("正弦波 · sine"), tr("在最小值与最大值之间平滑往返。适合温度、压力、液位与实时曲线测试。")},
        {tr("斜坡 · ramp"), tr("从最小值线性升到最大值，再从头开始。适合量程、阈值和动画测试。")},
        {tr("周期翻转 · toggle"), tr("仅用于 BOOL，每半个周期相对初始值翻转一次，可模拟心跳或运行状态。")},
        {tr("跟随变量 · follow"), tr("复制来源地址的当前值。内置示例用它将启动命令映射为运行反馈。")}
    };
    for (const auto &rule : rules) {
        auto *card = new CardWidget(page);
        auto *body = cardLayout(card);
        body->addWidget(new StrongBodyLabel(rule.first, card));
        body->addWidget(description(rule.second, card));
        layout->addWidget(card);
    }
    auto *guide = new CardWidget(page);
    auto *body = cardLayout(guide);
    body->addWidget(new SubtitleLabel(tr("三步连接"), guide));
    body->addWidget(description(tr("1. 在“变量监视”载入示例并启动服务。\n2. 在上位机中选择经典 S7comm，地址 127.0.0.1，端口 1102，机架 0、插槽 1。\n3. 使用表格中的地址与类型读取温度和压力，写入启动命令并观察运行反馈。"), guide));
    body->addWidget(description(tr("1102 是开发测试端口，客户端需支持自定义端口。若客户端固定使用 102，可停止服务后改为 102；操作系统可能要求相应权限。跨电脑连接时，将监听地址改为本机局域网 IP。"), guide));
    body->addWidget(description(tr("本工具提供 S7 读写内存和简单动态规则，不运行西门子梯形图或真实 PLC 程序，也不提供 S7CommPlus。"), guide));
    layout->addWidget(guide);
    layout->addStretch();
    return page;
}

QWidget *MainWindow::createLogsPage()
{
    QVBoxLayout *layout = nullptr;
    QWidget *page = scrollPage(QStringLiteral("logsPage"), &layout);
    pageHeading(layout, tr("服务日志"), tr("查看服务启动、客户端连接与数值操作。保留最近 2,000 行。"));
    auto *tools = new QHBoxLayout;
    tools->addStretch();
    auto *clear = new PushButton(icon(FluentIcon::Delete), tr("清空日志"), page);
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
    pageHeading(layout, tr("设置"), tr("工程参数随项目保存，外观设置仅保存在这台电脑。"));
    auto *projectCard = new CardWidget(page);
    auto *projectLayout = cardLayout(projectCard);
    projectLayout->addWidget(new SubtitleLabel(tr("当前工程"), projectCard));
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
    m_memorySpin->setSuffix(tr(" 字节"));
    form->addRow(tr("工程名称"), m_projectNameEdit);
    form->addRow(tr("每个 DB / 区域容量"), m_memorySpin);
    projectLayout->addLayout(form);
    projectLayout->addWidget(description(tr("容量需覆盖全部变量地址。修改后将在下一次启动或重置时装载；运行时不可修改。"), projectCard));
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
    auto *appearanceLayout = cardLayout(appearance);
    appearanceLayout->addWidget(new SubtitleLabel(tr("外观"), appearance));
    auto *themeRow = new QHBoxLayout;
    themeRow->addWidget(new BodyLabel(tr("应用主题"), appearance));
    themeRow->addStretch();
    m_themeCombo = new ComboBox(appearance);
    m_themeCombo->setObjectName(QStringLiteral("themeCombo"));
    m_themeCombo->addItem(tr("浅色"), {}, QStringLiteral("Light"));
    m_themeCombo->addItem(tr("深色"), {}, QStringLiteral("Dark"));
    m_themeCombo->addItem(tr("跟随系统"), {}, QStringLiteral("Auto"));
    m_themeCombo->setCurrentIndex(ThemeManager::instance()->theme() == Theme::Dark ? 1 : ThemeManager::instance()->theme() == Theme::Auto ? 2 : 0);
    m_themeCombo->setMinimumWidth(170);
    themeRow->addWidget(m_themeCombo);
    appearanceLayout->addLayout(themeRow);
    appearanceLayout->addWidget(description(tr("青绿色强调色 · Fluent 桌面控件"), appearance));
    layout->addWidget(appearance);
    connect(m_themeCombo, &ComboBox::currentIndexChanged, this, [this](int index) {
        ThemeManager::instance()->setTheme(index == 1 ? Theme::Dark : index == 2 ? Theme::Auto : Theme::Light);
        QSettings().setValue(QStringLiteral("ui/theme"), m_themeCombo->currentData().toString());
    });
    auto *about = new CardWidget(page);
    auto *aboutLayout = cardLayout(about);
    aboutLayout->addWidget(new StrongBodyLabel(tr("FluentPLC Simulator"), about));
    aboutLayout->addWidget(description(tr("独立桌面 S7 模拟器。默认载入示例并保持服务停止，仅在你启动后监听连接。"), about));
    layout->addWidget(about);
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

bool MainWindow::applyProject(const Project &project, const QString &path, bool dirty)
{
    QString error;
    if (!project.validate(&error) || !m_server->setProject(project, &error)) {
        showError(tr("无法装载工程"), error);
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
        showError(tr("服务正在运行"), tr("请先停止服务，再打开其他工程。"));
        return false;
    }
    Project loaded;
    QString error;
    if (!Project::load(path, &loaded, &error)) {
        showError(tr("打开失败"), error);
        return false;
    }
    if (!confirmSaveChanges()) return false;
    if (!applyProject(loaded, QFileInfo(path).absoluteFilePath(), false)) return false;
    appendLog(tr("已打开工程：%1").arg(path));
    showSuccess(tr("工程已打开"), loaded.name);
    return true;
}

bool MainWindow::saveProject(const QString &path)
{
    if (path.trimmed().isEmpty()) return false;
    QString error;
    if (!m_project.validate(&error) || !m_project.save(path, &error)) {
        showError(tr("保存失败"), error);
        return false;
    }
    m_projectPath = QFileInfo(path).absoluteFilePath();
    setDirty(false);
    appendLog(tr("已保存工程：%1").arg(m_projectPath));
    showSuccess(tr("工程已保存"), tr("已保存变量配置与初始值，临时运行数值不写入工程。"));
    return true;
}

bool MainWindow::confirmSaveChanges()
{
    if (!m_dirty) return true;
    QMessageBox box(QMessageBox::Question, tr("保存工程更改"), tr("“%1”有未保存的更改。是否保存后继续？").arg(m_project.name),
                    QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, this);
    box.setObjectName(QStringLiteral("unsavedChangesDialog"));
    box.setDefaultButton(QMessageBox::Save);
    box.button(QMessageBox::Save)->setText(tr("保存"));
    box.button(QMessageBox::Discard)->setText(tr("放弃更改"));
    box.button(QMessageBox::Cancel)->setText(tr("取消"));
    const int result = box.exec();
    return result == QMessageBox::Discard || (result == QMessageBox::Save && saveCurrentProject());
}

bool MainWindow::saveCurrentProject()
{
    return m_projectPath.isEmpty() ? saveProjectAs() : saveProject(m_projectPath);
}

bool MainWindow::saveProjectAs()
{
    QString path = QFileDialog::getSaveFileName(this, tr("保存 FluentPLC 工程"),
                    m_projectPath.isEmpty() ? m_project.name + QStringLiteral(".plcsim.json") : m_projectPath,
                    tr("FluentPLC 工程 (*.plcsim.json);;JSON 文件 (*.json);;所有文件 (*)"));
    if (path.isEmpty()) return false;
    if (QFileInfo(path).suffix().isEmpty()) path += QStringLiteral(".plcsim.json");
    return saveProject(path);
}

void MainWindow::chooseProject()
{
    if (m_server->isRunning()) return;
    const QString path = QFileDialog::getOpenFileName(this, tr("打开 FluentPLC 工程"), m_projectPath,
                    tr("FluentPLC 工程 (*.plcsim.json *.json);;所有文件 (*)"));
    if (!path.isEmpty()) openProject(path);
}

void MainWindow::newProject()
{
    if (m_server->isRunning() || !confirmSaveChanges()) return;
    Project project;
    project.name = tr("新建 S7 测试台");
    if (applyProject(project, {}, true)) appendLog(tr("已新建工程。添加变量后启动服务。"));
}

void MainWindow::loadDemo()
{
    if (m_server->isRunning() || !confirmSaveChanges()) return;
    if (applyProject(Project::demo(), {}, false)) {
        appendLog(tr("已载入温度、压力与启动反馈示例。"));
        showSuccess(tr("示例已就绪"), tr("点击“启动服务”后，即可从上位机连接。"));
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
    Dialog confirm(tr("删除变量"), tr("确定删除“%1”（%2）？引用它的跟随规则需要先修改。 ").arg(tag.name, tag.address), this);
    confirm.setObjectName(QStringLiteral("deleteTagDialog"));
    confirm.yesButton()->setText(tr("删除"));
    confirm.yesButton()->setObjectName(QStringLiteral("confirmDeleteTagButton"));
    confirm.cancelButton()->setText(tr("取消"));
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
            if (column == 3) item->setToolTip(tr("双击写入临时值；初始值为 %1").arg(valueText(tag.initialValue, tag.dataType)));
            if (column == 4) item->setToolTip(tag.mode == QStringLiteral("follow") ? tr("跟随 %1").arg(tag.source) : tag.mode);
            m_table->setItem(row, column, item);
        }
    }
    m_tagCountLabel->setText(tr("%1 个变量").arg(m_project.tags.size()));
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
        const QString text = valueText(m_server->value(row), m_project.tags.at(row).dataType);
        if (auto *item = m_table->item(row, 3); item && item->text() != text) item->setText(text);
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
    m_stateLabel->setText(running ? tr("● 运行中") : tr("● 已停止"));
    m_stateLabel->setTextColor(running ? QColor(QStringLiteral("#087e74")) : QColor(QStringLiteral("#687780")),
                             running ? QColor(QStringLiteral("#55d6bd")) : QColor(QStringLiteral("#a6b6bf")));
    m_clientLabel->setText(tr("客户端 %1").arg(m_server->clientCount()));
}

void MainWindow::updateTitle()
{
    setWindowTitle(QStringLiteral("%1[*] — FluentPLC Simulator").arg(m_project.name));
    setWindowModified(m_dirty);
    const QString file = m_projectPath.isEmpty() ? tr("尚未保存为文件") : m_projectPath;
    m_projectLabel->setText(QStringLiteral("%1  ·  %2%3").arg(m_project.name, file, m_dirty ? tr("  ·  未保存") : QString()));
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
        showError(tr("服务启动失败"), error);
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
        showError(tr("重置失败"), error);
        return;
    }
    m_elapsedSeconds = 0;
    refreshValues();
    appendLog(tr("所有变量已恢复为工程初始值。"));
    showSuccess(tr("数值已重置"), tr("已恢复工程初始值。"));
}

void MainWindow::showError(const QString &title, const QString &message)
{
    InfoBar::error(title, message, Qt::Horizontal, true, 5500, InfoBarPosition::TopRight, this);
    appendLog(tr("错误 · %1：%2").arg(title, message));
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
        event->ignore();
        return;
    }
    m_server->stop();
    event->accept();
}

} // namespace fplc
