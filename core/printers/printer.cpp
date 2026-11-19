#include "printers/printer.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QPromise>
#include <QTimer>

#include <atomic>
#include <cstdio>
#include <deque>

namespace resindeck {

// Lives in the printer's thread. Driver calls block (and run nested event loops while they wait), so
// every operation goes through one queue and runs to the end before the next one starts.
class PrinterWorker : public QObject {
public:
    PrinterWorker(Printer *printer, DriverType type, std::shared_ptr<lan::Network> network, PrinterTiming timing)
        : p_(printer), type_(std::move(type)), net_(std::move(network)), timing_(std::move(timing)) {
        pollTimer_.setInterval(timing_.pollMs);
        retryTimer_.setSingleShot(true);
        QObject::connect(&pollTimer_, &QTimer::timeout, this, [this] { schedulePoll(); });
        QObject::connect(&retryTimer_, &QTimer::timeout, this, [this] { enqueue([this] { tryConnect(); }); });
        sinceStart_.start();
    }

    void enqueue(std::function<void()> op) {
        queue_.push_back(std::move(op));
        pump();
    }

    // May run inside a driver call's nested event loop: then the connection is closed when that call
    // returns (see pump), never under it.
    void stop() {
        stopping_ = true;
        pollTimer_.stop();
        retryTimer_.stop();
        queue_.clear();
        if (!running_) closeConnection();
        else if (conn_) conn_->abort();  // the waiting call fails now; pump closes the connection after it
    }

    void tryConnect() {
        const QJsonObject cfg = p_->config();
        try {
            auto d = type_.create(cfg.value("host").toString(), cfg.value("password").toString());
            d->connect();
            if (!checkMac()) throw NetworkError("another device answers at this address");
            conn_ = std::move(d);
            failures_ = 0;
            p_->setState([&] {
                p_->online_ = true;
                p_->version_ = conn_->version();
                p_->error_.clear();
            });
            pollTimer_.start();
            poll();
        } catch (const std::exception &) {
            disconnected();
        }
    }

    void poll() {
        if (!conn_) return;
        try {
            if (type_.info.password && p_->config().value("password").toString().isEmpty()) {
                conn_->ping();  // keeps checking it is online
                p_->setState([&] {
                    p_->status_.reset();
                    p_->error_ = "no_password";
                });
            } else {
                try {
                    const Status st = conn_->status();
                    p_->setState([&] {
                        p_->status_ = st;
                        p_->error_.clear();
                    });
                } catch (const PrinterError &e) {
                    p_->setState([&] {
                        p_->status_.reset();
                        p_->error_ = e.code().isEmpty() ? e.message() : e.code();
                    });
                }
            }
            trackJob();
        } catch (const NetworkError &) {
            disconnected();
        }
    }

    // A driver command; the result or the error goes to the promise.
    void run(const std::shared_ptr<QPromise<QJsonObject>> &promise, const std::function<QJsonObject()> &op) {
        promise->start();
        try {
            if (!conn_) throw PrinterError("The printer is offline.", "offline");
            promise->addResult(op());
        } catch (const NetworkError &) {
            promise->setException(std::current_exception());
            disconnected();
        } catch (...) {
            promise->setException(std::current_exception());
        }
        promise->finish();
    }

    QJsonObject command(const QString &action, const QString &argument) {
        conn_->needPassword();
        QJsonObject r;
        if (action == "start") r = conn_->start(argument);
        else if (action == "pause") r = conn_->pause();
        else if (action == "resume") r = conn_->resume();
        else if (action == "stop") r = conn_->stop();
        else throw PrinterError("Unknown command: " + action, "bad_request");
        poll();
        return r;
    }

    QJsonObject send(const QString &path, bool start) {
        const QFileInfo file(path);
        const QString ext = "." + file.suffix().toLower();
        if (!type_.info.extensions.contains(ext))
            throw PrinterError(QString("%1 printers do not accept %2 files.").arg(type_.info.name, ext), "file_not_supported",
                               {{"printer", type_.info.name}, {"ext", ext}});
        if (const auto st = p_->status(); st && !st->ready()) {
            const QString state = st->code.isEmpty() ? stateName(st->state) : st->code;
            throw PrinterError(QString("The printer is busy (%1).").arg(state), "printer_busy", {{"state", state}});
        }
        conn_->needPassword();
        auto progress = [&](int pct) { p_->setState([&] { p_->upload_ = {{"file", file.fileName()}, {"pct", pct}}; }); };
        progress(0);
        QJsonObject result;
        try {
            result = conn_->upload(path, progress);
        } catch (...) {
            p_->setState([&] { p_->upload_ = {}; });
            throw;
        }
        p_->setState([&] { p_->upload_ = {}; });
        if (start) result.insert("start", conn_->start(file.fileName()));
        return result;
    }

private:
    void pump() {
        if (running_) return;  // a driver call is waiting: run the rest after it
        running_ = true;
        while (!queue_.empty() && !stopping_) {
            auto op = std::move(queue_.front());
            queue_.pop_front();
            op();
        }
        running_ = false;
        if (stopping_) closeConnection();
    }

    void schedulePoll() {
        if (pollQueued_) return;
        pollQueued_ = true;
        enqueue([this] {
            pollQueued_ = false;
            poll();
        });
    }

    void closeConnection() {
        if (!conn_) return;
        try {
            conn_->close();
        } catch (const std::exception &) {
        }
        conn_.reset();
    }

    void disconnected() {
        closeConnection();
        pollTimer_.stop();
        p_->setState([&] {
            p_->online_ = false;
            p_->status_.reset();
            p_->error_ = "no_connection";
        });
        if (stopping_) return;
        if (++failures_ >= 2 && relocate()) {
            enqueue([this] { tryConnect(); });  // try the new address right away
            return;
        }
        retryTimer_.start(timing_.retryMs);
    }

    // DHCP may give the printer another IP address: remember its MAC address, which does not change,
    // and look for it on the network when the printer stops answering at the old address.

    // Learn the MAC address on the first connection; false if a different device answers.
    bool checkMac() {
        const QJsonObject cfg = p_->config();
        const QString mac = net_->macOf(cfg.value("host").toString());
        if (mac.isEmpty()) return true;  // not on this network segment, or the system does not say: trust it
        if (cfg.value("mac").toString().isEmpty()) {
            p_->setConfig([&](QJsonObject &c) { c.insert("mac", mac); });
            return true;
        }
        return mac == cfg.value("mac").toString();
    }

    // Find the printer's MAC address at another IP. True if the address changed.
    bool relocate() {
        const QJsonObject cfg = p_->config();
        const QString mac = cfg.value("mac").toString();
        const qint64 now = sinceStart_.elapsed();
        if (mac.isEmpty() || (lastSearch_ >= 0 && now - lastSearch_ < timing_.searchMs)) return false;
        lastSearch_ = now;
        const QString host = cfg.value("host").toString();
        const QString ip = net_->findByMac(mac, type_, lan::hostPart(host));
        if (ip.isEmpty()) return false;
        // keep a ":port" suffix (used by tests and by printers behind port forwarding)
        const QString port = host.mid(lan::hostPart(host).size());
        std::fprintf(stderr, "%s: %s moved from %s to %s\n", qPrintable(cfg.value("name").toString()), qPrintable(mac),
                     qPrintable(host), qPrintable(ip + port));
        p_->setConfig([&](QJsonObject &c) { c.insert("host", ip + port); });
        return true;
    }

    double now() const {
        return timing_.clock ? timing_.clock() : static_cast<double>(QDateTime::currentMSecsSinceEpoch()) / 1000.0;
    }

    // Printers report how long is left, not when the print began: note when each job starts and ends.
    // Saved as cfg["job"] = {"file", "started", "approx", "ended"}: Unix times. `started` is null when
    // unknown; `ended` is null while printing, and false once over without a known finish time
    // (stopped, failed, or finished while ResinDeck was closed).
    void trackJob() {
        const auto st = p_->status();
        if (!st) return;
        const QJsonObject job = p_->config().value("job").toObject();
        const double t = now();
        if (!st->ready()) {
            if (!job.isEmpty() && job.value("file").toString() == st->file && job.value("ended").isNull()) {
                seenActive_ = true;
                return;
            }
            QJsonValue started = t;
            bool approx = false;
            if (st->layer > 1) {  // already printing when first seen: work the start out from the progress
                approx = true;
                const int done = st->layer - 1;
                started = st->remaining && st->layers > done
                              ? QJsonValue(t - double(st->remaining) * done / (st->layers - done))
                              : QJsonValue(QJsonValue::Null);
            }
            p_->setConfig([&](QJsonObject &c) {
                c.insert("job", QJsonObject{{"file", st->file}, {"started", started}, {"approx", approx},
                                            {"ended", QJsonValue::Null}});
            });
            seenActive_ = true;
        } else if (!job.isEmpty() && job.value("ended").isNull()) {
            QJsonObject ended = job;
            ended.insert("ended", seenActive_ && st->state == State::Finished ? QJsonValue(t) : QJsonValue(false));
            seenActive_ = false;
            p_->setConfig([&](QJsonObject &c) { c.insert("job", ended); });
        }
    }

    Printer *p_;
    const DriverType type_;
    std::shared_ptr<lan::Network> net_;
    PrinterTiming timing_;
    std::unique_ptr<Driver> conn_;
    std::deque<std::function<void()>> queue_;
    bool running_ = false;
    bool pollQueued_ = false;
    bool stopping_ = false;
    int failures_ = 0;
    qint64 lastSearch_ = -1;
    bool seenActive_ = false;  // saw the current job printing since ResinDeck started
    QElapsedTimer sinceStart_;
    QTimer pollTimer_{this};
    QTimer retryTimer_{this};
};

// --- Printer ---------------------------------------------------------------------------

Printer::Printer(const QJsonObject &cfg, std::shared_ptr<lan::Network> network, PrinterTiming timing, QObject *parent)
    : QObject(parent), type_(driverType(cfg.value("driver").toString())), cfg_(cfg) {
    cfg_.insert("driver", type_.info.id);
    worker_ = new PrinterWorker(this, type_, std::move(network), std::move(timing));
    worker_->moveToThread(&thread_);
    thread_.setObjectName("printer " + cfg.value("id").toString());
    thread_.start();
    QMetaObject::invokeMethod(worker_, [w = worker_] { w->enqueue([w] { w->tryConnect(); }); });
}

Printer::~Printer() {
    // Waits for a driver call in progress, which is bounded by the driver's timeouts.
    QMetaObject::invokeMethod(worker_, [w = worker_] { w->stop(); }, Qt::BlockingQueuedConnection);
    thread_.quit();
    thread_.wait();
    delete worker_;
}

QString Printer::id() const { return config().value("id").toString(); }

QJsonObject Printer::config() const {
    QMutexLocker lock(&mutex_);
    return cfg_;
}

bool Printer::online() const {
    QMutexLocker lock(&mutex_);
    return online_;
}

std::optional<Status> Printer::status() const {
    QMutexLocker lock(&mutex_);
    return status_;
}

QString Printer::error() const {
    QMutexLocker lock(&mutex_);
    return error_;
}

QJsonObject Printer::toJson() const {
    QMutexLocker lock(&mutex_);
    auto orNull = [](const QString &s) { return s.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(s); };
    return {
        {"id", cfg_.value("id")},
        {"name", cfg_.value("name")},
        {"host", cfg_.value("host")},
        {"mac", orNull(cfg_.value("mac").toString())},
        {"driver", type_.info.id},
        {"driverName", type_.info.name},
        {"extensions", QJsonArray::fromStringList(type_.info.extensions)},
        {"hasPassword", !cfg_.value("password").toString().isEmpty()},
        {"needsPassword", type_.info.password},
        {"online", online_},
        {"version", orNull(version_)},
        {"status", status_ ? QJsonValue(status_->toJson()) : QJsonValue(QJsonValue::Null)},
        {"error", orNull(error_)},
        {"upload", upload_.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(upload_)},
        {"job", cfg_.contains("job") ? cfg_.value("job") : QJsonValue(QJsonValue::Null)},
    };
}

QFuture<QJsonObject> Printer::command(const QString &action, const QString &argument) {
    auto promise = std::make_shared<QPromise<QJsonObject>>();
    QFuture<QJsonObject> future = promise->future();
    QMetaObject::invokeMethod(worker_, [w = worker_, promise, action, argument] {
        w->enqueue([w, promise, action, argument] { w->run(promise, [=] { return w->command(action, argument); }); });
    });
    return future;
}

QFuture<QJsonObject> Printer::send(const QString &path, bool start) {
    auto promise = std::make_shared<QPromise<QJsonObject>>();
    QFuture<QJsonObject> future = promise->future();
    QMetaObject::invokeMethod(worker_, [w = worker_, promise, path, start] {
        w->enqueue([w, promise, path, start] { w->run(promise, [=] { return w->send(path, start); }); });
    });
    return future;
}

void Printer::setState(const std::function<void()> &update) {
    {
        QMutexLocker lock(&mutex_);
        update();
    }
    emit changed();
}

void Printer::setConfig(const std::function<void(QJsonObject &)> &update) {
    {
        QMutexLocker lock(&mutex_);
        update(cfg_);
    }
    emit configChanged();
    emit changed();
}

}  // namespace resindeck
