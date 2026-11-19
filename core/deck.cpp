#include "deck.h"

#include "config.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QPointer>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QUuid>
#include <QtConcurrent/QtConcurrentRun>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace resindeck {

namespace {

QString shortId() { return QUuid::createUuid().toString(QUuid::Id128).left(8); }

double unixNow() { return static_cast<double>(QDateTime::currentMSecsSinceEpoch()) / 1000.0; }

QString sha256(const QString &s) { return QCryptographicHash::hash(s.toUtf8(), QCryptographicHash::Sha256).toHex(); }

// Constant-time comparison, so the PIN cannot be guessed from response times.
bool sameSecret(const QByteArray &a, const QByteArray &b) {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (qsizetype i = 0; i < a.size(); ++i) diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    return diff == 0;
}

QJsonObject loadSettings(const QString &path) {
    QJsonObject s;
    QFile f(path);
    if (f.open(QIODevice::ReadOnly)) {
        s = QJsonDocument::fromJson(f.readAll()).object();
    } else {
        s = {{"printers", QJsonArray{}}, {"folders", QJsonArray{}}};
        const CliConfig cli = loadConfig();  // migrate the command line's config
        if (!cli.host.isEmpty())
            s["printers"] = QJsonArray{QJsonObject{{"id", shortId()}, {"name", "HALOT"}, {"driver", kDefaultDriver},
                                                   {"host", cli.host}, {"password", cli.password}}};
    }
    QJsonArray printers;
    for (const QJsonValue &v : s.value("printers").toArray()) {
        QJsonObject p = v.toObject();
        if (p.value("driver").toString().isEmpty()) p["driver"] = kDefaultDriver;  // from before drivers existed
        printers.append(p);
    }
    s["printers"] = printers;
    if (!s.value("folders").isArray()) s["folders"] = QJsonArray{};
    if (s.value("pin").toString().isEmpty()) s["pin"] = newPin();
    // devices that entered the PIN: {"id", "hash" (sha256 of the cookie), "name", "ip", "created"}
    QJsonArray sessions;
    for (const QJsonValue &v : s.value("sessions").toArray())
        if (v.isObject()) sessions.append(v);
    s["sessions"] = sessions;
    return s;
}

}  // namespace

QString defaultSettingsPath() { return QFileInfo(configPath()).path() + "/settings.json"; }
QString defaultCacheDir() { return QDir::homePath() + "/.cache/resindeck"; }

QString newPin() { return QString("%1").arg(QRandomGenerator::system()->bounded(1000000), 6, 10, QChar('0')); }

QString deviceName(const QString &ua) {
    const std::pair<const char *, const char *> systems[] = {{"iPhone", "iPhone"}, {"iPad", "iPad"},
                                                             {"Android", "Android"}, {"Windows", "Windows"},
                                                             {"Mac OS", "Mac"}, {"CrOS", "ChromeOS"},
                                                             {"Linux", "Linux"}};
    const std::pair<const char *, const char *> browsers[] = {
        {"EdgA", "Edge"},       {"Edg/", "Edge"},   {"SamsungBrowser", "Samsung"}, {"Firefox", "Firefox"},
        {"FxiOS", "Firefox"},   {"CriOS", "Chrome"}, {"Chrome", "Chrome"},          {"Safari", "Safari"}};
    QString os, browser;
    for (const auto &[key, name] : systems)
        if (ua.contains(key)) {
            os = name;
            break;
        }
    if (os == "Android") os += ua.contains("Mobile") ? " phone" : " tablet";
    for (const auto &[key, name] : browsers)
        if (ua.contains(key)) {
            browser = name;
            break;
        }
    QStringList parts;
    if (!os.isEmpty()) parts << os;
    if (!browser.isEmpty()) parts << browser;
    return parts.isEmpty() ? QString("Unknown device") : parts.join(" · ");
}

Deck::Deck(QString settingsPath, std::shared_ptr<lan::Network> network, PrinterTiming timing, QString cacheDir,
           QObject *parent)
    : QObject(parent), settingsPath_(std::move(settingsPath)), cacheDir_(std::move(cacheDir)),
      network_(std::move(network)), timing_(std::move(timing)) {
    settings_ = loadSettings(settingsPath_);
    notifyTimer_.setSingleShot(true);
    notifyTimer_.setInterval(100);
    connect(&notifyTimer_, &QTimer::timeout, this, &Deck::printersChanged);
    for (const QJsonValue &v : settings_.value("printers").toArray()) {
        try {
            startPrinter(v.toObject());
        } catch (const PrinterError &e) {
            std::fprintf(stderr, "Skipping printer %s: %s\n", qPrintable(v.toObject().value("name").toString()), e.what());
        }
    }
    save();
}

Deck::~Deck() {
    for (auto &job : voxelJobs_) job.future.waitForFinished();
    printers_.clear();  // stops each printer's thread
}

void Deck::save() {
    QJsonArray list;
    for (const QString &id : order_) list.append(printers_.at(id)->config());
    settings_["printers"] = list;
    QDir().mkpath(QFileInfo(settingsPath_).path());
    QSaveFile f(settingsPath_);
    if (!f.open(QIODevice::WriteOnly)) return;
    f.write(QJsonDocument(settings_).toJson(QJsonDocument::Indented));
    f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);  // it holds printer passwords
    f.commit();
}

// --- printers ----------------------------------------------------------------------------

void Deck::startPrinter(const QJsonObject &cfg) {
    auto p = std::make_unique<Printer>(cfg, network_, timing_);
    connect(p.get(), &Printer::changed, this, &Deck::notify, Qt::QueuedConnection);
    connect(p.get(), &Printer::configChanged, this, &Deck::save, Qt::QueuedConnection);
    const QString id = p->id();
    if (!order_.contains(id)) order_.append(id);
    printers_[id] = std::move(p);
}

void Deck::notify() {
    if (!notifyTimer_.isActive()) notifyTimer_.start();
}

QList<Printer *> Deck::printers() const {
    QList<Printer *> list;
    for (const QString &id : order_) list.append(printers_.at(id).get());
    return list;
}

Printer *Deck::printer(const QString &id) const {
    const auto it = printers_.find(id);
    return it == printers_.end() ? nullptr : it->second.get();
}

Printer &Deck::printerOr404(const QString &id) const {
    Printer *p = printer(id);
    if (!p) throw HttpError(404, "printer not found");
    return *p;
}

QJsonArray Deck::printerList() const {
    QJsonArray list;
    for (Printer *p : printers()) list.append(p->toJson());
    return list;
}

QJsonObject Deck::cleanPrinter(const QJsonObject &body, const QJsonObject &old) {
    QString driverId = body.value("driver").toString();
    if (driverId.isEmpty()) driverId = old.value("driver").toString();
    const DriverType type = driverType(driverId);
    QString name = body.value("name").toString().trimmed();
    if (name.isEmpty()) name = old.value("name").toString();
    if (name.isEmpty()) name = type.info.name;
    name = name.left(40).trimmed();
    const QString host = body.value("host").toString().trimmed();
    if (host.isEmpty()) throw PrinterError("Enter the printer IP address.", "no_host");
    QJsonObject cfg{{"id", old.value("id").toString().isEmpty() ? shortId() : old.value("id").toString()},
                    {"name", name},
                    {"driver", type.info.id},
                    {"host", host},
                    {"password", old.value("password").toString()}};
    if (host == old.value("host").toString())  // a new address may be another printer: learn its MAC again
        for (const char *key : {"mac", "job"})
            if (old.contains(key) && !old.value(key).isNull()) cfg[key] = old.value(key);
    const QJsonValue pw = body.value("password");
    if (pw.isString() && !pw.toString().isEmpty()) {
        type.info.checkPassword(pw.toString());
        cfg["password"] = pw.toString();
    }
    if (body.value("clearPassword").toBool()) cfg["password"] = "";
    return cfg;
}

QString Deck::addPrinter(const QJsonObject &body) {
    const QJsonObject cfg = cleanPrinter(body);
    startPrinter(cfg);
    save();
    notify();
    return cfg.value("id").toString();
}

void Deck::editPrinter(const QString &id, const QJsonObject &body) {
    const QJsonObject cfg = cleanPrinter(body, printerOr404(id).config());
    printers_.erase(id);  // stops the old connection
    startPrinter(cfg);
    save();
    notify();
}

void Deck::removePrinter(const QString &id) {
    printerOr404(id);
    printers_.erase(id);
    order_.removeAll(id);
    save();
    notify();
}

QFuture<QJsonArray> Deck::scan() {
    QSet<QString> knownHosts, knownMacs;
    for (Printer *p : printers()) {
        const QJsonObject c = p->config();
        knownHosts << c.value("host").toString();
        if (!c.value("mac").toString().isEmpty()) knownMacs << c.value("mac").toString();
    }
    auto net = network_;
    return QtConcurrent::run([net, knownHosts, knownMacs] {
        const auto found = lan::sweep(driverTypes(), net->hosts());
        const auto table = net->neighbors();
        QJsonArray list;
        for (const auto &[driver, host] : found) {
            const QString mac = table.value(host);
            list.append(QJsonObject{{"host", host},
                                    {"mac", mac.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(mac)},
                                    {"driver", driver},
                                    {"driverName", driverType(driver).info.name},
                                    {"known", knownHosts.contains(host) || (!mac.isEmpty() && knownMacs.contains(mac))}});
        }
        return list;
    });
}

// --- library -----------------------------------------------------------------------------

QStringList Deck::folders() const {
    QStringList list;
    for (const QJsonValue &v : settings_.value("folders").toArray()) list << v.toString();
    return list;
}

QStringList Deck::setFolders(const QJsonValue &raw) {
    if (!raw.isArray()) throw PrinterError("Invalid request.", "bad_request");
    QStringList list;
    for (const QJsonValue &v : raw.toArray()) {
        QString f = v.toString().trimmed();
        if (f.startsWith("~")) f = QDir::homePath() + f.mid(1);
        const QFileInfo info(f);
        if (!info.isDir()) throw PrinterError("Folder does not exist: " + f, "folder_missing", {{"folder", f}});
        const QString canonical = info.canonicalFilePath();
        if (!list.contains(canonical)) list << canonical;
    }
    settings_["folders"] = QJsonArray::fromStringList(list);
    save();
    return list;
}

QJsonObject Deck::library() const {
    QJsonArray files;
    QList<QJsonObject> all;
    for (const QString &folder : folders()) {
        QDirIterator it(folder, QDir::Files);
        while (it.hasNext()) {
            const QFileInfo f(it.next());
            if (!isSlicedFile(f.fileName())) continue;
            all.append({{"path", f.filePath()},
                        {"name", f.fileName()},
                        {"folder", folder},
                        {"size", f.size()},
                        {"mtime", static_cast<double>(f.lastModified().toMSecsSinceEpoch()) / 1000.0}});
        }
    }
    std::sort(all.begin(), all.end(),
              [](const QJsonObject &a, const QJsonObject &b) { return a.value("mtime").toDouble() > b.value("mtime").toDouble(); });
    for (const QJsonObject &f : all) files.append(f);
    return {{"folders", QJsonArray::fromStringList(folders())}, {"files", files}};
}

QString Deck::resolve(const QString &raw) const {
    if (raw.isEmpty()) throw HttpError(400, "missing path");
    const QFileInfo info(raw);
    // the canonical path follows symbolic links, so a link inside a folder cannot point outside it
    const QString path = info.exists() ? info.canonicalFilePath() : QDir::cleanPath(info.absoluteFilePath());
    for (const QString &folder : folders()) {
        const QString root = QFileInfo(folder).canonicalFilePath();
        if (!root.isEmpty() && path.startsWith(root + "/")) {
            if (!QFileInfo(path).isFile()) throw HttpError(404, "file not found");
            return path;
        }
    }
    throw HttpError(403, "file is outside the library folders");
}

QString Deck::saveUpload(const QString &rawName, const QByteArray &data) {
    if (folders().isEmpty()) throw PrinterError("Set up a library folder first.", "no_folder");
    const QString name = QFileInfo(rawName).fileName();
    if (name.isEmpty() || !isSlicedFile(name))
        throw PrinterError("Only .cxdlpv4 files: " + name, "only_cxdlp", {{"name", name}});
    const QString dest = folders().first() + "/" + name;
    QSaveFile f(dest);
    if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size() || !f.commit())
        throw PrinterError("Could not save " + name + ": " + f.errorString(), "bad_file", {{"detail", f.errorString()}});
    return dest;
}

std::shared_ptr<SlicedFile> Deck::file(const QString &path) {
    const qint64 mtime = QFileInfo(path).lastModified().toMSecsSinceEpoch();
    const auto it = files_.constFind(path);
    if (it != files_.cend() && it->mtime == mtime) return it->file;
    std::shared_ptr<SlicedFile> f;
    try {
        f = loadSliced(path);
    } catch (const FormatError &e) {
        throw PrinterError(QString::fromUtf8(e.what()), "bad_file", {{"detail", QString::fromUtf8(e.what())}});
    }
    if (files_.size() >= 32) files_.clear();
    files_.insert(path, {mtime, f});
    return f;
}

QJsonObject Deck::fileInfo(const QString &path) {
    const auto f = file(path);
    QJsonObject info = f->info();
    info["hasVoxels"] = hasVoxels(path);
    QJsonArray layers;
    for (int i = 0; i < f->layerCount(); ++i) layers.append(std::round(f->layerZ(i) * 1e4) / 1e4);
    info["layers"] = layers;
    return info;
}

QString Deck::voxelCachePath(const QString &path) const {
    const QFileInfo f(path);
    const QString key = QString("%1|%2|%3").arg(path).arg(f.lastModified().toMSecsSinceEpoch()).arg(f.size());
    return cacheDir_ + "/vox2-" + QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha1).toHex().left(16) + ".bin";
}

bool Deck::hasVoxels(const QString &path) const { return QFileInfo::exists(voxelCachePath(path)); }

void Deck::voxels(const QString &path, VoxelsDone done) {
    if (const auto it = voxelJobs_.find(path); it != voxelJobs_.end()) {
        it->waiting.append(std::move(done));
        return;
    }
    const QString cache = voxelCachePath(path);
    std::shared_ptr<SlicedFile> f;
    try {
        f = file(path);
    } catch (...) {
        done({}, std::current_exception());
        return;
    }
    QPointer<Deck> self(this);
    QFuture<QByteArray> job = QtConcurrent::run([f, cache, path, self] {
        QFile cached(cache);
        if (cached.open(QIODevice::ReadOnly)) return cached.readAll();
        QElapsedTimer last;
        last.start();
        const Voxels v = f->voxels(6, [&](double fraction) {
            if (last.elapsed() < 300) return;
            last.restart();
            QMetaObject::invokeMethod(qApp, [self, path, fraction] {
                if (self) emit self->voxelProgress(path, fraction);
            });
        });
        QJsonObject meta;
        if (!v.empty())
            meta = {{"shape", QJsonArray{v.shape[0], v.shape[1], v.shape[2]}},  // z, y, x
                    {"voxel", QJsonArray{v.voxel[0], v.voxel[1], v.voxel[2]}},  // x, y, z in mm
                    {"origin", QJsonArray{v.origin[0], v.origin[1], v.origin[2]}},
                    {"layersPerVoxel", v.layersPerVoxel},
                    {"bed", QJsonArray{v.bed[0], v.bed[1], v.bed[2]}}};
        const QByteArray metaBytes = QJsonDocument(meta).toJson(QJsonDocument::Compact);
        QByteArray payload(4, '\0');
        qToLittleEndian(static_cast<quint32>(metaBytes.size()), payload.data());
        payload += metaBytes;
        QByteArray bits((static_cast<qsizetype>(v.cells.size()) + 7) / 8, '\0');
        for (std::size_t i = 0; i < v.cells.size(); ++i)
            if (v.cells[i]) bits[static_cast<qsizetype>(i / 8)] = static_cast<char>(bits[static_cast<qsizetype>(i / 8)] | (0x80 >> (i % 8)));
        payload += bits;
        QDir().mkpath(QFileInfo(cache).path());
        QSaveFile out(cache);
        if (out.open(QIODevice::WriteOnly)) {
            out.write(payload);
            out.commit();
        }
        return payload;
    });
    voxelJobs_.insert(path, {job, {std::move(done)}});
    // the one continuation this future may have: it answers everyone who waited for this file
    job.then(this, [this, path](QFuture<QByteArray> finished) {
        const VoxelJob ended = voxelJobs_.take(path);
        QByteArray payload;
        std::exception_ptr error;
        try {
            payload = finished.result();
        } catch (...) {
            error = std::current_exception();
        }
        for (const VoxelsDone &d : ended.waiting) d(payload, error);
    });
}

// --- devices -----------------------------------------------------------------------------

QString Deck::pin() const { return settings_.value("pin").toString(); }

QString Deck::renewPin() {
    settings_["pin"] = newPin();
    settings_["sessions"] = QJsonArray{};
    seen_.clear();
    save();
    emit deviceDropped({});
    emit devicesChanged();
    return pin();
}

QString Deck::login(const QString &pinTyped, const QString &ip, const QString &userAgent) {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    auto [count, until] = loginFailures_.value(ip, {0, 0});
    if (now < until) throw PrinterError("Too many attempts. Wait a minute.", "pin_wait");
    if (!sameSecret(pinTyped.trimmed().toUtf8(), pin().toUtf8())) {
        ++count;
        loginFailures_.insert(ip, {count, count >= 5 ? now + 60000 : 0});
        throw PrinterError("Wrong PIN.", "pin_wrong");
    }
    loginFailures_.remove(ip);
    QByteArray raw(32, '\0');
    QRandomGenerator::system()->fillRange(reinterpret_cast<quint32 *>(raw.data()), 8);
    const QString token = QString::fromLatin1(raw.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
    QJsonArray sessions = settings_.value("sessions").toArray();
    while (sessions.size() >= 50) sessions.removeFirst();
    sessions.append(QJsonObject{{"id", shortId()}, {"hash", sha256(token)}, {"name", deviceName(userAgent)},
                                {"ip", ip}, {"created", unixNow()}});
    settings_["sessions"] = sessions;
    save();
    emit devicesChanged();
    return token;
}

std::optional<QJsonObject> Deck::device(const QString &token) {
    if (token.isEmpty()) return std::nullopt;
    const QString h = sha256(token);
    for (const QJsonValue &v : settings_.value("sessions").toArray())
        if (v.toObject().value("hash").toString() == h) {
            seen_.insert(v.toObject().value("id").toString(), unixNow());
            return v.toObject();
        }
    return std::nullopt;
}

void Deck::removeDevice(const QString &id) {
    QJsonArray kept;
    for (const QJsonValue &v : settings_.value("sessions").toArray())
        if (v.toObject().value("id").toString() != id) kept.append(v);
    settings_["sessions"] = kept;
    seen_.remove(id);
    save();
    emit deviceDropped(id);
    emit devicesChanged();
}

QJsonArray Deck::deviceList(const QSet<QString> &online) const {
    QJsonArray list;
    const QJsonArray sessions = settings_.value("sessions").toArray();
    for (qsizetype i = sessions.size() - 1; i >= 0; --i) {  // newest first
        const QJsonObject d = sessions.at(i).toObject();
        const QString id = d.value("id").toString();
        list.append(QJsonObject{{"id", id},
                                {"name", d.value("name")},
                                {"ip", d.value("ip")},
                                {"created", d.value("created")},
                                {"seen", seen_.contains(id) ? QJsonValue(seen_.value(id)) : QJsonValue(QJsonValue::Null)},
                                {"online", online.contains(id)}});
    }
    return list;
}

}  // namespace resindeck
