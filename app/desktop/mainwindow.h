// The main window: printers and the file library on the left; the chosen file in 3D, by layer or as
// its thumbnail on the right, with its print settings and the send buttons.
#pragma once

#include "deck.h"
#include "remote/server.h"
#include "viewer3d.h"
#include "widgets.h"

#include <QFileSystemWatcher>
#include <QMainWindow>
#include <QTimer>

class QComboBox;
class QGridLayout;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QSlider;
class QStackedWidget;
class QTabBar;
class QVBoxLayout;

namespace resindeck::ui {

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow(Deck &deck, Server *server, QWidget *parent = nullptr);
    void openFile(const QString &path);

protected:
    void dragEnterEvent(QDragEnterEvent *e) override;
    void dropEvent(QDropEvent *e) override;

private:
    void buildUi();
    void retranslate();
    void refreshPrinters();
    void refreshLibrary();
    void refreshSendButtons();
    void showLayer(int index);
    void showTab(int index);
    void send(bool print);
    void addFiles(const QStringList &paths);
    void status(const QString &text, bool error = false);

    Deck &deck_;
    Server *server_;
    QFileSystemWatcher watcher_;
    QTimer libraryTimer_;

    // left
    QLabel *printersTitle_, *libraryTitle_, *libraryNote_;
    QPushButton *addPrinter_, *folders_, *phone_, *dropHint_;
    QComboBox *language_;
    QVBoxLayout *printerList_;
    QList<PrinterCard *> cards_;
    QLineEdit *filter_;
    QListWidget *files_;

    // right
    QStackedWidget *stack_;
    QLabel *empty_, *fileName_, *fileSub_;
    QTabBar *tabs_;
    QStackedWidget *views_;
    Viewer3D *viewer_;
    ImageView *layerView_, *thumbView_;
    QLabel *meshing_;
    QSlider *layerSlider_;
    QLabel *layerLabel_, *layerValue_;
    QGridLayout *stats_;
    QComboBox *target_;
    QPushButton *send_, *sendPrint_;

    QString path_;        // the open file
    QJsonObject info_;    // its header values
    QJsonArray layerZ_;
    int layerRequest_ = 0;
};

}  // namespace resindeck::ui
