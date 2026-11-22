#include "mainwindow.h"

#include "dialogs.h"
#include "i18n.h"

#include <QComboBox>
#include <QDateTime>
#include <QDragEnterEvent>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMimeData>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QShortcut>
#include <QSlider>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTabBar>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>

#ifdef __GLIBC__
#include <malloc.h>
#endif

namespace resindeck::ui {

namespace {

// Runs `work` in a worker thread and `done` with its result in the window's thread, unless the
// window is gone by then.
template <typename T, typename Work, typename Done>
void inBackground(QObject *context, Work work, Done done) {
    auto *watcher = new QFutureWatcher<T>(context);
    QObject::connect(watcher, &QFutureWatcher<T>::finished, context, [watcher, done] {
        watcher->deleteLater();
        done(watcher->future());
    });
    watcher->setFuture(QtConcurrent::run(work));
}

// Gives the memory freed after building a mesh back to the system: glibc keeps it for later otherwise,
// one pool per worker thread.
void releaseMemory() {
#ifdef __GLIBC__
    malloc_trim(0);
#endif
}

QLabel *sectionTitle() {
    auto *l = new QLabel;
    l->setObjectName("sectionTitle");
    return l;
}

}  // namespace

MainWindow::MainWindow(Deck &deck, Server *server, QWidget *parent) : QMainWindow(parent), deck_(deck), server_(server) {
    setWindowTitle("ResinDeck");
    setAcceptDrops(true);
    resize(1280, 800);
    buildUi();
    retranslate();
    refreshPrinters();
    refreshLibrary();

    connect(&deck_, &Deck::printersChanged, this, &MainWindow::refreshPrinters);
    connect(&deck_, &Deck::voxelProgress, this, [this](const QString &path, double fraction) {
        if (path == path_) meshing_->setText(t("viewer.meshing") + QString(" %1%").arg(qRound(fraction * 100)));
    });
    // the library follows its folders: files added by the slicer show up on their own
    libraryTimer_.setSingleShot(true);
    libraryTimer_.setInterval(500);
    connect(&libraryTimer_, &QTimer::timeout, this, &MainWindow::refreshLibrary);
    connect(&watcher_, &QFileSystemWatcher::directoryChanged, &libraryTimer_, qOverload<>(&QTimer::start));
}

void MainWindow::buildUi() {
    auto *splitter = new QSplitter;
    setCentralWidget(splitter);

    // --- left: printers and library ---
    auto *left = new QWidget;
    left->setObjectName("sidebar");
    left->setMinimumWidth(300);
    left->setMaximumWidth(460);
    auto *lcol = new QVBoxLayout(left);
    lcol->setContentsMargins(14, 12, 14, 12);

    auto *brand = new QHBoxLayout;
    auto *logo = new QLabel("Resin<span style='color:#3d8bff'>Deck</span>");
    logo->setObjectName("brand");
    phone_ = new QPushButton;
    phone_->setObjectName("ghost");
    language_ = new QComboBox;
    for (const QString &code : languages()) language_->addItem(languageName(code), code);
    language_->setCurrentIndex(std::max(0, language_->findData(language())));
    brand->addWidget(logo, 1);
    brand->addWidget(phone_);
    brand->addWidget(language_);
    lcol->addLayout(brand);

    auto *ph = new QHBoxLayout;
    printersTitle_ = sectionTitle();
    addPrinter_ = new QPushButton;
    addPrinter_->setObjectName("ghost");
    ph->addWidget(printersTitle_, 1);
    ph->addWidget(addPrinter_);
    lcol->addLayout(ph);
    printerList_ = new QVBoxLayout;
    printerList_->setSpacing(8);
    lcol->addLayout(printerList_);

    auto *lh = new QHBoxLayout;
    libraryTitle_ = sectionTitle();
    folders_ = new QPushButton;
    folders_->setObjectName("ghost");
    lh->addWidget(libraryTitle_, 1);
    lh->addWidget(folders_);
    lcol->addSpacing(8);
    lcol->addLayout(lh);
    filter_ = new QLineEdit;
    filter_->setClearButtonEnabled(true);
    lcol->addWidget(filter_);
    files_ = new QListWidget;
    files_->setIconSize(QSize(44, 44));
    files_->setObjectName("files");
    files_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    files_->setTextElideMode(Qt::ElideMiddle);  // long slicer names keep their start and the extension
    lcol->addWidget(files_, 1);
    libraryNote_ = new QLabel;
    libraryNote_->setObjectName("muted");
    libraryNote_->setWordWrap(true);
    lcol->addWidget(libraryNote_);
    dropHint_ = new QPushButton;
    dropHint_->setObjectName("drop");
    dropHint_->setCursor(Qt::PointingHandCursor);
    lcol->addWidget(dropHint_);
    splitter->addWidget(left);

    // --- right: the open file ---
    stack_ = new QStackedWidget;
    empty_ = new QLabel;
    empty_->setAlignment(Qt::AlignCenter);
    empty_->setObjectName("muted");
    stack_->addWidget(empty_);

    auto *view = new QWidget;
    auto *rcol = new QVBoxLayout(view);
    rcol->setContentsMargins(0, 0, 0, 0);
    rcol->setSpacing(0);
    auto *header = new QWidget;
    header->setObjectName("fileHeader");
    auto *hrow = new QHBoxLayout(header);
    auto *names = new QVBoxLayout;
    fileName_ = new QLabel;
    fileName_->setObjectName("fileName");
    fileSub_ = new QLabel;
    fileSub_->setObjectName("muted");
    names->addWidget(fileName_);
    names->addWidget(fileSub_);
    tabs_ = new QTabBar;
    tabs_->addTab("3D");
    tabs_->addTab({});
    tabs_->addTab({});
    hrow->addLayout(names, 1);
    hrow->addWidget(tabs_);
    rcol->addWidget(header);

    views_ = new QStackedWidget;
    auto *viewerBox = new QWidget;
    auto *vb = new QVBoxLayout(viewerBox);
    vb->setContentsMargins(0, 0, 0, 0);
    viewer_ = new Viewer3D;
    meshing_ = new QLabel;
    meshing_->setObjectName("muted");
    meshing_->setAlignment(Qt::AlignCenter);
    vb->addWidget(viewer_, 1);
    vb->addWidget(meshing_);
    layerView_ = new ImageView;
    thumbView_ = new ImageView;
    views_->addWidget(viewerBox);
    views_->addWidget(layerView_);
    views_->addWidget(thumbView_);
    rcol->addWidget(views_, 1);

    auto *sliderRow = new QWidget;
    sliderRow->setObjectName("sliderRow");
    auto *srow = new QHBoxLayout(sliderRow);
    layerLabel_ = new QLabel;
    layerSlider_ = new QSlider(Qt::Horizontal);
    layerSlider_->setObjectName("layerSlider");
    layerValue_ = new QLabel;
    layerValue_->setObjectName("mono");
    followBadge_ = new QLabel;
    followBadge_->setObjectName("badge");
    followBadge_->hide();
    srow->addWidget(followBadge_);
    srow->addWidget(layerLabel_);
    srow->addWidget(layerSlider_, 1);
    srow->addWidget(layerValue_);
    rcol->addWidget(sliderRow);

    auto *bottom = new QWidget;
    bottom->setObjectName("infoBar");
    auto *brow = new QHBoxLayout(bottom);
    stats_ = new QGridLayout;
    stats_->setHorizontalSpacing(24);
    stats_->setVerticalSpacing(2);
    brow->addLayout(stats_, 1);
    target_ = new QComboBox;
    send_ = new QPushButton;
    sendPrint_ = new QPushButton;
    sendPrint_->setObjectName("primary");
    brow->addWidget(target_);
    brow->addWidget(send_);
    brow->addWidget(sendPrint_);
    rcol->addWidget(bottom);
    stack_->addWidget(view);
    splitter->addWidget(stack_);
    splitter->setStretchFactor(1, 1);

    statusBar()->setSizeGripEnabled(false);

    // --- behaviour ---
    connect(language_, &QComboBox::currentIndexChanged, this, [this] {
        setLanguage(language_->currentData().toString());
        retranslate();
        refreshPrinters();
        refreshLibrary();
        if (!path_.isEmpty()) openFile(path_);
    });
    connect(addPrinter_, &QPushButton::clicked, this, [this] { PrinterDialog(deck_, nullptr, this).exec(); });
    connect(folders_, &QPushButton::clicked, this, [this] {
        if (FoldersDialog(deck_, this).exec() == QDialog::Accepted) refreshLibrary();
    });
    connect(phone_, &QPushButton::clicked, this, [this] { PhoneDialog(deck_, server_, this).exec(); });
    connect(filter_, &QLineEdit::textChanged, this, [this](const QString &text) {
        for (int i = 0; i < files_->count(); ++i)
            files_->item(i)->setHidden(!files_->item(i)->text().contains(text, Qt::CaseInsensitive));
    });
    connect(files_, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *item) {
        if (item && item->data(Qt::UserRole).toString() != path_) openFile(item->data(Qt::UserRole).toString());
    });
    connect(tabs_, &QTabBar::currentChanged, this, &MainWindow::showTab);
    connect(layerSlider_, &QSlider::valueChanged, this, [this](int index) {
        if (!followMove_) {  // moved by hand: stop following
            followPending_ = false;
            if (!follow_.isEmpty()) {
                follow_.clear();
                updateFollow();
            }
        }
        showLayer(index);
    });
    connect(target_, &QComboBox::currentIndexChanged, this, &MainWindow::refreshSendButtons);
    connect(send_, &QPushButton::clicked, this, [this] { send(false); });
    connect(sendPrint_, &QPushButton::clicked, this, [this] { send(true); });
    auto pick = [this] { addFiles(QFileDialog::getOpenFileNames(this, {}, {}, "Sliced files (*.cxdlpv4 *.cxdlp)")); };
    connect(dropHint_, &QPushButton::clicked, this, pick);
    connect(new QShortcut(QKeySequence::Open, this), &QShortcut::activated, this, pick);
}

void MainWindow::retranslate() {
    printersTitle_->setText(t("printers.title").toUpper());
    addPrinter_->setText(t("printers.add"));
    addPrinter_->setToolTip(t("printers.addTitle"));
    libraryTitle_->setText(t("library.title").toUpper());
    folders_->setText(t("library.folders"));
    folders_->setToolTip(t("library.foldersTitle"));
    filter_->setPlaceholderText(t("library.filter"));
    dropHint_->setText(t("library.drop"));
    phone_->setText(t("remote.button"));
    phone_->setToolTip(t("remote.buttonTitle"));
    empty_->setText(t("viewer.empty"));
    tabs_->setTabText(1, t("viewer.tabLayers"));
    tabs_->setTabText(2, t("viewer.tabThumb"));
    layerLabel_->setText(t("viewer.layer"));
    send_->setText(t("send.send"));
    sendPrint_->setText(t("send.sendPrint"));
    target_->setToolTip(t("send.target"));
}

// --- printers ----------------------------------------------------------------------------

void MainWindow::refreshPrinters() {
    const QList<Printer *> printers = deck_.printers();
    QList<Printer *> shown;
    for (PrinterCard *c : cards_) shown << c->printer();
    if (shown != printers) {  // added, removed or replaced: rebuild the cards
        qDeleteAll(cards_);
        cards_.clear();
        while (QLayoutItem *item = printerList_->takeAt(0)) {
            delete item->widget();
            delete item;
        }
        for (Printer *p : printers) {
            auto *card = new PrinterCard(p, [this](const QString &name) { return libraryByName_.value(name); });
            connect(card, &PrinterCard::follow, this, &MainWindow::startFollowing);
            connect(card, &PrinterCard::configure, this, [this](Printer *printer) { PrinterDialog(deck_, printer, this).exec(); });
            connect(card, &PrinterCard::message, this, &MainWindow::status);
            printerList_->addWidget(card);
            cards_ << card;
        }
        if (printers.isEmpty()) {
            auto *none = new QLabel(t("printers.none"));
            none->setObjectName("muted");
            none->setWordWrap(true);
            printerList_->addWidget(none);
        }
        const QString selected = target_->currentData().toString();
        target_->blockSignals(true);
        target_->clear();
        for (Printer *p : printers) target_->addItem(p->config().value("name").toString(), p->id());
        target_->setCurrentIndex(std::max(0, target_->findData(selected)));
        target_->blockSignals(false);
    }
    for (PrinterCard *c : cards_) c->refresh();
    updateFollow();
    for (int i = 0; i < target_->count(); ++i)
        if (Printer *p = deck_.printer(target_->itemData(i).toString())) target_->setItemText(i, p->config().value("name").toString());
    refreshSendButtons();
}

void MainWindow::refreshSendButtons() {
    Printer *p = deck_.printer(target_->currentData().toString());
    QString why;
    if (!p) {
        why = t("send.whyNoPrinter");
    } else {
        const QJsonObject j = p->toJson();
        const bool accepts = j.value("extensions").toArray().contains(QJsonValue("." + QFileInfo(path_).suffix().toLower()));
        if (!j.value("online").toBool()) why = t("send.whyOffline");
        else if (j.value("needsPassword").toBool() && !j.value("hasPassword").toBool()) why = t("send.whyPassword");
        else if (j.value("upload").isObject()) why = t("send.whyUploading");
        else if (!printerReady(j)) why = t("send.whyBusy");
        else if (!accepts) why = t("send.whyFormat", {{"printer", j.value("driverName").toString()}});
    }
    for (QPushButton *b : {send_, sendPrint_}) {
        b->setEnabled(why.isEmpty() && !path_.isEmpty());
        b->setToolTip(why);
    }
}

void MainWindow::send(bool print) {
    Printer *p = deck_.printer(target_->currentData().toString());
    if (!p || path_.isEmpty()) return;
    const QString printer = p->config().value("name").toString();
    const QString file = QFileInfo(path_).fileName();
    if (print && QMessageBox::question(this, t("send.confirmTitle"), t("send.confirmText", {{"file", file}, {"printer", printer}}),
                                       QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
        return;
    auto *watcher = new QFutureWatcher<QJsonObject>(this);
    connect(watcher, &QFutureWatcher<QJsonObject>::finished, this, [this, watcher, print, printer] {
        watcher->deleteLater();
        try {
            watcher->result();
            status(t(print ? "send.sentPrinting" : "send.sent", {{"printer", printer}}));
            if (print) {  // follow it once the printer reports printing it (the upload ends first)
                followPending_ = true;
                updateFollow();
            }
        } catch (...) {
            const QString text = errorText(std::current_exception());
            status(text, true);
            QMessageBox::warning(this, "ResinDeck", text);
        }
    });
    watcher->setFuture(p->send(path_, print));
}

// --- library -----------------------------------------------------------------------------

void MainWindow::refreshLibrary() {
    if (!watcher_.directories().isEmpty()) watcher_.removePaths(watcher_.directories());
    if (!deck_.folders().isEmpty()) watcher_.addPaths(deck_.folders());

    const QJsonObject lib = deck_.library();
    const QJsonArray list = lib.value("files").toArray();
    libraryByName_.clear();
    for (const QJsonValue &v : list) {  // newest first: keep the first of each name
        const QJsonObject f = v.toObject();
        if (!libraryByName_.contains(f.value("name").toString())) libraryByName_.insert(f.value("name").toString(), f.value("path").toString());
    }
    files_->blockSignals(true);
    files_->clear();
    const QLocale locale(language());
    for (const QJsonValue &v : list) {
        const QJsonObject f = v.toObject();
        const QString date = locale.toString(QDateTime::fromMSecsSinceEpoch(qint64(f.value("mtime").toDouble() * 1000)), QLocale::ShortFormat);
        auto *item = new QListWidgetItem(f.value("name").toString() + "\n" + formatSize(qint64(f.value("size").toDouble())) + " · " + date, files_);
        const QString path = f.value("path").toString();
        item->setData(Qt::UserRole, path);
        item->setToolTip(path);
        item->setHidden(!filter_->text().isEmpty() && !item->text().contains(filter_->text(), Qt::CaseInsensitive));
        if (path == path_) files_->setCurrentItem(item);
        // thumbnails load in the background
        std::shared_ptr<SlicedFile> sliced;
        try {
            sliced = deck_.file(path);
        } catch (...) {
            continue;
        }
        QPointer<QListWidget> widget = files_;
        inBackground<QImage>(this, [sliced] { return sliced->previewCount() ? sliced->preview(0) : QImage(); },
                             [widget, path](QFuture<QImage> thumb) {
                                 if (!widget || thumb.result().isNull()) return;
                                 for (int i = 0; i < widget->count(); ++i)
                                     if (widget->item(i)->data(Qt::UserRole).toString() == path)
                                         widget->item(i)->setIcon(QPixmap::fromImage(thumb.result()));
                             });
    }
    files_->blockSignals(false);
    libraryNote_->setText(deck_.folders().isEmpty() ? t("library.noFolder") : list.isEmpty() ? t("library.empty") : QString());
    libraryNote_->setVisible(!libraryNote_->text().isEmpty());
    for (PrinterCard *c : cards_) c->refresh();  // a printing file may have arrived or gone
}

void MainWindow::addFiles(const QStringList &paths) {
    QStringList sliced;
    for (const QString &p : paths)
        if (isSlicedFile(p)) sliced << p;
    if (sliced.isEmpty()) {
        if (!paths.isEmpty()) status(t("library.dropOnly"), true);
        return;
    }
    status(t("library.adding", {{"n", sliced.size()}}));
    QString last;
    for (const QString &p : sliced) {
        QFile f(p);
        if (!f.open(QIODevice::ReadOnly)) continue;
        try {
            last = deck_.saveUpload(QFileInfo(p).fileName(), f.readAll());
        } catch (...) {
            status(errorText(std::current_exception()), true);
            return;
        }
    }
    refreshLibrary();
    if (!last.isEmpty()) openFile(last);
}

void MainWindow::dragEnterEvent(QDragEnterEvent *e) {
    if (e->mimeData()->hasUrls()) e->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent *e) {
    QStringList paths;
    for (const QUrl &u : e->mimeData()->urls())
        if (u.isLocalFile()) paths << u.toLocalFile();
    addFiles(paths);
}

// --- the open file -----------------------------------------------------------------------

void MainWindow::openFile(const QString &path) {
    path_ = path;
    follow_.clear();
    try {
        info_ = deck_.fileInfo(path);
    } catch (...) {
        stack_->setCurrentIndex(0);
        status(t("viewer.readFail", {{"error", errorText(std::current_exception())}}), true);
        path_.clear();
        return;
    }
    stack_->setCurrentIndex(1);
    fileName_->setText(info_.value("file").toString());
    fileSub_->setText(formatSize(qint64(info_.value("size").toDouble())) + " · " + QFileInfo(path).path());
    layerZ_ = info_.value("layers").toArray();

    // print settings, like the web interface's info bar
    while (QLayoutItem *item = stats_->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    const QList<std::pair<QString, QString>> stats{
        {t("stats.printer"), info_.value("printer").toString()},
        {t("stats.time"), formatDuration(info_.value("printTime").toInt())},
        {t("stats.layers"), QString("%1 × %2 mm").arg(info_.value("layerCount").toInt()).arg(info_.value("layerHeight").toDouble())},
        {t("stats.height"), QString("%1 mm").arg(info_.value("printHeight").toDouble())},
        {t("stats.resin"), info_.value("volumeMl").toDouble() > 0 ? QString::number(info_.value("volumeMl").toDouble(), 'f', 1) + " ml" : "—"},
        {t("stats.exposure"), t("stats.exposureValue", {{"exp", info_.value("exposure").toDouble()},
                                                         {"bottom", info_.value("bottomExposure").toDouble()},
                                                         {"n", info_.value("bottomLayers").toInt()}})},
        {t("stats.antialias"), QString("%1×").arg(info_.value("antiAlias").toInt())},
    };
    for (int i = 0; i < stats.size(); ++i) {
        auto *name = new QLabel(stats[i].first.toUpper());
        name->setObjectName("statName");
        auto *value = new QLabel(stats[i].second);
        stats_->addWidget(name, 0, i);
        stats_->addWidget(value, 1, i);
    }

    layerSlider_->blockSignals(true);
    layerSlider_->setRange(0, std::max(0, int(layerZ_.size()) - 1));
    layerSlider_->setValue(layerSlider_->maximum());
    layerSlider_->blockSignals(false);
    showLayer(layerSlider_->value());

    // thumbnail
    const auto sliced = deck_.file(path);
    QPointer<ImageView> thumb = thumbView_;
    inBackground<QImage>(this, [sliced] { return sliced->previewCount() ? sliced->preview(-1) : QImage(); },
                         [thumb](QFuture<QImage> f) {
                             if (thumb) thumb->setImage(f.result());
                         });

    // 3D, in two steps: the voxel grid (cached, shared with the web interface) right away, then the
    // model as printed (pixel by layer), or for models too large for that a smooth surface through
    // the layers, which replaces it when ready
    if (surfaceCancel_) surfaceCancel_->store(true);
    surfaceCancel_ = std::make_shared<std::atomic<bool>>(false);
    surfaceShown_ = false;
    viewer_->clear();
    meshing_->setText(t("viewer.meshing"));
    meshing_->setVisible(true);
    deck_.voxels(path, [this, path, sliced, cancel = surfaceCancel_](const QByteArray &payload, std::exception_ptr error) {
        if (path != path_) return;
        if (error) {
            meshing_->setText(t("viewer.readFail", {{"error", errorText(error)}}));
            return;
        }
        const Voxels voxels = decodeVoxels(payload);
        // the quick preview: a million triangles at most (about 60 MB), it is replaced in a few seconds
        inBackground<Mesh>(this, [voxels] { return voxelMesh(voxels, 1'000'000); }, [this, path](QFuture<Mesh> f) {
            if (path != path_ || surfaceShown_) return;
            viewer_->setMesh(f.result());
            showLayer(layerSlider_->value());
        });
        QPointer<QLabel> label = meshing_;
        inBackground<Mesh>(
            this,
            [sliced, voxels, cancel, label] {
                SurfaceOptions o;
                modelBounds(*sliced, voxels, o);
                o.cancel = cancel.get();
                QElapsedTimer last;
                last.start();
                o.progress = [&](double fraction) {
                    if (last.elapsed() < 200) return;
                    last.restart();
                    QMetaObject::invokeMethod(label, [label, fraction] {
                        if (label) label->setText(t("viewer.meshing") + QString(" %1%").arg(qRound(fraction * 100)));
                    });
                };
                o.maxTriangles = 6'000'000;  // about 360 MB on the GPU
                Mesh printed = printedMesh(*sliced, o);
                if (!printed.tooLarge || cancel->load()) return printed;
                o.maxTriangles = 4'000'000;  // about 100 MB
                Mesh smooth = surfaceMesh(*sliced, o);
                smooth.tooLarge = true;  // tells the window it is the smooth one
                return smooth;
            },
            [this, path, cancel](QFuture<Mesh> f) {
                if (path != path_ || cancel->load()) return;
                Mesh m = f.result();
                const bool smooth = m.tooLarge;
                if (!m.empty()) {
                    viewer_->setMesh(std::move(m), true);
                    surfaceShown_ = true;
                }
                meshing_->setText(smooth ? t("viewer.smoothed") : QString());
                meshing_->setVisible(smooth);
                releaseMemory();
            });
    });
    refreshSendButtons();
    followPending_ = true;  // if a printer is printing this file, follow it (now or at its next status)
    updateFollow();
    for (int i = 0; i < files_->count(); ++i)
        if (files_->item(i)->data(Qt::UserRole).toString() == path) {
            files_->blockSignals(true);
            files_->setCurrentRow(i);
            files_->blockSignals(false);
        }
}

// --- following a print ---------------------------------------------------------------------

void MainWindow::startFollowing(Printer *printer, const QString &path) {
    if (path != path_) openFile(path);
    if (path != path_) return;  // could not open it
    follow_ = printer->id();
    followPending_ = false;
    updateFollow();
}

// Keeps the slider, and so the 3D cut, on the layer being printed; stops when the print ends.
void MainWindow::updateFollow() {
    // follow a printer printing the open file: right after the file was opened (followPending_), or
    // whenever a printer starts printing it, even if the slider was moved before
    const QString open = path_.isEmpty() ? QString() : QFileInfo(path_).fileName();
    for (Printer *printer : deck_.printers()) {
        const QJsonObject j = printer->toJson();
        const QString file = j.value("online").toBool() && !printerReady(j) ? j.value("status").toObject().value("file").toString() : QString();
        // the print's start time tells a new print of the same file even if the pause between them was missed
        const QJsonObject job = j.value("job").toObject();
        const QString print = file.isEmpty() ? QString() : file + '@' + QString::number(job.value("file").toString() == file ? job.value("started").toDouble() : 0, 'f', 3);
        const bool started = printingFile_.value(printer->id()) != print;
        printingFile_.insert(printer->id(), print);
        if (follow_.isEmpty() && !open.isEmpty() && file == open && (followPending_ || started)) {
            follow_ = printer->id();
            followPending_ = false;
        }
    }
    Printer *p = follow_.isEmpty() ? nullptr : deck_.printer(follow_);
    const QJsonObject j = p ? p->toJson() : QJsonObject();
    const QJsonObject st = j.value("status").toObject();
    const int total = st.value("layers").toInt(), cur = st.value("layer").toInt();
    const bool printing = p && j.value("online").toBool() && !printerReady(j) && total > 0 &&
                          st.value("file").toString() == QFileInfo(path_).fileName();
    if (p && !printing && printerReady(j)) {  // finished, stopped or another file: show the whole model
        follow_.clear();
        followMove_ = true;
        layerSlider_->setValue(layerSlider_->maximum());
        followMove_ = false;
    }
    for (PrinterCard *c : cards_) c->setFollowing(printing && c->printer() == p);
    followBadge_->setVisible(printing);
    if (!printing) return;
    const QString state = st.value("state").toString();
    followBadge_->setText(t(state == "paused" || state == "pausing" ? "viewer.followingPaused" : "viewer.following",
                            {{"name", j.value("name").toString()}, {"cur", cur}, {"total", total}}));
    followMove_ = true;
    layerSlider_->setValue(std::clamp(cur - 1, 0, layerSlider_->maximum()));
    followMove_ = false;
}

void MainWindow::showLayer(int index) {
    if (layerZ_.isEmpty()) return;
    const double z = layerZ_.at(index).toDouble();
    layerValue_->setText(QString("%1/%2 · %3 mm").arg(index + 1).arg(layerZ_.size()).arg(z, 0, 'f', 2));
    viewer_->setClipZ(index == layerZ_.size() - 1 ? -1 : z);
    if (views_->currentIndex() != 1) return;
    const int request = ++layerRequest_;
    const auto sliced = deck_.file(path_);
    inBackground<QImage>(this, [sliced, index] { return sliced->layer(index); }, [this, request](QFuture<QImage> f) {
        if (request == layerRequest_) layerView_->setImage(f.result());
    });
}

void MainWindow::showTab(int index) {
    views_->setCurrentIndex(index);
    showLayer(layerSlider_->value());
}

void MainWindow::status(const QString &text, bool error) {
    statusBar()->setStyleSheet(error ? "color: #e85a50;" : "");
    statusBar()->showMessage(text, 8000);
}

}  // namespace resindeck::ui
