// The window's dialogs: a printer's settings, the library folders and phone pairing.
#pragma once

#include "deck.h"
#include "remote/server.h"

#include <QDialog>

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QVBoxLayout;

namespace resindeck::ui {

// Add a printer (printer == nullptr) or change one: type, name, address, password, network search.
class PrinterDialog : public QDialog {
    Q_OBJECT
public:
    PrinterDialog(Deck &deck, Printer *printer, QWidget *parent = nullptr);

private:
    void applyDriver();
    void save();

    Deck &deck_;
    QString id_;
    QJsonObject old_;
    QComboBox *driver_;
    QLabel *models_, *macHint_, *error_, *scanStatus_;
    QLineEdit *name_, *host_, *password_;
    QListWidget *found_;
    QPushButton *scan_;
};

class FoldersDialog : public QDialog {
    Q_OBJECT
public:
    FoldersDialog(Deck &deck, QWidget *parent = nullptr);

private:
    Deck &deck_;
    QListWidget *list_;
    QLabel *error_;
};

// Phone access: QR code and addresses, the PIN, and the paired devices.
class PhoneDialog : public QDialog {
    Q_OBJECT
public:
    PhoneDialog(Deck &deck, Server *server, QWidget *parent = nullptr);

private:
    void refresh();

    Deck &deck_;
    Server *server_;
    QLabel *qr_, *urls_, *pin_;
    QVBoxLayout *devices_;
};

}  // namespace resindeck::ui
