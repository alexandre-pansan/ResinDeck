// ResinDeck, the desktop application: the window, plus the web interface for phones and browsers
// served from the same process.
//
//   resindeck [FILE.cxdlpv4] [--port N] [--screenshot PICTURE.png]
//
// RESINDECK_LAN=0 keeps the web interface reachable from this computer only.
#include "config.h"
#include "deck.h"
#include "i18n.h"
#include "mainwindow.h"
#include "remote/server.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QFile>
#include <QFileInfo>
#include <QIcon>
#include <QMessageBox>
#include <QTimer>

using namespace resindeck;

int main(int argc, char *argv[]) {
#ifdef Q_OS_LINUX
    // Qt's GTK3 theme plugin makes GTK abort on newer GNOME (a removed xsettings key). Qt's built-in
    // GNOME theme does not load GTK; the window has its own style anyway.
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORMTHEME") && qEnvironmentVariable("XDG_CURRENT_DESKTOP").contains("GNOME"))
        qputenv("QT_QPA_PLATFORMTHEME", "gnome");
#endif
    QApplication app(argc, argv);
    QApplication::setApplicationName("ResinDeck");
    QApplication::setOrganizationName("ResinDeck");
    QApplication::setApplicationVersion(RESINDECK_VERSION);
    QApplication::setWindowIcon(QIcon(":/icon.png"));

    QCommandLineParser cli;
    cli.setApplicationDescription("Send files to resin printers over the network, follow and control prints.");
    cli.addHelpOption();
    cli.addVersionOption();
    cli.addOptions({
        {"port", "Port of the web interface for phones and browsers (default 8765).", "port", "8765"},
        {"screenshot", "Save a picture of the window after a few seconds and quit (for docs and tests).", "file"},
    });
    cli.addPositionalArgument("file", "A sliced file to open.", "[file]");
    cli.process(app);

    QFile style(":/style.qss");
    if (style.open(QIODevice::ReadOnly)) app.setStyleSheet(QString::fromUtf8(style.readAll()));

    migrateConfig();
    const QString webDir = Server::findWebDir();
    ui::loadStrings(webDir);

    Deck deck;
    Server server(deck, webDir);
    const bool lan = qEnvironmentVariable("RESINDECK_LAN", "1") != "0";
    const auto port = static_cast<quint16>(cli.value("port").toUInt());
    const bool serving = server.listen(port, lan);

    ui::MainWindow window(deck, serving ? &server : nullptr);
    if (cli.isSet("screenshot")) window.setWindowFlag(Qt::WindowTransparentForInput);  // the mouse must not move the camera
    window.show();
    if (!cli.positionalArguments().isEmpty()) window.openFile(QFileInfo(cli.positionalArguments().first()).absoluteFilePath());
    if (!serving && !cli.isSet("screenshot"))
        QMessageBox::warning(&window, "ResinDeck",
                             QString("Port %1 is in use, probably by another ResinDeck. This window works, but phones "
                                     "cannot connect to it.")
                                 .arg(port));
    QObject::connect(&deck, &Deck::quitRequested, &app, &QApplication::quit);

    if (cli.isSet("screenshot")) {
        const QString file = cli.value("screenshot");
        const int delay = qEnvironmentVariableIsSet("RESINDECK_SCREENSHOT_DELAY") ? qEnvironmentVariableIntValue("RESINDECK_SCREENSHOT_DELAY") : 4000;
        QTimer::singleShot(delay, &window, [&window, file] {
            window.grab().save(file);
            QApplication::quit();
        });
    }
    return QApplication::exec();
}
