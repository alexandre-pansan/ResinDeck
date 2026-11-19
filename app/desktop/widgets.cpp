#include "widgets.h"

#include "i18n.h"

#include <QDateTime>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QMessageBox>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>

namespace resindeck::ui {

QString formatDuration(int s) {
    const int h = s / 3600, m = s % 3600 / 60;
    return h ? QString("%1h%2").arg(h).arg(m, 2, 10, QChar('0')) : QString("%1 min").arg(m);
}

QString formatClock(double unixTime) {
    const QDateTime d = QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(unixTime * 1000));
    const QLocale locale(language());
    const QString time = locale.toString(d.time(), QLocale::ShortFormat);
    return d.date() == QDate::currentDate() ? time : locale.toString(d.date(), "ddd") + " " + time;
}

QString formatSize(qint64 b) {
    return b > 1000000 ? QString::number(double(b) / 1e6, 'f', 1) + " MB" : QString::number(qRound(double(b) / 1e3)) + " KB";
}

bool printerReady(const QJsonObject &p) {
    if (!p.value("online").toBool()) return false;
    const QJsonValue st = p.value("status");
    if (!st.isObject()) return true;  // online without a status (no password yet): nothing printing that we know of
    const QString s = st.toObject().value("state").toString();
    return s == "idle" || s == "stopped" || s == "finished" || s == "failed";
}

QString statusLabel(const QJsonObject &p) {
    if (!p.value("online").toBool()) return t("status.offline");
    const QJsonObject st = p.value("status").toObject();
    const QString state = st.value("state").toString();
    if (state.isEmpty()) return t("status.online");
    const QString label = t("status." + state);
    return state == "unknown" || label == "status." + state ? st.value("code").toString() : label;
}

QColor statusColor(const QJsonObject &p) {
    if (!p.value("online").toBool()) return QColor(120, 125, 135);
    const QString s = p.value("status").toObject().value("state").toString();
    if (s == "printing" || s == "finishing") return QColor(61, 139, 255);
    if (s == "paused" || s == "pausing") return QColor(240, 180, 40);
    if (s == "failed" || s == "stopping" || s == "stopped") return QColor(232, 90, 80);
    return QColor(52, 199, 89);
}

// --- PrinterCard -------------------------------------------------------------------------

PrinterCard::PrinterCard(Printer *printer, QWidget *parent) : QFrame(parent), printer_(printer) {
    setObjectName("printerCard");
    auto *col = new QVBoxLayout(this);
    col->setContentsMargins(12, 10, 12, 10);
    col->setSpacing(4);

    auto *top = new QHBoxLayout;
    dot_ = new QLabel("●");
    name_ = new QLabel;
    name_->setObjectName("printerName");
    host_ = new QLabel;
    host_->setObjectName("muted");
    top->addWidget(dot_);
    top->addWidget(name_, 1);
    top->addWidget(host_);
    col->addLayout(top);

    state_ = new QLabel;
    file_ = new QLabel;
    file_->setObjectName("muted");
    file_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    col->addWidget(state_);
    col->addWidget(file_);

    progress_ = new QProgressBar;
    progress_->setTextVisible(false);
    progress_->setMaximumHeight(6);
    col->addWidget(progress_);
    auto row = [&](QLabel *&left, QLabel *&right) {
        auto *l = new QHBoxLayout;
        left = new QLabel;
        right = new QLabel;
        left->setObjectName("small");
        right->setObjectName("small");
        l->addWidget(left, 1);
        l->addWidget(right);
        col->addLayout(l);
    };
    row(layer_, remaining_);
    row(started_, eta_);

    uploadBar_ = new QProgressBar;
    uploadBar_->setObjectName("upload");
    uploadBar_->setTextVisible(false);
    uploadBar_->setMaximumHeight(6);
    upload_ = new QLabel;
    upload_->setObjectName("small");
    col->addWidget(uploadBar_);
    col->addWidget(upload_);

    error_ = new QLabel;
    error_->setObjectName("error");
    error_->setWordWrap(true);
    col->addWidget(error_);

    auto *buttons = new QHBoxLayout;
    pause_ = new QPushButton;
    stop_ = new QPushButton;
    stop_->setObjectName("danger");
    configure_ = new QPushButton;
    configure_->setObjectName("ghost");
    buttons->addWidget(pause_);
    buttons->addWidget(stop_);
    buttons->addStretch();
    buttons->addWidget(configure_);
    col->addLayout(buttons);

    connect(pause_, &QPushButton::clicked, this, [this] {
        const QString state = printer_->toJson().value("status").toObject().value("state").toString();
        runCommand(state == "paused" || state == "pausing" ? "resume" : "pause");
    });
    connect(stop_, &QPushButton::clicked, this, [this] {
        const QJsonObject p = printer_->toJson();
        const auto answer = QMessageBox::warning(
            this, t("stop.title"),
            t("stop.text", {{"file", p.value("status").toObject().value("file").toString()}, {"printer", p.value("name").toString()}}),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
        if (answer == QMessageBox::Yes) runCommand("stop");
    });
    connect(configure_, &QPushButton::clicked, this, [this] { emit configure(printer_); });
    refresh();
}

void PrinterCard::runCommand(const QString &action) {
    auto *watcher = new QFutureWatcher<QJsonObject>(this);
    connect(watcher, &QFutureWatcher<QJsonObject>::finished, this, [this, watcher, action] {
        watcher->deleteLater();
        try {
            watcher->result();
            if (action == "stop") emit message(t("stop.done"), false);
        } catch (...) {
            emit message(errorText(std::current_exception()), true);
        }
    });
    watcher->setFuture(printer_->command(action));
    if (action == "pause" || action == "resume") emit message(t(action == "pause" ? "pause.pausing" : "pause.resuming"), false);
}

void PrinterCard::refresh() {
    const QJsonObject p = printer_->toJson();
    const QJsonObject st = p.value("status").toObject();
    const QString state = st.value("state").toString();
    const bool online = p.value("online").toBool();

    dot_->setStyleSheet(QString("color: %1;").arg(statusColor(p).name()));
    name_->setText(p.value("name").toString());
    host_->setText(p.value("host").toString());
    QStringList tip{p.value("driverName").toString()};
    if (p.value("mac").isString()) tip << p.value("mac").toString();
    host_->setToolTip(tip.join(" · "));
    state_->setText(statusLabel(p) + (p.value("version").isString() ? "  · fw " + p.value("version").toString() : QString()));

    const QString file = st.value("file").toString();
    file_->setText(file);
    file_->setVisible(online && !file.isEmpty());

    const int total = st.value("layers").toInt(), cur = st.value("layer").toInt(), rem = st.value("remaining").toInt();
    const bool active = online && !printerReady(p) && total > 0;
    const bool showProgress = online && total > 0;
    progress_->setVisible(showProgress);
    layer_->setVisible(showProgress);
    remaining_->setVisible(showProgress);
    if (showProgress) {
        progress_->setRange(0, total);
        progress_->setValue(cur);
        layer_->setText(t("printers.layer", {{"cur", cur}, {"total", total}, {"pct", cur * 100 / total}}));
        remaining_->setText(rem ? t("printers.remaining", {{"time", formatDuration(rem)}}) : QString());
    }

    // when the print started and when it should end (or ended): the printer only reports the time left
    const QJsonObject job = p.value("job").toObject();
    QString start, end;
    if (online && !job.isEmpty() && job.value("file").toString() == file) {
        const QString about = job.value("approx").toBool() ? "≈ " : "";
        if (job.value("started").isDouble()) start = t("printers.started", {{"time", about + formatClock(job.value("started").toDouble())}});
        if (active && rem)
            end = t("printers.eta", {{"time", formatClock(double(QDateTime::currentMSecsSinceEpoch()) / 1000.0 + rem)}});
        else if (state == "finished" && job.value("ended").isDouble())
            end = t("printers.ended", {{"time", formatClock(job.value("ended").toDouble())}});
        if (!active && state != "finished") start.clear();
    }
    started_->setText(start);
    eta_->setText(end);
    started_->setVisible(!start.isEmpty() || !end.isEmpty());
    eta_->setVisible(started_->isVisible());

    const QJsonObject up = p.value("upload").toObject();
    uploadBar_->setVisible(!up.isEmpty());
    upload_->setVisible(!up.isEmpty());
    if (!up.isEmpty()) {
        uploadBar_->setRange(0, 100);
        uploadBar_->setValue(up.value("pct").toInt());
        upload_->setText(t("printers.uploading", {{"file", up.value("file").toString()}}) + QString("  %1%").arg(up.value("pct").toInt()));
    }

    const QString err = p.value("error").toString();
    error_->setText(err.isEmpty() || err == "no_connection" ? QString() : errorText(err));
    error_->setVisible(!error_->text().isEmpty());

    const bool paused = state == "paused" || state == "pausing";
    pause_->setText(paused ? t("printers.resume") : t("printers.pause"));
    stop_->setText(t("printers.stop"));
    pause_->setVisible(active);
    stop_->setVisible(active);
    configure_->setText(t("printers.configure"));
    configure_->setToolTip(t("printers.configureTitle"));
}

// --- ImageView ---------------------------------------------------------------------------

ImageView::ImageView(QWidget *parent) : QWidget(parent) { setMinimumSize(200, 150); }

void ImageView::setImage(const QImage &image) {
    image_ = image;
    placeholder_.clear();
    update();
}

void ImageView::setPlaceholder(const QString &text) {
    image_ = {};
    placeholder_ = text;
    update();
}

void ImageView::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.fillRect(rect(), QColor(13, 16, 20));
    if (image_.isNull()) {
        p.setPen(QColor(140, 146, 158));
        p.drawText(rect(), Qt::AlignCenter, placeholder_);
        return;
    }
    const QSize size = image_.size().scaled(this->size() - QSize(16, 16), Qt::KeepAspectRatio);
    const QRect target(QPoint((width() - size.width()) / 2, (height() - size.height()) / 2), size);
    p.setRenderHint(QPainter::SmoothPixmapTransform, size.width() < image_.width());
    p.drawImage(target, image_);
}

}  // namespace resindeck::ui
