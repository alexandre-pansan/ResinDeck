// What the interface shows for a printer, the same for every driver.
#pragma once

#include <QJsonObject>
#include <QString>
#include <QVariantMap>

#include <stdexcept>

namespace resindeck {

// Printer states. Drivers translate whatever their printer reports into one of these.
enum class State { Idle, Printing, Pausing, Paused, Stopping, Stopped, Finishing, Finished, Failed, Unknown };

// "idle", "printing"…: the names the web interface and settings use.
QString stateName(State state);
State stateFromName(const QString &name);  // Unknown for anything else

struct Status {
    State state = State::Idle;
    QString file;        // name of the file being printed, as stored on the printer
    int layer = 0;       // current layer, 1-based; 0 when unknown
    int layers = 0;      // total layers; 0 when unknown
    int remaining = 0;   // seconds left; 0 when unknown
    QString code;        // the printer's own state name, shown when `state` is Unknown
    QJsonObject raw;     // the printer's full reply, for `resindeck raw-status`

    // A new file can be sent and printed.
    bool ready() const;
    // For the web interface (without the raw reply).
    QJsonObject toJson() const;
};

// An error meant for the user. `code` and `params` let the web interface translate it
// (keys "errors.<code>" in web/i18n.js).
class PrinterError : public std::runtime_error {
public:
    PrinterError(const QString &message, QString code = {}, QVariantMap params = {});
    QString message() const { return QString::fromUtf8(what()); }
    const QString &code() const { return code_; }
    const QVariantMap &params() const { return params_; }

private:
    QString code_;
    QVariantMap params_;
};

}  // namespace resindeck
