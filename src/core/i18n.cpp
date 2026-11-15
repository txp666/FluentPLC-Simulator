#include "core/i18n.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QFile>
#include <QtCore/QHash>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QLocale>
#include <QtCore/QPointer>
#include <QtCore/QResource>
#include <QtCore/QSettings>
#include <QtCore/QThread>
#include <QtCore/QTranslator>

// Keep this helper in the global namespace: Q_INIT_RESOURCE must reference the
// global rcc symbol, including when fplc_core is linked as a static library.
static void initializeI18nResources()
{
    Q_INIT_RESOURCE(i18n);
}

namespace fplc::I18n {
namespace {
class DictionaryTranslator final : public QTranslator {
public:
    explicit DictionaryTranslator(QObject *parent) : QTranslator(parent) {}

    bool readCatalog()
    {
        QFile file(QStringLiteral(":/i18n/zh_CN.json"));
        if (!file.open(QIODevice::ReadOnly)) return false;
        QJsonParseError error;
        const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
        if (error.error != QJsonParseError::NoError || !document.isObject() || document.object().isEmpty()) return false;
        QHash<QString, QString> messages;
        const QJsonObject object = document.object();
        for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
            if (it.key().isEmpty() || !it.value().isString() || it.value().toString().isEmpty()) return false;
            messages.insert(it.key(), it.value().toString());
        }
        m_messages = std::move(messages);
        return true;
    }

    QString translate(const char *, const char *source, const char *, int n) const override
    {
        if (!source) return {};
        const auto found = m_messages.constFind(QString::fromUtf8(source));
        if (found == m_messages.constEnd()) return {}; // Qt falls back to its English source.
        QString text = *found;
        if (n >= 0) text.replace(QStringLiteral("%n"), QString::number(n));
        return text;
    }

    bool isEmpty() const override { return m_messages.isEmpty(); }

private:
    QHash<QString, QString> m_messages;
};

struct State {
    QPointer<QCoreApplication> app;
    QPointer<DictionaryTranslator> translator;
    QString locale = QStringLiteral("en_US");
    QString preference = QStringLiteral("Auto");
    bool installed = false;
};

State &state()
{
    static State instance;
    return instance;
}

bool supported(const QString &locale)
{
    return locale == QLatin1String("Auto") || locale == QLatin1String("en_US") || locale == QLatin1String("zh_CN");
}

bool ensureTranslator(QCoreApplication *app)
{
    State &current = state();
    if (!app) return false;
    Q_ASSERT(QThread::currentThread() == app->thread());
    if (current.app != app || !current.translator) {
        initializeI18nResources();
        if (current.app && current.translator && current.installed)
            current.app->removeTranslator(current.translator);
        delete current.translator.data();
        current.app = app;
        current.translator = new DictionaryTranslator(app);
        current.installed = false;
        current.locale = QStringLiteral("en_US");
        current.preference = QStringLiteral("Auto");
        if (!current.translator->readCatalog()) {
            delete current.translator.data();
            current.translator = nullptr;
            return false;
        }
    }
    return true;
}
} // namespace

void initialize(QCoreApplication *app, const QString &overrideLanguage)
{
    if (!ensureTranslator(app)) {
        state().locale = QStringLiteral("en_US");
        QLocale::setDefault(QLocale(QStringLiteral("en_US")));
        return;
    }
    const QString requested = overrideLanguage.isEmpty()
                                  ? QSettings().value(QStringLiteral("ui/language"), QStringLiteral("Auto")).toString()
                                  : overrideLanguage;
    setLanguage(supported(requested) ? requested : QStringLiteral("Auto"), false);
}

QString language() { return state().locale; }
QString setting() { return state().preference; }

QString resolveLanguage(const QString &preference, const QString &systemLocale)
{
    if (!supported(preference)) return {};
    if (preference != QLatin1String("Auto")) return preference;
    const QString locale = systemLocale.isEmpty() ? QLocale::system().name() : systemLocale;
    return locale.startsWith(QLatin1String("zh"), Qt::CaseInsensitive)
               ? QStringLiteral("zh_CN") : QStringLiteral("en_US");
}

bool setLanguage(const QString &locale, bool persist)
{
    if (!supported(locale) || !ensureTranslator(QCoreApplication::instance())) return false;
    State &current = state();
    const QString resolved = resolveLanguage(locale);
    const bool chinese = resolved == QLatin1String("zh_CN");
    // Publish the locale before Qt delivers LanguageChange to widgets.
    current.preference = locale;
    current.locale = resolved;
    QLocale::setDefault(QLocale(resolved));
    if (chinese && !current.installed) {
        if (!current.app->installTranslator(current.translator)) return false;
        current.installed = true;
    } else if (!chinese && current.installed) {
        current.app->removeTranslator(current.translator);
        current.installed = false;
    }
    if (persist) {
        QSettings settings;
        settings.setValue(QStringLiteral("ui/language"), locale);
        settings.sync();
        return settings.status() == QSettings::NoError;
    }
    return true;
}

QString tr(const char *source) { return QCoreApplication::translate("fplc", source); }
} // namespace fplc::I18n
