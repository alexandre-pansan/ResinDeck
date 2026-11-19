#include "i18n.h"

#include "deck.h"
#include "printers/driver.h"
#include "printers/status.h"

#include <QFile>
#include <QHash>
#include <QLocale>
#include <QRegularExpression>
#include <QSettings>

namespace resindeck::ui {

namespace {

QStringList codes;
QHash<QString, QHash<QString, QString>> strings;  // language -> key -> text
QString current = "en";

QString unescape(const QString &s) {
    QString out;
    for (qsizetype i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            const QChar c = s[++i];
            out += c == 'n' ? QChar('\n') : c;
        } else {
            out += s[i];
        }
    }
    return out;
}

}  // namespace

void loadStrings(const QString &webDir) {
    QFile f(webDir + "/i18n.js");
    if (!f.open(QIODevice::ReadOnly)) return;
    // blocks like `  en: {` or `  "pt-BR": {`, entries like `    "key": "text",`
    static const QRegularExpression block(R"re(^  "?([A-Za-z-]+)"?: \{\s*$)re");
    static const QRegularExpression entry(R"re(^\s+"([^"]+)":\s*"((?:[^"\\]|\\.)*)",?\s*$)re");
    QString lang;
    for (const QString &line : QString::fromUtf8(f.readAll()).split('\n')) {
        if (const auto header = block.match(line); header.hasMatch()) {
            lang = header.captured(1);
            if (!codes.contains(lang)) codes << lang;
        } else if (line.startsWith("  }")) {
            lang.clear();
        } else if (!lang.isEmpty()) {
            if (const auto e = entry.match(line); e.hasMatch()) strings[lang].insert(e.captured(1), unescape(e.captured(2)));
        }
    }
    const QString saved = QSettings().value("language").toString();
    if (codes.contains(saved)) {
        current = saved;
        return;
    }
    const QString system = QLocale::system().name().replace('_', '-');
    for (const QString &c : codes)
        if (c.compare(system, Qt::CaseInsensitive) == 0) current = c;
    if (current == "en")
        for (const QString &c : codes)
            if (c.section('-', 0, 0) == system.section('-', 0, 0)) {
                current = c;
                break;
            }
}

QStringList languages() { return codes.isEmpty() ? QStringList{"en"} : codes; }
QString languageName(const QString &code) { return strings.value(code).value("lang.name", code); }
QString language() { return current; }

void setLanguage(const QString &code) {
    if (!codes.contains(code)) return;
    current = code;
    QSettings().setValue("language", code);
}

QString t(const QString &key, const QVariantMap &params) {
    QString s = strings.value(current).value(key);
    if (s.isEmpty()) s = strings.value("en").value(key, key);
    for (auto it = params.begin(); it != params.end(); ++it) s.replace("{" + it.key() + "}", it.value().toString());
    return s;
}

QString errorText(const QString &code, const QVariantMap &params, const QString &fallback) {
    const QString key = "errors." + code;
    if (strings.value("en").contains(key)) return t(key, params);
    return fallback.isEmpty() ? code : fallback;
}

QString errorText(std::exception_ptr error) {
    try {
        std::rethrow_exception(error);
    } catch (const PrinterError &e) {
        return errorText(e.code(), e.params(), e.message());
    } catch (const NetworkError &e) {
        return errorText("comm_failure", {{"detail", QString::fromUtf8(e.what())}});
    } catch (const std::exception &e) {
        return QString::fromUtf8(e.what());
    } catch (...) {
        return {};
    }
}

}  // namespace resindeck::ui
