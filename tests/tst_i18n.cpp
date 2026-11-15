#include "core/address.h"
#include "core/i18n.h"
#include "core/project.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QChar>
#include <QtCore/QDirIterator>
#include <QtCore/QEvent>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QLocale>
#include <QtCore/QRegularExpression>
#include <QtCore/QSettings>
#include <QtCore/QSet>
#include <QtCore/QTemporaryDir>
#include <QtTest/QTest>

#include <memory>

#ifndef FPLC_SOURCE_DIR
#define FPLC_SOURCE_DIR ""
#endif

namespace {
class LanguageEvents final : public QObject {
public:
    int changes = 0;
    bool eventFilter(QObject *, QEvent *event) override
    {
        if (event->type() == QEvent::LanguageChange) ++changes;
        return false;
    }
};

QStringList placeholders(const QString &text)
{
    static const QRegularExpression expression(QStringLiteral("%(?:L?[1-9][0-9]?|n)"));
    QStringList result;
    auto matches = expression.globalMatch(text);
    while (matches.hasNext()) result.append(matches.next().captured());
    result.sort();
    return result;
}

QString decodeLiteralSequence(const QString &text)
{
    static const QRegularExpression literal(QStringLiteral("\"(?:\\\\.|[^\"\\\\])*\""));
    QString result;
    auto parts = literal.globalMatch(text);
    while (parts.hasNext()) {
        const QByteArray encoded = '[' + parts.next().captured().toUtf8() + ']';
        QJsonParseError error;
        const auto parsed = QJsonDocument::fromJson(encoded, &error);
        if (error.error != QJsonParseError::NoError || !parsed.isArray()) return {};
        result += parsed.array().at(0).toString();
    }
    return result;
}

bool containsCjkText(const QString &text)
{
    // Check letters' primary Unicode scripts instead of script extensions,
    // which can classify shared punctuation such as a middle dot as Han.
    // Walk scalar values so supplementary-plane ideographs are checked too.
    for (const char32_t point : text.toUcs4()) {
        if (!QChar::isLetter(point)) continue;
        switch (QChar::script(point)) {
        case QChar::Script_Han:
        case QChar::Script_Hiragana:
        case QChar::Script_Katakana:
        case QChar::Script_Hangul: return true;
        default: break;
        }
    }
    return false;
}
} // namespace

class I18nTest final : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void init();
    void cleanup();
    void automaticIsDefault();
    void automaticResolution_data();
    void automaticResolution();
    void chineseMessagesAndParameters();
    void switchingSendsLanguageChange();
    void settingsAndExplicitOverrides();
    void unsupportedLanguageDoesNotChangeState();
    void projectDataDoesNotTranslate();
    void sourceTextDetector_data();
    void sourceTextDetector();
    void sourceCatalogIsComplete();

private:
    std::unique_ptr<QTemporaryDir> settingsDirectory;
};

void I18nTest::initTestCase()
{
    settingsDirectory = std::make_unique<QTemporaryDir>();
    QVERIFY(settingsDirectory->isValid());
    QCoreApplication::setOrganizationName(QStringLiteral("FluentPLC-I18n-Tests"));
    QCoreApplication::setApplicationName(QStringLiteral("IsolatedSettings"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDirectory->path());
}

void I18nTest::init()
{
    QSettings().clear();
    fplc::I18n::initialize(QCoreApplication::instance(), QStringLiteral("en_US"));
}

void I18nTest::cleanup()
{
    fplc::I18n::setLanguage(QStringLiteral("en_US"), false);
    QSettings().clear();
}

void I18nTest::automaticIsDefault()
{
    fplc::I18n::initialize(QCoreApplication::instance());
    QCOMPARE(fplc::I18n::setting(), QStringLiteral("Auto"));
    QCOMPARE(fplc::I18n::language(), fplc::I18n::resolveLanguage(QStringLiteral("Auto")));
    QCOMPARE(QLocale().name(), fplc::I18n::language());
    QCOMPARE(fplc::Project().name, QStringLiteral("S7 Test Bench"));
    QVERIFY(!QSettings().contains(QStringLiteral("ui/language")));
}

void I18nTest::automaticResolution_data()
{
    QTest::addColumn<QString>("preference");
    QTest::addColumn<QString>("systemLocale");
    QTest::addColumn<QString>("resolved");
    QTest::newRow("Chinese system") << QStringLiteral("Auto") << QStringLiteral("zh_CN") << QStringLiteral("zh_CN");
    QTest::newRow("Traditional Chinese system") << QStringLiteral("Auto") << QStringLiteral("zh_TW") << QStringLiteral("zh_CN");
    QTest::newRow("English system") << QStringLiteral("Auto") << QStringLiteral("en_US") << QStringLiteral("en_US");
    QTest::newRow("Other system") << QStringLiteral("Auto") << QStringLiteral("de_DE") << QStringLiteral("en_US");
    QTest::newRow("English override") << QStringLiteral("en_US") << QStringLiteral("zh_CN") << QStringLiteral("en_US");
    QTest::newRow("Chinese override") << QStringLiteral("zh_CN") << QStringLiteral("en_US") << QStringLiteral("zh_CN");
}

void I18nTest::automaticResolution()
{
    QFETCH(QString, preference);
    QFETCH(QString, systemLocale);
    QFETCH(QString, resolved);
    QCOMPARE(fplc::I18n::resolveLanguage(preference, systemLocale), resolved);
}

void I18nTest::chineseMessagesAndParameters()
{
    QVERIFY(fplc::I18n::setLanguage(QStringLiteral("zh_CN"), false));
    QCOMPARE(fplc::I18n::language(), QStringLiteral("zh_CN"));
    QCOMPARE(QLocale().name(), QStringLiteral("zh_CN"));
    QCOMPARE(fplc::I18n::tr("Unsupported data type: %1").arg(QStringLiteral("TEST")), QStringLiteral("不支持的数据类型：TEST"));
    QCOMPARE(QCoreApplication::translate("arbitrary-context", "Invalid tag index"), QStringLiteral("标签索引无效"));
    QCOMPARE(fplc::I18n::tr("S7 data exchange: %1 reads, %2 writes").arg(3).arg(7), QStringLiteral("S7 数据交换：读取 3 次，写入 7 次"));
    fplc::Address address;
    QString error;
    QVERIFY(!fplc::parseAddress(QStringLiteral("invalid"), &address, &error));
    QVERIFY(error.contains(QStringLiteral("无效 S7 地址")));
    auto project = fplc::Project::demo();
    project.port = 0;
    QVERIFY(!project.validate(&error));
    QCOMPARE(error, QStringLiteral("端口必须为 1–65535"));
}

void I18nTest::switchingSendsLanguageChange()
{
    LanguageEvents events;
    auto *app = QCoreApplication::instance();
    app->installEventFilter(&events);
    QVERIFY(fplc::I18n::setLanguage(QStringLiteral("zh_CN"), false));
    QCoreApplication::processEvents();
    QVERIFY(events.changes > 0);
    const int previous = events.changes;
    QVERIFY(fplc::I18n::setLanguage(QStringLiteral("en_US"), false));
    QCoreApplication::processEvents();
    QVERIFY(events.changes > previous);
    app->removeEventFilter(&events);
}

void I18nTest::settingsAndExplicitOverrides()
{
    QVERIFY(fplc::I18n::setLanguage(QStringLiteral("zh_CN")));
    QCOMPARE(QSettings().value(QStringLiteral("ui/language")).toString(), QStringLiteral("zh_CN"));
    fplc::I18n::initialize(QCoreApplication::instance(), QStringLiteral("en_US"));
    QCOMPARE(fplc::I18n::language(), QStringLiteral("en_US"));
    QCOMPARE(fplc::I18n::tr("Invalid tag index"), QStringLiteral("Invalid tag index"));
    QCOMPARE(QSettings().value(QStringLiteral("ui/language")).toString(), QStringLiteral("zh_CN"));
    fplc::I18n::initialize(QCoreApplication::instance());
    QCOMPARE(fplc::I18n::language(), QStringLiteral("zh_CN"));
    QVERIFY(fplc::I18n::setLanguage(QStringLiteral("en_US"), false));
    QCOMPARE(QSettings().value(QStringLiteral("ui/language")).toString(), QStringLiteral("zh_CN"));
    QVERIFY(fplc::I18n::setLanguage(QStringLiteral("en_US")));
    QCOMPARE(QSettings().value(QStringLiteral("ui/language")).toString(), QStringLiteral("en_US"));
    QVERIFY(fplc::I18n::setLanguage(QStringLiteral("Auto")));
    QCOMPARE(fplc::I18n::setting(), QStringLiteral("Auto"));
    QCOMPARE(QSettings().value(QStringLiteral("ui/language")).toString(), QStringLiteral("Auto"));
    QVERIFY(fplc::I18n::setLanguage(QStringLiteral("zh_CN"), false));
    fplc::I18n::initialize(QCoreApplication::instance());
    QCOMPARE(fplc::I18n::setting(), QStringLiteral("Auto"));
    QCOMPARE(fplc::I18n::language(), fplc::I18n::resolveLanguage(QStringLiteral("Auto")));
    QVERIFY(fplc::I18n::setLanguage(QStringLiteral("en_US")));
    fplc::I18n::initialize(QCoreApplication::instance(), QStringLiteral("zh_CN"));
    QCOMPARE(fplc::I18n::language(), QStringLiteral("zh_CN"));
    QCOMPARE(QSettings().value(QStringLiteral("ui/language")).toString(), QStringLiteral("en_US"));
}

void I18nTest::unsupportedLanguageDoesNotChangeState()
{
    QVERIFY(!fplc::I18n::setLanguage(QStringLiteral("fr_FR")));
    QCOMPARE(fplc::I18n::language(), QStringLiteral("en_US"));
    QCOMPARE(QLocale().name(), QStringLiteral("en_US"));
    QVERIFY(!QSettings().contains(QStringLiteral("ui/language")));
    QSettings().setValue(QStringLiteral("ui/language"), QStringLiteral("unsupported"));
    fplc::I18n::initialize(QCoreApplication::instance());
    QCOMPARE(fplc::I18n::setting(), QStringLiteral("Auto"));
    QCOMPARE(fplc::I18n::language(), fplc::I18n::resolveLanguage(QStringLiteral("Auto")));
}

void I18nTest::projectDataDoesNotTranslate()
{
    auto project = fplc::Project::demo();
    project.name = QStringLiteral("客户项目 — UTF-8");
    project.tags[0].name = QStringLiteral("运行反馈（用户命名）");
    const QString name = project.tags.at(0).name;
    QVERIFY(fplc::I18n::setLanguage(QStringLiteral("zh_CN"), false));
    QCOMPARE(project.tags.at(0).name, name);
    QCOMPARE(fplc::Project::demo().tags.at(2).name, QStringLiteral("Temperature"));
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("unicode.plcsim.json"));
    QString error;
    QVERIFY2(project.save(path, &error), qPrintable(error));
    fplc::Project loaded;
    QVERIFY2(fplc::Project::load(path, &loaded, &error), qPrintable(error));
    QCOMPARE(loaded.name, project.name);
    QCOMPARE(loaded.tags.at(0).name, name);
    QVERIFY(fplc::I18n::setLanguage(QStringLiteral("en_US"), false));
    QCOMPARE(loaded.tags.at(0).name, name);
}

void I18nTest::sourceTextDetector_data()
{
    QTest::addColumn<QString>("text");
    QTest::addColumn<bool>("containsCjk");
    QTest::newRow("ASCII source") << QStringLiteral("S7 memory, TCP port 1102") << false;
    QTest::newRow("Middle-dot separator") << QStringLiteral("Sine wave · sine") << false;
    QTest::newRow("Shared punctuation") << QStringLiteral("“English” — source・with punctuation") << false;
    QTest::newRow("Engineering units") << QStringLiteral("20 °C ± 0.5 µs / m³ / Ω") << false;
    QTest::newRow("Chinese letters") << QStringLiteral("Start 启动") << true;
    QTest::newRow("Supplementary ideograph") << QStringLiteral("Test 𠀀") << true;
    QTest::newRow("Hiragana letters") << QStringLiteral("ひらがな") << true;
    QTest::newRow("Katakana letters") << QStringLiteral("カタカナ") << true;
    QTest::newRow("Hangul letters") << QStringLiteral("설정") << true;
}

void I18nTest::sourceTextDetector()
{
    QFETCH(QString, text);
    QFETCH(bool, containsCjk);
    QCOMPARE(containsCjkText(text), containsCjk);
}

void I18nTest::sourceCatalogIsComplete()
{
    const QString sourceRoot = QString::fromUtf8(FPLC_SOURCE_DIR);
    QVERIFY2(!sourceRoot.isEmpty(), "tst_i18n needs FPLC_SOURCE_DIR set to the repository root");
    QFile file(QStringLiteral(":/i18n/zh_CN.json"));
    QVERIFY(file.open(QIODevice::ReadOnly));
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(file.readAll(), &error);
    QCOMPARE(error.error, QJsonParseError::NoError);
    QVERIFY(document.isObject());
    const QJsonObject catalog = document.object();
    QVERIFY(fplc::I18n::setLanguage(QStringLiteral("zh_CN"), false));
    QSet<QString> sources;
    QStringList failures;
    const QRegularExpression calls(QStringLiteral("(?:\\btr|\\bQT_TR_NOOP)\\s*\\(\\s*((?:\"(?:\\\\.|[^\"\\\\])*\"\\s*)+)"));
    QDirIterator files(sourceRoot + QStringLiteral("/src"), {QStringLiteral("*.cpp"), QStringLiteral("*.h")}, QDir::Files, QDirIterator::Subdirectories);
    while (files.hasNext()) {
        QFile source(files.next());
        QVERIFY2(source.open(QIODevice::ReadOnly), qPrintable(source.fileName()));
        auto matches = calls.globalMatch(QString::fromUtf8(source.readAll()));
        while (matches.hasNext()) {
            const QString text = decodeLiteralSequence(matches.next().captured(1));
            QVERIFY2(!text.isEmpty(), qPrintable(source.fileName() + QStringLiteral(": cannot decode translation source")));
            if (containsCjkText(text)) failures.append(source.fileName() + QStringLiteral(": source must be English: ") + text);
            sources.insert(text);
        }
    }
    QVERIFY(sources.size() > 50);
    for (const QString &text : sources) {
        const QJsonValue translated = catalog.value(text);
        if (!translated.isString() || translated.toString().isEmpty()) {
            failures.append(QStringLiteral("Missing zh_CN translation: ") + text);
            continue;
        }
        if (placeholders(text) != placeholders(translated.toString())) failures.append(QStringLiteral("Placeholder mismatch: ") + text);
        if (QCoreApplication::translate("coverage-check", text.toUtf8().constData()) != translated.toString())
            failures.append(QStringLiteral("Resource translation mismatch: ") + text);
    }
    QVERIFY2(failures.isEmpty(), qPrintable(failures.join(QLatin1Char('\n'))));
}

QTEST_GUILESS_MAIN(I18nTest)
#include "tst_i18n.moc"
