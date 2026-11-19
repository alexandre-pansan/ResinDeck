// resindeck-cli: send sliced files to resin printers over the network and control them.
// The native version of resindeck.py's commands.
//
//   resindeck-cli version
//   resindeck-cli status
//   resindeck-cli send FILE.cxdlpv4 [--print] [-y]
//   resindeck-cli print NAME_ON_PRINTER.cxdlpv4 [-y]
//   resindeck-cli pause | stop
//
// Config: ~/.config/resindeck/config (HOST=..., PASSWORD=..., DRIVER=halot)
#include "config.h"
#include "printers/drivers.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonDocument>
#include <QTextStream>

#include <cstdio>

using namespace resindeck;

namespace {

QTextStream out(stdout);
QTextStream err(stderr);

[[noreturn]] void fail(const QString &message) {
    err << message << Qt::endl;
    std::exit(1);
}

QString json(const QJsonObject &obj, bool indented = false) {
    return QString::fromUtf8(QJsonDocument(obj).toJson(indented ? QJsonDocument::Indented : QJsonDocument::Compact))
        .trimmed();
}

bool confirm(const QString &name, bool yes) {
    if (yes) return true;
    out << "Start printing '" << name << "' now? Check the resin and the build plate. [y/N] " << Qt::flush;
    const QString answer = QTextStream(stdin).readLine().trimmed().toLower();
    return answer == "y" || answer == "yes" || answer == "s" || answer == "sim";
}

int run(const QCommandLineParser &cli) {
    const QStringList args = cli.positionalArguments();
    const QString cmd = args.value(0);
    if (cmd.isEmpty() || cmd == "ui")
        fail("The interface is not in the native version yet. Run start.sh for the web interface.");
    const QStringList known{"version", "status", "raw-status", "send", "print", "pause", "stop"};
    if (!known.contains(cmd)) fail("Unknown command: " + cmd + ". See --help.");
    if ((cmd == "send" || cmd == "print") && args.size() < 2) fail("Missing file name. See --help.");

    migrateConfig();
    const CliConfig cfg = loadConfig();
    const QString host = cli.isSet("host") ? cli.value("host") : cfg.host;
    if (host.isEmpty()) fail("No printer IP. Put HOST=... in " + configPath() + " or use --host");
    const bool yes = cli.isSet("yes");

    try {
        const DriverType type = driverType(cli.isSet("driver") ? cli.value("driver") : cfg.driver);
        const auto d = type.create(host, cfg.password);
        d->connect();
        if (cmd == "version") {
            out << type.info.name << " at " << host << ", firmware "
                << (d->version().isEmpty() ? QString("unknown") : d->version()) << Qt::endl;
        } else if (cmd == "status") {
            out << formatStatus(d->status()) << Qt::endl;
        } else if (cmd == "raw-status") {
            out << json(d->status().raw, true) << Qt::endl;
        } else if (cmd == "send") {
            const QFileInfo file(args[1]);
            if (!file.isFile()) fail("File not found: " + args[1]);
            if (!type.info.extensions.contains("." + file.suffix().toLower()))
                err << "Warning: " << type.info.name << " printers expect " << type.info.extensions.join(", ")
                    << " files." << Qt::endl;
            const Status st = d->status();  // checks the password before uploading
            if (!st.ready()) err << "Warning: the printer is " << stateName(st.state) << " (" << st.code << ")." << Qt::endl;
            out << QString("Uploading %1 (%2 MB) to %3...").arg(file.fileName()).arg(double(file.size()) / 1e6, 0, 'f', 1).arg(host)
                << Qt::endl;
            const QJsonObject r = d->upload(file.filePath(), [](int pct) {
                out << QString("\r  uploading... %1%").arg(pct, 3) << Qt::flush;
            });
            out << Qt::endl << "Upload finished. " << json(r) << Qt::endl;
            if (cli.isSet("print") && confirm(file.fileName(), yes)) out << json(d->start(file.fileName())) << Qt::endl;
        } else if (cmd == "print") {
            if (confirm(args[1], yes)) out << json(d->start(args[1])) << Qt::endl;
        } else if (cmd == "pause") {
            out << json(d->status().state == State::Paused ? d->resume() : d->pause()) << Qt::endl;
        } else if (cmd == "stop") {
            out << json(d->stop()) << Qt::endl;
        }
        d->close();
    } catch (const PrinterError &e) {
        fail(e.message());
    } catch (const NetworkError &e) {
        fail("Could not talk to " + host + ": " + QString::fromUtf8(e.what()));
    }
    return 0;
}

}  // namespace

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("resindeck-cli");
    QCoreApplication::setApplicationVersion(RESINDECK_VERSION);

    QCommandLineParser cli;
    cli.setApplicationDescription(
        "Send files to resin printers over the network and control them.\n\n"
        "Commands:\n"
        "  version                  firmware version (no password needed)\n"
        "  status                   print status\n"
        "  raw-status               raw status reply (debug)\n"
        "  send FILE [--print]      upload a sliced file to the printer\n"
        "  print NAME               print a file that is already on the printer\n"
        "  pause                    pause / resume\n"
        "  stop                     cancel the print\n\n"
        "Config: ~/.config/resindeck/config (HOST=..., PASSWORD=..., DRIVER=halot)");
    cli.addHelpOption();
    cli.addVersionOption();
    cli.addOptions({
        {"host", "Printer IP (default: from config).", "host"},
        {"driver", "Printer type (default: from config, or halot).", "driver"},
        {"print", "send: start printing after the upload."},
        {{"y", "yes"}, "Do not ask for confirmation before printing."},
    });
    cli.addPositionalArgument("command", "version, status, raw-status, send, print, pause or stop.");
    cli.process(app);
    return run(cli);
}
