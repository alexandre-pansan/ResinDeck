// The contract every printer driver follows.
//
// A driver is a subclass of Driver for one printer family, listed in printers/drivers.cpp. The rest
// of ResinDeck (window, web server, command line) only talks to this interface, so supporting a new
// printer family means writing one driver; nothing else changes. printers/halot.cpp is a complete
// example.
//
// Methods block until the printer answers. Each printer runs in its own thread, so a slow printer
// never freezes the window or the server. Methods throw PrinterError for problems the user can fix
// (wrong password, printer busy) and NetworkError when the connection fails: the caller reconnects.
#pragma once

#include "printers/status.h"

#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <functional>
#include <stdexcept>
#include <utility>

namespace resindeck {

// The connection failed, timed out or was closed. Not meant for the user as is: the caller
// shows the printer as offline and reconnects.
class NetworkError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Describes a printer family.
struct DriverInfo {
    QString id;              // short identifier stored in settings.json, e.g. "halot"
    QString name;            // shown in the interface, e.g. "Creality HALOT"
    QString models;          // models known to work, for the interface and docs
    QStringList extensions;  // file types the printer accepts, lower case, e.g. {".cxdlpv4"}
    bool password = true;    // whether the printer needs a password for status and commands
    int passwordMax = 0;     // longest accepted password (0: no limit)

    // Throws PrinterError("password_length") if the printer cannot accept this password.
    void checkPassword(const QString &password) const;
    // For the web interface's printer dialog.
    QJsonObject toJson() const;
};

// One connection to one printer.
class Driver {
public:
    using Progress = std::function<void(int percent)>;

    Driver(QString host, QString password) : host_(std::move(host)), password_(std::move(password)) {}
    virtual ~Driver() = default;
    Driver(const Driver &) = delete;
    Driver &operator=(const Driver &) = delete;

    virtual const DriverInfo &info() const = 0;

    // Open the connection; set version_ if the printer reports it.
    virtual void connect() = 0;
    virtual void close() = 0;
    // Check the printer still answers, without a password (used while none is set).
    virtual void ping() = 0;
    virtual Status status() = 0;
    // Send a file to the printer, calling progress(percent) as it goes. Returns what the printer
    // answered (shown to the user as is).
    virtual QJsonObject upload(const QString &path, const Progress &progress = {}) = 0;
    // Start printing a file already on the printer.
    virtual QJsonObject start(const QString &filename) = 0;
    virtual QJsonObject pause() = 0;
    virtual QJsonObject resume() = 0;
    virtual QJsonObject stop() = 0;
    // Called from the driver's thread, possibly while another call is waiting for the printer (from
    // inside its event loop): make that call fail soon with NetworkError. Must not destroy anything;
    // close() follows. The default does nothing, so waiting calls end at their timeout.
    virtual void abort() {}

    const QString &host() const { return host_; }
    const QString &version() const { return version_; }
    // Throws PrinterError("no_password") if the printer needs a password and none is set.
    void needPassword() const;

protected:
    QString host_;
    QString password_;
    QString version_;  // firmware version, when connect() can tell
};

}  // namespace resindeck
