#include "dialogs.h"

#include "i18n.h"
#include "remote/qr.h"

#include <QComboBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPixmap>
#include <QPushButton>
#include <QVBoxLayout>

namespace resindeck::ui {

// --- PrinterDialog -----------------------------------------------------------------------

PrinterDialog::PrinterDialog(Deck &deck, Printer *printer, QWidget *parent) : QDialog(parent), deck_(deck) {
    if (printer) {
        old_ = printer->config();
        id_ = old_.value("id").toString();
    }
    setWindowTitle(printer ? t("printerDialog.edit") : t("printerDialog.add"));
    setMinimumWidth(460);
    auto *col = new QVBoxLayout(this);
    auto *form = new QFormLayout;
    driver_ = new QComboBox;
    for (const DriverType &type : driverTypes()) driver_->addItem(type.info.name, type.info.id);
    driver_->setCurrentIndex(std::max(0, driver_->findData(old_.value("driver").toString())));
    models_ = new QLabel;
    models_->setObjectName("muted");
    models_->setWordWrap(true);
    name_ = new QLineEdit(old_.value("name").toString());
    host_ = new QLineEdit(old_.value("host").toString());
    host_->setPlaceholderText("192.168.1.50");
    password_ = new QLineEdit;
    password_->setEchoMode(QLineEdit::Password);
    form->addRow(t("printerDialog.driver"), driver_);
    form->addRow(QString(), models_);
    form->addRow(t("printerDialog.name"), name_);
    form->addRow(t("printerDialog.host"), host_);
    form->addRow(t("printerDialog.password"), password_);
    col->addLayout(form);

    macHint_ = new QLabel;
    macHint_->setObjectName("muted");
    macHint_->setWordWrap(true);
    if (old_.value("mac").isString()) macHint_->setText(t("printerDialog.macHint", {{"mac", old_.value("mac").toString()}}));
    macHint_->setVisible(!macHint_->text().isEmpty());
    col->addWidget(macHint_);

    auto *scanRow = new QHBoxLayout;
    scan_ = new QPushButton(t("printerDialog.scan"));
    scanStatus_ = new QLabel;
    scanStatus_->setObjectName("muted");
    scanRow->addWidget(scan_);
    scanRow->addWidget(scanStatus_, 1);
    col->addLayout(scanRow);
    found_ = new QListWidget;
    found_->setVisible(false);
    found_->setMaximumHeight(120);
    col->addWidget(found_);

    error_ = new QLabel;
    error_->setObjectName("error");
    error_->setWordWrap(true);
    error_->setVisible(false);
    col->addWidget(error_);

    auto *buttons = new QDialogButtonBox;
    buttons->addButton(t("dialog.save"), QDialogButtonBox::AcceptRole);
    buttons->addButton(t("dialog.cancel"), QDialogButtonBox::RejectRole);
    if (printer) {
        QPushButton *remove = buttons->addButton(t("printerDialog.remove"), QDialogButtonBox::DestructiveRole);
        remove->setObjectName("danger");
        connect(remove, &QPushButton::clicked, this, [this] {
            if (QMessageBox::warning(this, t("remove.title"), t("remove.text", {{"name", old_.value("name").toString()}, {"host", old_.value("host").toString()}}),
                                     QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
                return;
            deck_.removePrinter(id_);
            accept();
        });
    }
    col->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &PrinterDialog::save);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(driver_, &QComboBox::currentIndexChanged, this, &PrinterDialog::applyDriver);

    connect(scan_, &QPushButton::clicked, this, [this] {
        scan_->setEnabled(false);
        scanStatus_->setText(t("printerDialog.scanning"));
        found_->clear();
        auto *watcher = new QFutureWatcher<QJsonArray>(this);
        connect(watcher, &QFutureWatcher<QJsonArray>::finished, this, [this, watcher] {
            watcher->deleteLater();
            scan_->setEnabled(true);
            const QJsonArray list = watcher->result();
            scanStatus_->setText(list.isEmpty() ? t("printerDialog.scanNone") : QString());
            for (const QJsonValue &v : list) {
                const QJsonObject f = v.toObject();
                QString text = f.value("host").toString() + "  ·  " + f.value("driverName").toString();
                if (f.value("known").toBool()) text += "  (" + t("printerDialog.scanKnown") + ")";
                auto *item = new QListWidgetItem(text, found_);
                item->setData(Qt::UserRole, f);
            }
            found_->setVisible(!list.isEmpty());
        });
        watcher->setFuture(deck_.scan());
    });
    connect(found_, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
        const QJsonObject f = item->data(Qt::UserRole).toJsonObject();
        host_->setText(f.value("host").toString());
        driver_->setCurrentIndex(std::max(0, driver_->findData(f.value("driver").toString())));
    });
    applyDriver();
}

void PrinterDialog::applyDriver() {
    const DriverType type = driverType(driver_->currentData().toString());
    models_->setText(type.info.models);
    password_->setEnabled(type.info.password);
    password_->setMaxLength(type.info.passwordMax ? type.info.passwordMax : 32767);
    password_->setPlaceholderText(!old_.value("password").toString().isEmpty()
                                      ? t("printerDialog.passwordKeep")
                                      : t("printerDialog.passwordPlaceholder", {{"max", type.info.passwordMax}}));
}

void PrinterDialog::save() {
    QJsonObject body{{"driver", driver_->currentData().toString()}, {"name", name_->text()}, {"host", host_->text()}};
    if (!password_->text().isEmpty()) body.insert("password", password_->text());
    try {
        if (id_.isEmpty())
            deck_.addPrinter(body);
        else
            deck_.editPrinter(id_, body);
        accept();
    } catch (...) {
        error_->setText(errorText(std::current_exception()));
        error_->setVisible(true);
    }
}

// --- FoldersDialog -----------------------------------------------------------------------

FoldersDialog::FoldersDialog(Deck &deck, QWidget *parent) : QDialog(parent), deck_(deck) {
    setWindowTitle(t("library.foldersTitle"));
    setMinimumWidth(520);
    auto *col = new QVBoxLayout(this);
    auto *hint = new QLabel(t("foldersDialog.hint"));
    hint->setObjectName("muted");
    hint->setWordWrap(true);
    col->addWidget(hint);
    list_ = new QListWidget;
    list_->addItems(deck_.folders());
    col->addWidget(list_);
    auto *row = new QHBoxLayout;
    auto *add = new QPushButton("+");
    auto *remove = new QPushButton("−");
    row->addWidget(add);
    row->addWidget(remove);
    row->addStretch();
    col->addLayout(row);
    error_ = new QLabel;
    error_->setObjectName("error");
    error_->setVisible(false);
    col->addWidget(error_);
    auto *buttons = new QDialogButtonBox;
    buttons->addButton(t("dialog.save"), QDialogButtonBox::AcceptRole);
    buttons->addButton(t("dialog.cancel"), QDialogButtonBox::RejectRole);
    col->addWidget(buttons);

    connect(add, &QPushButton::clicked, this, [this] {
        const QString dir = QFileDialog::getExistingDirectory(this, t("library.foldersTitle"));
        if (!dir.isEmpty() && list_->findItems(dir, Qt::MatchExactly).isEmpty()) list_->addItem(dir);
    });
    connect(remove, &QPushButton::clicked, this, [this] { delete list_->currentItem(); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        QJsonArray folders;
        for (int i = 0; i < list_->count(); ++i) folders.append(list_->item(i)->text());
        try {
            deck_.setFolders(folders);
            accept();
        } catch (...) {
            error_->setText(errorText(std::current_exception()));
            error_->setVisible(true);
        }
    });
}

// --- PhoneDialog -------------------------------------------------------------------------

PhoneDialog::PhoneDialog(Deck &deck, Server *server, QWidget *parent) : QDialog(parent), deck_(deck), server_(server) {
    setWindowTitle(t("remote.title"));
    setMinimumWidth(420);
    auto *col = new QVBoxLayout(this);
    auto *hint = new QLabel(t("remote.hint"));
    hint->setWordWrap(true);
    col->addWidget(hint);
    qr_ = new QLabel;
    qr_->setAlignment(Qt::AlignCenter);
    col->addWidget(qr_);
    urls_ = new QLabel;
    urls_->setAlignment(Qt::AlignCenter);
    urls_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    col->addWidget(urls_);

    auto *pinRow = new QHBoxLayout;
    auto *pinLabel = new QLabel(t("remote.pin"));
    pin_ = new QLabel;
    pin_->setObjectName("pin");
    auto *renew = new QPushButton(t("remote.newPin"));
    renew->setToolTip(t("remote.newPinHint"));
    pinRow->addWidget(pinLabel);
    pinRow->addWidget(pin_);
    pinRow->addStretch();
    pinRow->addWidget(renew);
    col->addLayout(pinRow);

    auto *devicesTitle = new QLabel(t("remote.devices"));
    devicesTitle->setObjectName("sectionTitle");
    col->addWidget(devicesTitle);
    devices_ = new QVBoxLayout;
    col->addLayout(devices_);

    auto *buttons = new QDialogButtonBox;
    buttons->addButton(t("dialog.close"), QDialogButtonBox::RejectRole);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    col->addWidget(buttons);

    connect(renew, &QPushButton::clicked, this, [this] {
        deck_.renewPin();
        refresh();
    });
    connect(&deck_, &Deck::devicesChanged, this, &PhoneDialog::refresh);
    refresh();
}

void PhoneDialog::refresh() {
    const QStringList urls = server_ ? server_->remoteUrls() : QStringList();
    if (urls.isEmpty()) {
        qr_->setText(t("remote.off"));
        urls_->clear();
    } else {
        qr_->setPixmap(QPixmap::fromImage(qr::image(urls.first(), 6, 2)));
        urls_->setText(urls.join("\n"));
    }
    pin_->setText(deck_.pin());

    while (QLayoutItem *item = devices_->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    const QJsonArray list = deck_.deviceList(server_ ? server_->onlineDevices() : QSet<QString>());
    if (list.isEmpty()) {
        auto *none = new QLabel(t("remote.noDevices"));
        none->setObjectName("muted");
        devices_->addWidget(none);
    }
    for (const QJsonValue &v : list) {
        const QJsonObject d = v.toObject();
        auto *row = new QWidget;
        auto *l = new QHBoxLayout(row);
        l->setContentsMargins(0, 0, 0, 0);
        const QString since = QLocale(language()).toString(QDateTime::fromSecsSinceEpoch(qint64(d.value("created").toDouble())), QLocale::ShortFormat);
        auto *label = new QLabel(QString("<b>%1</b><br><span style='color:#8c929e'>%2 · %3%4</span>")
                                     .arg(d.value("name").toString().toHtmlEscaped(), d.value("ip").toString(),
                                          t("remote.since", {{"date", since}}),
                                          d.value("online").toBool() ? " · " + t("remote.online") : QString()));
        auto *disconnect = new QPushButton(t("remote.disconnect"));
        const QString id = d.value("id").toString();
        connect(disconnect, &QPushButton::clicked, this, [this, id] { deck_.removeDevice(id); });
        l->addWidget(label, 1);
        l->addWidget(disconnect);
        devices_->addWidget(row);
    }
}

}  // namespace resindeck::ui
