#pragma once

#include <QtCore/QString>

class QCoreApplication;

namespace fplc::I18n {

// Without an override, restore ui/language (Auto on a fresh installation).
// An explicit override affects this process only and never writes settings.
void initialize(QCoreApplication *app, const QString &overrideLanguage = {});
QString language();
QString setting(); // Auto, en_US or zh_CN; language() returns the resolved locale.
QString resolveLanguage(const QString &setting, const QString &systemLocale = {});
bool setLanguage(const QString &locale, bool persist = true);

// Non-QObject messages use the same context-independent catalog as QWidget::tr.
QString tr(const char *source);

} // namespace fplc::I18n
