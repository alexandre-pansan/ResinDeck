#include "config.h"

#include "printers/drivers.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QStringList>

namespace resindeck {

namespace {
// RESINDECK_CONFIG_DIR moves the configuration elsewhere (portable setups, tests: on Windows Qt
// ignores HOME and asks the system for the home folder)
QString configDir() {
    return qEnvironmentVariableIsSet("RESINDECK_CONFIG_DIR") ? qEnvironmentVariable("RESINDECK_CONFIG_DIR")
                                                             : QDir::homePath() + "/.config/resindeck";
}
QString oldConfigDir() { return QFileInfo(configDir()).path() + "/halot-send"; }

QString env(const char *name, const QString &fallback) {
    return qEnvironmentVariableIsSet(name) ? qEnvironmentVariable(name) : fallback;
}
}  // namespace

QString configPath() { return configDir() + "/config"; }

CliConfig loadConfig() {
    QHash<QString, QString> values;
    QFile f(configPath());
    if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        for (const QString &line : QString::fromUtf8(f.readAll()).split('\n')) {
            const qsizetype eq = line.indexOf('=');
            if (eq < 0 || line.trimmed().startsWith('#')) continue;
            values.insert(line.left(eq).trimmed(), line.mid(eq + 1).trimmed());
        }
    }
    return {env("RESINDECK_HOST", values.value("HOST")), env("RESINDECK_PASSWORD", values.value("PASSWORD")),
            env("RESINDECK_DRIVER", values.value("DRIVER", kDefaultDriver))};
}

void migrateConfig() {
    if (!QDir(oldConfigDir()).exists() || QDir(configDir()).exists()) return;
    QDir().mkpath(configDir());
    QDirIterator it(oldConfigDir(), QDir::Files);
    while (it.hasNext()) {
        const QString file = it.next();
        QFile::copy(file, configDir() + "/" + QFileInfo(file).fileName());
    }
}

QString formatStatus(const Status &st) {
    QStringList lines{"State:     " + stateName(st.state) + (st.code.isEmpty() ? "" : " (" + st.code + ")")};
    if (!st.file.isEmpty()) lines << "File:      " + st.file;
    if (st.layers)
        lines << QString("Layer:     %1/%2 (%3%)").arg(st.layer).arg(st.layers).arg(st.layer * 100 / st.layers);
    if (st.remaining)
        lines << QString("Remaining: %1h%2min").arg(st.remaining / 3600).arg(st.remaining % 3600 / 60, 2, 10, QChar('0'));
    return lines.join('\n');
}

}  // namespace resindeck
