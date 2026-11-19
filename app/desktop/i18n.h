// The interface's strings, read from the web interface's i18n.js so the window and the browser use
// the same words and translations: adding a language there translates both.
#pragma once

#include <QStringList>
#include <QVariantMap>

#include <exception>

namespace resindeck::ui {

// Loads web/i18n.js and picks the saved language, or the system's.
void loadStrings(const QString &webDir);
QStringList languages();         // codes in file order: {"en", "pt-BR"}
QString languageName(const QString &code);
QString language();
void setLanguage(const QString &code);  // remembered for the next start

// The string for `key` in the current language (English when missing), with {name} placeholders
// filled from `params`.
QString t(const QString &key, const QVariantMap &params = {});
// A message for an error code, e.g. "printer_busy", or `fallback` when there is no translation.
QString errorText(const QString &code, const QVariantMap &params = {}, const QString &fallback = {});
// A message for an exception from the core (PrinterError, NetworkError, HttpError...).
QString errorText(std::exception_ptr error);

}  // namespace resindeck::ui
