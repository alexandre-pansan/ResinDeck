// The application's state, shared by the desktop window and the web server: settings, printers,
// the file library, and the devices paired with the PIN. Lives in the main thread.
#pragma once

#include "formats/sliced.h"
#include "net/lan.h"
#include "printers/printer.h"

#include <QFuture>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QString>
#include <QTimer>

#include <map>
#include <memory>
#include <optional>
#include <stdexcept>

namespace resindeck {

// An HTTP-style error that is not meant to be translated (bad path, file not found...).
class HttpError : public std::runtime_error {
public:
    HttpError(int status, const QString &text) : std::runtime_error(text.toStdString()), status_(status) {}
    int status() const { return status_; }

private:
    int status_;
};

// ~/.config/resindeck/settings.json, shared with the Python version.
QString defaultSettingsPath();
QString defaultCacheDir();  // ~/.cache/resindeck
QString newPin();           // six random digits
// A readable name from a User-Agent, like "Android phone · Chrome".
QString deviceName(const QString &userAgent);

class Deck : public QObject {
    Q_OBJECT
public:
    explicit Deck(QString settingsPath = defaultSettingsPath(),
                  std::shared_ptr<lan::Network> network = std::make_shared<lan::Network>(), PrinterTiming timing = {},
                  QString cacheDir = defaultCacheDir(), QObject *parent = nullptr);
    ~Deck() override;

    void save();

    // --- printers ------------------------------------------------------------------------
    QList<Printer *> printers() const;
    Printer *printer(const QString &id) const;  // nullptr if there is none
    Printer &printerOr404(const QString &id) const;
    QJsonArray printerList() const;
    QString addPrinter(const QJsonObject &body);  // returns the new id; throws PrinterError
    void editPrinter(const QString &id, const QJsonObject &body);
    void removePrinter(const QString &id);
    // Validates a printer dialog's body against the old config (for an edit).
    static QJsonObject cleanPrinter(const QJsonObject &body, const QJsonObject &old = {});
    // Searches the network for every known printer family: [{host, mac, driver, driverName, known}].
    QFuture<QJsonArray> scan();

    // --- library -------------------------------------------------------------------------
    QStringList folders() const;
    QStringList setFolders(const QJsonValue &folders);  // throws PrinterError("folder_missing")
    QJsonObject library() const;                        // {folders, files: [{path, name, folder, size, mtime}]}
    // Only files inside the library folders: the canonical path, or HttpError 400/403/404.
    QString resolve(const QString &raw) const;
    // Saves an uploaded file into the first library folder; throws PrinterError.
    QString saveUpload(const QString &name, const QByteArray &data);
    std::shared_ptr<SlicedFile> file(const QString &path);  // cached by path and modification time
    QJsonObject fileInfo(const QString &path);
    bool hasVoxels(const QString &path) const;
    // The 3D view's payload: uint32 meta length (little-endian), JSON meta, packed bits (most
    // significant bit first, z-major). Built once per file and cached on disk; requests for a file
    // that is being built wait for the same job. `done` runs in the main thread with the payload, or
    // with the error.
    using VoxelsDone = std::function<void(const QByteArray &payload, std::exception_ptr error)>;
    void voxels(const QString &path, VoxelsDone done);

    // --- phones and other devices --------------------------------------------------------
    QString pin() const;
    QString renewPin();  // new PIN, and every paired device signs out
    // Checks the PIN for a device at `ip`; returns the session token for its cookie. Throws
    // PrinterError("pin_wrong") or ("pin_wait") after 5 failures, for a minute.
    QString login(const QString &pin, const QString &ip, const QString &userAgent);
    // The paired device with this session token, if any; marks it as seen.
    std::optional<QJsonObject> device(const QString &token);
    void removeDevice(const QString &id);
    QJsonArray deviceList(const QSet<QString> &online) const;

    void requestQuit() { emit quitRequested(); }

signals:
    void printersChanged();  // coalesced: at most every 100 ms
    void devicesChanged();
    void deviceDropped(const QString &id);  // its connections must close; empty for every device
    void voxelProgress(const QString &path, double fraction);
    void quitRequested();

private:
    void startPrinter(const QJsonObject &cfg);
    void notify();
    QString voxelCachePath(const QString &path) const;

    QString settingsPath_;
    QString cacheDir_;
    std::shared_ptr<lan::Network> network_;
    PrinterTiming timing_;
    QJsonObject settings_;
    QStringList order_;  // printer ids in the user's order
    std::map<QString, std::unique_ptr<Printer>> printers_;
    QTimer notifyTimer_;
    struct CachedFile {
        qint64 mtime;
        std::shared_ptr<SlicedFile> file;
    };
    QHash<QString, CachedFile> files_;
    struct VoxelJob {
        QFuture<QByteArray> future;
        QList<VoxelsDone> waiting;
    };
    QHash<QString, VoxelJob> voxelJobs_;
    QHash<QString, std::pair<int, qint64>> loginFailures_;  // ip -> (count, blocked until in ms)
    QHash<QString, double> seen_;                            // device id -> last request (Unix time)
};

}  // namespace resindeck
