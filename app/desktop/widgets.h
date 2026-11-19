// Small pieces of the window: formatting, the printer card and a scaled image view.
#pragma once

#include "printers/printer.h"

#include <QFrame>
#include <QImage>
#include <QJsonObject>
#include <QWidget>

class QLabel;
class QProgressBar;
class QPushButton;

namespace resindeck::ui {

QString formatDuration(int seconds);     // "1h02" or "23 min"
QString formatClock(double unixTime);    // "14:32", with the weekday when it is not today
QString formatSize(qint64 bytes);        // "1.3 MB" or "521 KB"
QString statusLabel(const QJsonObject &printer);  // "Printing", "Offline"...
QColor statusColor(const QJsonObject &printer);
bool printerReady(const QJsonObject &printer);    // online and can take a new file

// One printer: state, progress, start and end times, and its buttons.
class PrinterCard : public QFrame {
    Q_OBJECT
public:
    PrinterCard(Printer *printer, QWidget *parent = nullptr);
    void refresh();
    Printer *printer() const { return printer_; }

signals:
    void configure(Printer *printer);
    void message(const QString &text, bool error);

private:
    void runCommand(const QString &action);

    Printer *printer_;
    QLabel *dot_, *name_, *host_, *state_, *file_, *layer_, *remaining_, *started_, *eta_, *upload_, *error_;
    QProgressBar *progress_, *uploadBar_;
    QPushButton *pause_, *stop_, *configure_;
};

// An image scaled to fit, centred on a dark background (layers and thumbnails).
class ImageView : public QWidget {
public:
    explicit ImageView(QWidget *parent = nullptr);
    void setImage(const QImage &image);
    void setPlaceholder(const QString &text);

protected:
    void paintEvent(QPaintEvent *) override;

private:
    QImage image_;
    QString placeholder_;
};

}  // namespace resindeck::ui
