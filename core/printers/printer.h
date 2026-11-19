// Keeps a connection to one printer through its driver, polls its status and runs commands.
// The C++ version of server.py's Printer.
//
// Each Printer has its own thread, where the driver lives. Everything that talks to the printer runs
// there, one thing at a time, in order. The public methods can be called from any thread: getters
// return a copy of the latest state, and commands return a QFuture.
#pragma once

#include "net/lan.h"
#include "printers/drivers.h"
#include "printers/status.h"

#include <QFuture>
#include <QJsonObject>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QThread>

#include <functional>
#include <memory>
#include <optional>

namespace resindeck {

struct PrinterTiming {
    int pollMs = 3000;        // between status updates
    int retryMs = 5000;       // between connection attempts
    int searchMs = 60000;     // between network searches for a printer that moved
    std::function<double()> clock;  // Unix time in seconds; the system clock when empty
};

class PrinterWorker;

class Printer : public QObject {
    Q_OBJECT
public:
    // `cfg` is the printer's entry in settings.json: id, name, driver, host, password, and the mac
    // and job this class adds. Throws PrinterError("unknown_driver").
    Printer(const QJsonObject &cfg, std::shared_ptr<lan::Network> network = std::make_shared<lan::Network>(),
            PrinterTiming timing = {}, QObject *parent = nullptr);
    ~Printer() override;

    QString id() const;
    QJsonObject config() const;  // to save in settings.json
    const DriverInfo &driverInfo() const { return type_.info; }

    bool online() const;
    std::optional<Status> status() const;
    QString error() const;  // error code, empty when fine
    // For the web interface: the same fields as the Python version's Printer.public().
    QJsonObject toJson() const;

    // start(filename), pause(), resume() or stop(), then a status update.
    QFuture<QJsonObject> command(const QString &action, const QString &argument = {});
    // Upload a file, then start printing it if `start`. Refuses files the printer does not accept and
    // printers that are busy.
    QFuture<QJsonObject> send(const QString &path, bool start = false);

signals:
    void changed();         // state changed: online, status, error, upload progress
    void configChanged();   // config() changed (MAC learned, new address, job): save the settings

private:
    friend class PrinterWorker;

    // Called from the worker thread.
    void setState(const std::function<void()> &update);
    void setConfig(const std::function<void(QJsonObject &)> &update);

    const DriverType type_;
    mutable QMutex mutex_;
    QJsonObject cfg_;
    bool online_ = false;
    QString version_;
    std::optional<Status> status_;
    QString error_;
    QJsonObject upload_;  // {"file", "pct"} while uploading

    QThread thread_;
    PrinterWorker *worker_ = nullptr;
};

}  // namespace resindeck
