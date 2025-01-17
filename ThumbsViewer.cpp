/*
 *  Copyright (C) 2013-2014 Ofer Kashayov <oferkv@live.com>
 *  This file is part of Phototonic Image Viewer.
 *
 *  Phototonic is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  Phototonic is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with Phototonic.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <QApplication>
#include <QCollator>
#include <QColorSpace>
#include <QDirIterator>
#include <QCryptographicHash>
#include <QDrag>
#include <QImageReader>
#include <QLabel>
#include <QMimeData>
#include <QMimeDatabase>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QProgressDialog>
#include <QScrollBar>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QThreadPool>
#include <QTimer>
#include <QTreeWidget>

#include "MetadataCache.h"
#include "Settings.h"
#include "SmartCrop.h"
#include "Tags.h"
#include "ThumbsViewer.h"

#define BATCH_SIZE 10

ThumbsViewer::ThumbsViewer(QWidget *parent) : QListView(parent) {
    m_busy = false;
    Settings::thumbsBackgroundColor = Settings::value(Settings::optionThumbsBackgroundColor).value<QColor>();
    Settings::thumbsTextColor = Settings::value(Settings::optionThumbsTextColor).value<QColor>();
    setThumbColors();
    Settings::thumbsPagesReadCount = Settings::value(Settings::optionThumbsPagesReadCount).toUInt();
    thumbSize = Settings::value(Settings::optionThumbsZoomLevel).toInt();

    setViewMode(QListView::IconMode);
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    setResizeMode(QListView::Adjust);
    setWordWrap(true);
    setWrapping(true);
    setDragEnabled(true);
    setEditTriggers(QAbstractItemView::NoEditTriggers);
    setUniformItemSizes(false);

    // This is the default but set for clarity. Could make it configurable to use
    // QAbstractItemView::ScrollPerPixel instead.
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);

    m_model = new QStandardItemModel(this);
    m_model->setSortRole(SortRole);
    setModel(m_model);

    m_selectionChangedTimer.setInterval(10);
    m_selectionChangedTimer.setSingleShot(true);
    connect(&m_selectionChangedTimer, &QTimer::timeout, this, &ThumbsViewer::onSelectionChanged);
    connect(this->selectionModel(), &QItemSelectionModel::selectionChanged, this, [=]() {
        if (!m_selectionChangedTimer.isActive()) {
            m_selectionChangedTimer.start();
        }
    });

    m_loadThumbTimer.setInterval(250);
    m_loadThumbTimer.setSingleShot(true);
    connect(&m_loadThumbTimer, &QTimer::timeout, [=](){ loadVisibleThumbs(verticalScrollBar()->value()); });

    emptyImg.load(":/images/no_image.png");
}

void ThumbsViewer::setThumbColors() {
    QColor background = Settings::thumbsLayout == Squares ? Qt::transparent : Settings::thumbsBackgroundColor;
    QPalette pal = palette();
    pal.setColor(QPalette::Base, background);
    pal.setColor(QPalette::Text, Settings::thumbsTextColor);
    if (!Settings::thumbsBackgroundImage.isEmpty()) {
        QImage bgImg(Settings::thumbsBackgroundImage);
        pal.setBrush(QPalette::Base, bgImg);
    }
    setPalette(pal);
}

QString ThumbsViewer::getSingleSelectionFilename() {
    if (selectionModel()->selectedIndexes().size() == 1)
        return m_model->item(selectionModel()->selectedIndexes().first().row())->data(FileNameRole).toString();

    return ("");
}

QString ThumbsViewer::fullPathOf(int idx)
{
    return m_model->item(idx)->data(FileNameRole).toString();
}

QIcon ThumbsViewer::icon(int idx)
{
    QIcon icon = m_model->item(idx)->icon();
    if (icon.isNull() && loadThumb(idx, true))
        icon = m_model->item(idx)->icon();
    return icon;
}

int ThumbsViewer::getNextRow() {
    if (currentIndex().row() == m_model->rowCount() - 1) {
        return -1;
    }

    return currentIndex().row() + 1;
}

int ThumbsViewer::getPrevRow() {
    if (currentIndex().row() == 0) {
        return -1;
    }

    return currentIndex().row() - 1;
}

bool ThumbsViewer::setCurrentIndex(const QString &fileName) {
    if (!m_model->rowCount()) {
        m_desiredThumbPath = fileName;
        return true;
    }
    QModelIndexList indexList = m_model->match(m_model->index(0, 0), FileNameRole, fileName);
    if (indexList.size()) {
        setCurrentIndex(indexList.at(0));
        return true;
    }
    return false;
}

bool ThumbsViewer::setCurrentIndex(int row) {
    QModelIndex idx = m_model->indexFromItem(m_model->item(row));
    if (idx.isValid()) {
        setCurrentIndex(idx);
        return true;
    }
    return false;
}

void ThumbsViewer::currentChanged(const QModelIndex &current, const QModelIndex &previous) {
    QListView::currentChanged(current, previous);
    emit currentIndexChanged(current);
}

void ThumbsViewer::onSelectionChanged() {
    if (Settings::setWindowIcon) {
        window()->setWindowIcon(QApplication::windowIcon());
    }

    QModelIndexList indexesList = selectionModel()->selectedIndexes();
    int selectedThumbs = indexesList.size();
    if (selectedThumbs > 0) {
        int currentRow = indexesList.first().row();
        QString thumbFullPath = m_model->item(currentRow)->data(FileNameRole).toString();

        if (Settings::setWindowIcon) {
            window()->setWindowIcon(m_model->item(currentRow)->icon().pixmap(WINDOW_ICON_SIZE));
        }
    }

    if (imageTags->isVisible() && imageTags->currentDisplayMode == SelectionTagsDisplay) {
        imageTags->showSelectedImagesTags();
    }

    if (selectedThumbs >= 1) {
        emit status(tr("Selected %1 of %2").arg(QString::number(selectedThumbs))
                                           .arg(tr(" %n image(s)", "", m_model->rowCount())));
    } else if (!selectedThumbs) {
        updateThumbsCount();
    }
}

QStringList ThumbsViewer::getSelectedThumbsList() {
    QModelIndexList indexesList = selectionModel()->selectedIndexes();
    QStringList SelectedThumbsPaths;

    for (int tn = indexesList.size() - 1; tn >= 0; --tn) {
        SelectedThumbsPaths << m_model->item(indexesList[tn].row())->data(FileNameRole).toString();
    }

    return SelectedThumbsPaths;
}

void ThumbsViewer::startDrag(Qt::DropActions) {
    QModelIndexList indexesList = selectionModel()->selectedIndexes();
    if (indexesList.isEmpty()) {
        return;
    }

    QDrag *drag = new QDrag(this);
    QMimeData *mimeData = new QMimeData;
    QList<QUrl> urls;
    for (QModelIndexList::const_iterator it = indexesList.constBegin(),
                 end = indexesList.constEnd(); it != end; ++it) {
        urls << QUrl::fromLocalFile(m_model->item(it->row())->data(FileNameRole).toString());
    }
    mimeData->setUrls(urls);
    drag->setMimeData(mimeData);
    QPixmap pix;
    if (indexesList.count() > 1) {
        pix = QPixmap(128, 112);
        pix.fill(Qt::transparent);
        QPainter painter(&pix);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(Qt::white, 2));
        int x = 0, y = 0, xMax = 0, yMax = 0;
        for (int i = 0; i < qMin(5, indexesList.count()); ++i) {
            QPixmap pix = m_model->item(indexesList.at(i).row())->icon().pixmap(72);
            if (i == 4) {
                x = (xMax - pix.width()) / 2;
                y = (yMax - pix.height()) / 2;
            }
            painter.drawPixmap(x, y, pix);
            xMax = qMax(xMax, qMin(128, x + pix.width()));
            yMax = qMax(yMax, qMin(112, y + pix.height()));
            painter.drawRect(x + 1, y + 1, qMin(126, pix.width() - 2), qMin(110, pix.height() - 2));
            x = !(x == y) * 56;
            y = !y * 40;
        }
        painter.end();
        pix = pix.copy(0, 0, xMax, yMax);
        drag->setPixmap(pix);
    } else {
        pix = m_model->item(indexesList.at(0).row())->icon().pixmap(128);
        drag->setPixmap(pix);
    }
    drag->setHotSpot(QPoint(pix.width() / 2, pix.height() / 2));
    drag->exec(Qt::CopyAction | Qt::MoveAction | Qt::LinkAction, Qt::IgnoreAction);
}

void ThumbsViewer::abort(bool permanent) {
    if (!m_busy)
        return;

    isAbortThumbsLoading = true;

    if (!isClosing && permanent) {
        isClosing = true;
    }
}

void ThumbsViewer::loadVisibleThumbs(int scrollBarValue) {

    // Hack:
    // when a paint even is requested Qt first calls updateGeometry() on
    // everything.
    // qscrollbar emits valueChanged() in its updateGeometry(), leading to us
    // possibly recursing when calling processEvents.
    static bool processing = false;
    if (processing) {
        return;
    }
    processing = true;

    static int lastScrollBarValue = 0;

    if (scrollBarValue >= 0) {
        scrolledForward = (scrollBarValue >= lastScrollBarValue);
        lastScrollBarValue = scrollBarValue;
    } else {
        loadThumbsRange();
        processing = false;
        return;
    }

    for (;;) {
        int firstVisible = getFirstVisibleThumb();
        int lastVisible = getLastVisibleThumb();
        if (isAbortThumbsLoading || firstVisible < 0 || lastVisible < 0) {
            processing = false;
            return;
        }

        if (scrolledForward) {
            lastVisible += ((lastVisible - firstVisible) * (Settings::thumbsPagesReadCount + 1));
            if (lastVisible >= m_model->rowCount()) {
                lastVisible = m_model->rowCount() - 1;
            }
        } else {
            firstVisible -= (lastVisible - firstVisible) * (Settings::thumbsPagesReadCount + 1);
            if (firstVisible < 0) {
                firstVisible = 0;
            }

            lastVisible += 10;
            if (lastVisible >= m_model->rowCount()) {
                lastVisible = m_model->rowCount() - 1;
            }
        }

        if (thumbsRangeFirst == firstVisible && thumbsRangeLast == lastVisible) {
            processing = false;
            return;
        }

        thumbsRangeFirst = firstVisible;
        thumbsRangeLast = lastVisible;

        loadThumbsRange();
        if (isAbortThumbsLoading) {
            processing = false;
            break;
        }
    }
    processing = false;
}

int ThumbsViewer::getFirstVisibleThumb() {
    QModelIndex idx;

    for (int currThumb = 0; currThumb < m_model->rowCount(); ++currThumb) {
        idx = m_model->indexFromItem(m_model->item(currThumb));
        if (viewport()->rect().contains(QPoint(0, visualRect(idx).y() + visualRect(idx).height() + 1))) {
            return idx.row();
        }
    }

    return -1;
}

int ThumbsViewer::getLastVisibleThumb() {
    QModelIndex idx;

    for (int currThumb = m_model->rowCount() - 1; currThumb >= 0; --currThumb) {
        idx = m_model->indexFromItem(m_model->item(currThumb));
        if (viewport()->rect().contains(QPoint(0, visualRect(idx).y() + visualRect(idx).height() + 1))) {
            return idx.row();
        }
    }

    return -1;
}

void ThumbsViewer::loadFileList() {
    for (int i = 0; i < Settings::filesList.size(); i++) {
        addThumb(Settings::filesList[i]);
    }
    updateThumbsCount();

    imageTags->populateTagsTree();

    if (!m_desiredThumbPath.isEmpty()) {
        setCurrentIndex(m_desiredThumbPath);
        m_desiredThumbPath.clear();
        scrollTo(currentIndex());
    } else if (thumbFileInfoList.size() && selectionModel()->selectedIndexes().size() == 0) {
        setCurrentIndex(0);
    }

    m_busy = false;
}

void ThumbsViewer::reLoad() {
    static QTimer *scrollDelay = nullptr;
    if (!scrollDelay) {
        scrollDelay = new QTimer(this);
        scrollDelay->setInterval(150);
        scrollDelay->setSingleShot(true);
        connect(scrollDelay, &QTimer::timeout, [=]() { loadVisibleThumbs(verticalScrollBar()->value()); });
    }
    scrollDelay->stop();
    disconnect(verticalScrollBar(), SIGNAL(valueChanged(int)), scrollDelay, SLOT(start()));
    m_busy = true;

    loadPrepare();

    if (Settings::isFileListLoaded) {
        loadFileList();
        return;
    }

    applyFilter();
    initThumbs();
    updateThumbsCount();
    loadVisibleThumbs();

    if (Settings::includeSubDirectories) {
        loadSubDirectories();
    }

    m_busy = false;
    connect(verticalScrollBar(), SIGNAL(valueChanged(int)), scrollDelay, SLOT(start()));
}

void ThumbsViewer::loadSubDirectories() {
    QDirIterator dirIterator(Settings::currentDirectory, QDirIterator::Subdirectories);

    int processed = 0;
    while (dirIterator.hasNext()) {
        dirIterator.next();
        if (dirIterator.fileInfo().isDir() && dirIterator.fileName() != "." && dirIterator.fileName() != "..") {
            thumbsDir.setPath(dirIterator.filePath());

            initThumbs();
            updateThumbsCount();
            loadVisibleThumbs();

            if (isAbortThumbsLoading) {
                return;
            }
        }
        if (++processed > BATCH_SIZE) {
            QApplication::processEvents();
            processed = 0;
        }
    }

    onSelectionChanged();
}

bool ThumbsViewer::setFilter(const QString &filter, QString *error) {
    QStringList tokens = filter.split('/');
    m_filter = tokens.first().trimmed();
    m_constraints.clear();
    bool sane = true;
    for (int i = 1; i < tokens.size(); ++i) {
        m_constraints.append(Constraint());
        QStringList subtokens = tokens.at(i).split(' ', Qt::SkipEmptyParts);
        char side = 0;
        for (QString t : subtokens) {
            if (t.startsWith("<")) {
                side = side ? -1 : 1; t.remove(0,1);
            } else if (t.startsWith("=")) {
                side = side ? -1 : 3; t.remove(0,1);
            } else if (t.startsWith(">")) {
                side = side ? -1 : 2; t.remove(0,1);
            }
            if (side < 0) {
                sane = false;
                if (error)
                    *error += "Invalid sequence: " + tokens.at(i) + "\n";
                break;
            }
            if (side == 0)
                side = 3;
            if (t.isEmpty())
                continue;
            auto setSizeConstraint = [=](int multiplier) {
                bool ok;
                qint64 v = t.chopped(2).toFloat(&ok) * multiplier;
                if (!ok) { if (error) *error += "Invalid value: " + t + "\n"; return false; }
                if (side & 1) m_constraints.last().smaller = v;
                if (side & 2) m_constraints.last().bigger = v;
                if ((side & 3) == 3) {
                    m_constraints.last().smaller =  v * 101 / 100;
                    m_constraints.last().bigger  =  v *  99 / 100;
                }
                return true;
            };
            auto setAgeConstraint = [=](int multiplier) {
                bool ok;
                qint64 v = t.chopped(1).toFloat(&ok) * multiplier;
                if (!ok) { if (error) *error += "Invalid value: " + t + "\n"; return false; }
                if (side & 1) m_constraints.last().younger = v;
                if (side & 2) m_constraints.last().older = v;
                    if ((side & 3) == 3) {
                    m_constraints.last().younger =  v * 101 / 100;
                    m_constraints.last().older   =  v *  99 / 100;
                }
                return true;
            };
            if (t.endsWith("kb", Qt::CaseInsensitive)) {
                if (!setSizeConstraint(1024)) { sane = false; break; }
            } else if (t.endsWith("mb", Qt::CaseInsensitive)) {
                if (!setSizeConstraint(1024*1024)) { sane = false; break; }
            } else if (t.endsWith("gb", Qt::CaseInsensitive)) {
                if (!setSizeConstraint(1024*1024*1024)) { sane = false; break; }
            } else if (t.endsWith("m", Qt::CaseSensitive)) {
                if (!setAgeConstraint(60)) { sane = false; break; }
            } else if (t.endsWith("h", Qt::CaseInsensitive)) {
                if (!setAgeConstraint(60*60)) { sane = false; break; }
            } else if (t.endsWith("d", Qt::CaseInsensitive)) {
                if (!setAgeConstraint(24*60*60)) { sane = false; break; }
            } else if (t.endsWith("w", Qt::CaseInsensitive)) {
                if (!setAgeConstraint(7*24*60*60)) { sane = false; break; }
            } else if (t.endsWith("M", Qt::CaseInsensitive)) {
                if (!setAgeConstraint(30*24*60*60)) { sane = false; break; }
            } else if (t.endsWith("y", Qt::CaseInsensitive)) {
                if (!setAgeConstraint(365*24*60*60)) { sane = false; break; }
            } else {
                QDateTime date = QDateTime::fromString(t, "yyyy-MM-dd");
                if (!date.isValid()) {
                    if (error) { *error += "Invalid value: " + t + "\n"; } sane = false; break;
                }
                qint64 secs = date.secsTo(QDateTime::currentDateTime());
                if (secs < 0) {
                    qWarning() << "warning, issued future date constraint";
                    secs = 0;
                }
                if (side & 1) m_constraints.last().younger = secs;
                if (side & 2) m_constraints.last().older = secs;
                if ((side & 3) == 3) {
                    m_constraints.last().younger += 24*60*60;
                    m_constraints.last().older   -= 24*60*60;
                }
            }
            side = 0;
        }
        side = 0;
    }
    return sane;
}

void ThumbsViewer::applyFilter() {
    // Get all patterns supported by QImageReader
    static QStringList imageTypeGlobs;
    // Not threadsafe, but whatever
    if (imageTypeGlobs.isEmpty()) {
        QMimeDatabase db;
        for (const QByteArray &type : QImageReader::supportedMimeTypes()) {
            imageTypeGlobs.append(db.mimeTypeForName(type).globPatterns());
        }
    }

    QStringList fileFilters;
    QStringList tokens = m_filter.split(" ", Qt::SkipEmptyParts);
    if (tokens.isEmpty())
        tokens.append(QString()); // basic filetype glob
    for (const QString &t : tokens) {
        if (imageTypeGlobs.contains(t))
            fileFilters.append(t);
        else {
            for (const QString &glob : imageTypeGlobs) {
                fileFilters.append("*" + t + glob);
            }
        }
    }

    thumbsDir.setNameFilters(fileFilters);
    thumbsDir.setFilter(QDir::Files);
    if (Settings::showHiddenFiles) {
        thumbsDir.setFilter(thumbsDir.filter() | QDir::Hidden);
    }

    thumbsDir.setPath(Settings::currentDirectory);
    QDir::SortFlags tempThumbsSortFlags = thumbsSortFlags;
    if (tempThumbsSortFlags & QDir::Size || tempThumbsSortFlags & QDir::Time) {
        tempThumbsSortFlags ^= QDir::Reversed;
    }

    if (thumbsSortFlags & QDir::Time || thumbsSortFlags & QDir::Size || thumbsSortFlags & QDir::Type) {
        thumbsDir.setSorting(tempThumbsSortFlags);
    } else { // by name
        thumbsDir.setSorting(QDir::NoSort);
    }
}

static int gs_fontHeight = 0;

QSize ThumbsViewer::itemSizeHint() const
{
    switch(Settings::thumbsLayout) {
    case Squares:
        return QSize(thumbSize, thumbSize);
    case Compact:
        return QSize(thumbSize, thumbSize + int(2.5*gs_fontHeight));
    case Classic:
        return QSize(thumbSize, thumbSize + int(1.5*gs_fontHeight));
    default:
        qWarning() << "Invalid thumbs layout" << Settings::thumbsLayout;
        return QSize(thumbSize, thumbSize);
    }

}


void ThumbsViewer::loadPrepare() {

    m_model->clear();
    gs_fontHeight = QFontMetrics(font()).height();
    setIconSize(QSize(thumbSize, thumbSize));
    setViewportMargins(0, gs_fontHeight, 0, 0);

    if (Settings::thumbsLayout == Squares) {
        setSpacing(0);
        setUniformItemSizes(true);
        setGridSize(itemSizeHint());
    } else if (Settings::thumbsLayout == Compact) {
        setSpacing(0);
        setUniformItemSizes(false);
        setGridSize(itemSizeHint());
    } else {
        setUniformItemSizes(false);
        setGridSize(itemSizeHint() + QSize(gs_fontHeight,gs_fontHeight));
    }

    if (isNeedToScroll) {
        scrollToTop();
    }

    if (!isClosing) {
        isAbortThumbsLoading = false;
    }

    thumbsRangeFirst = -1;
    thumbsRangeLast = -1;

    imageTags->resetTagsState();
}

void ThumbsViewer::loadDuplicates()
{
    m_busy = true;
    loadPrepare();

    emit status(tr("Searching duplicate images..."));

    findDupes(true);
    m_model->setSortRole(SortRole);

    if (Settings::includeSubDirectories) {
        QDirIterator iterator(Settings::currentDirectory, QDirIterator::Subdirectories);
        while (iterator.hasNext()) {
            iterator.next();
            if (iterator.fileInfo().isDir() && iterator.fileName() != "." && iterator.fileName() != "..") {
                thumbsDir.setPath(iterator.filePath());

                findDupes(false);
                if (isAbortThumbsLoading) {
                    goto finish;
                }
            }
        }
    }

finish:
    m_model->sort(0);
    m_busy = false;
    return;
}

void ThumbsViewer::initThumbs() {
    thumbFileInfoList = thumbsDir.entryInfoList();

    if (!(thumbsSortFlags & QDir::Time) && !(thumbsSortFlags & QDir::Size) && !(thumbsSortFlags & QDir::Type)) {
        QCollator collator;
        if (thumbsSortFlags & QDir::IgnoreCase) {
            collator.setCaseSensitivity(Qt::CaseInsensitive);
        }

        collator.setNumericMode(true);

        if (thumbsSortFlags & QDir::Reversed) {
            std::sort(thumbFileInfoList.begin(), thumbFileInfoList.end(), [&](const QFileInfo &a, const QFileInfo &b) {
                    return collator.compare(a.fileName(), b.fileName()) > 0;
                    });
        } else {
            std::sort(thumbFileInfoList.begin(), thumbFileInfoList.end(), [&](const QFileInfo &a, const QFileInfo &b) {
                    return collator.compare(a.fileName(), b.fileName()) < 0;
                    });
        }
    }

    static QStandardItem *thumbItem;
    static int fileIndex;
    static QPixmap emptyPixMap;
    static QSize hintSize;
    int processed = 0;

    emptyPixMap = emptyImg.scaled(thumbSize, thumbSize);
    hintSize = itemSizeHint();

    for (fileIndex = 0; fileIndex < thumbFileInfoList.size(); ++fileIndex) {
        thumbFileInfo = thumbFileInfoList.at(fileIndex);

        Metadata::cache(thumbFileInfo.filePath());
        if (imageTags->dirFilteringActive && imageTags->isImageFilteredOut(thumbFileInfo.filePath())) {
            continue;
        }

        bool constrained = false;
        for (const Constraint &c : m_constraints) {
            constrained = false;
            if ((constrained = (c.smaller && thumbFileInfo.size() > c.smaller))) continue;
            if ((constrained = (c.bigger  && thumbFileInfo.size() < c.bigger ))) continue;
            qint64 age = thumbFileInfo.lastModified().secsTo(QDateTime::currentDateTime());
            if ((constrained = (c.older   && age < c.older  ))) continue;
            if ((constrained = (c.younger && age > c.younger))) continue;
            break; // this constraint is sufficient
        }
        if (constrained)
            continue;

        thumbItem = new QStandardItem();
        thumbItem->setData(false, LoadedRole);
        thumbItem->setData(fileIndex, SortRole);
        thumbItem->setData(thumbFileInfo.size(), SizeRole);
        thumbItem->setData(thumbFileInfo.suffix(), TypeRole);
        thumbItem->setData(thumbFileInfo.lastModified(), TimeRole);
        thumbItem->setData(thumbFileInfo.filePath(), FileNameRole);
        thumbItem->setSizeHint(hintSize);

        if (Settings::thumbsLayout != Squares) {
            thumbItem->setTextAlignment(Qt::AlignTop | Qt::AlignHCenter);
            thumbItem->setText(thumbFileInfo.fileName());
        }

        m_model->appendRow(thumbItem);

        if (++processed > BATCH_SIZE) {
            QApplication::processEvents();
            processed = 0;
        }
    }

    imageTags->populateTagsTree();

    if (!m_desiredThumbPath.isEmpty()) {
        setCurrentIndex(m_desiredThumbPath);
        m_desiredThumbPath.clear();
        scrollTo(currentIndex());
    } else if (thumbFileInfoList.size() && selectionModel()->selectedIndexes().size() == 0) {
        setCurrentIndex(0);
    }
}

void ThumbsViewer::updateThumbsCount() {
    emit status(m_model->rowCount() > 0 ? tr("%n image(s)", "", m_model->rowCount()) : tr("No images"));
    thumbsDir.setPath(Settings::currentDirectory);
}

void ThumbsViewer::selectThumbByRow(int row) {
    setCurrentIndex(row);
}

struct DuplicateImage
{
    QString filePath;
    unsigned int duplicates;
    unsigned int id = 0;
};

void ThumbsViewer::findDupes(bool resetCounters)
{
    thumbFileInfoList = thumbsDir.entryInfoList();
    static unsigned int duplicateFiles, scannedFiles, totalFiles;
    static QHash<QBitArray, DuplicateImage> imageHashes;
    if (resetCounters) {
        imageHashes.clear();
        duplicateFiles = scannedFiles = totalFiles = 0;
    }
    totalFiles += thumbsDir.entryInfoList().size();

    QElapsedTimer timer;
    timer.start();

    for (int currThumb = 0; currThumb < thumbFileInfoList.size(); ++currThumb) {
        if (timer.elapsed() > 30) {
            emit progress(scannedFiles, totalFiles);
            emit status(tr("Found %1 duplicates among %2 files").arg(duplicateFiles).arg(totalFiles));
            m_model->sort(0);
            QApplication::processEvents();
            timer.restart();
        }

        thumbFileInfo = thumbFileInfoList.at(currThumb);

        QImageReader imageReader;
        QString imageFileName = thumbFileInfo.absoluteFilePath();
        QImage image;
        imageReader.setFileName(imageFileName);
        imageReader.setQuality(50); // 50 is the threshold where Qt does fast decoding, but still good scaling
        const QSize targetSize = imageReader.size();
        QSize realSize;
        QString thumbnailPath = locateThumbnail(imageFileName);
        if (!thumbnailPath.isEmpty() && QImageReader(thumbnailPath).canRead()) {
            imageReader.setFileName(thumbnailPath);
            imageReader.read(&image);
            realSize = QSize(image.text("Thumb::Image::Width").toInt(), image.text("Thumb::Image::Height").toInt());
        }
        if (targetSize != realSize) {
            imageReader.setFileName(imageFileName);
            imageReader.read(&image);
        }

        ++scannedFiles;

        if (image.isNull()) {
            qWarning() << "invalid image" << thumbFileInfo.fileName();
            continue;
        }

        QBitArray imageHash(64);
        image = image.convertToFormat(QImage::Format_Grayscale8).scaled(9, 9, Qt::KeepAspectRatioByExpanding);
        for (int y=0; y<8; ++y) {
            const uchar *line = image.scanLine(y);
            //const uchar *nextLine = image.scanLine(y+1);
            for (int x=0; x<8; ++x) {
                imageHash.setBit(y * 8 + x, line[x] > line[x+1]);
                //imageHash.setBit(y * 8 + x + 64, line[x] > nextLine[x]);
            }
        }

        QString currentFilePath = thumbFileInfo.filePath();

        QHash<QBitArray, DuplicateImage>::iterator match = imageHashes.find(imageHash);
        if (match == imageHashes.end()) {
            imageHashes.insert(imageHash, {currentFilePath, 0, (unsigned int)imageHashes.count()});
        } else {
            ++duplicateFiles;
            // display sibling
            if (match.value().duplicates < 1) {
                if (QStandardItem *item = addThumb(match.value().filePath)) {
                    item->setData(match.value().id, SortRole);
                }
            }
            // ... and this one
            match.value().duplicates++;
            if (QStandardItem *item = addThumb(currentFilePath)) {
                item->setData(match.value().id, SortRole);
            }
        }

        if (isAbortThumbsLoading) {
            break;
        }
    }

    emit progress(scannedFiles, totalFiles);
    emit status(tr("Found %1 duplicates among %2 files").arg(duplicateFiles).arg(totalFiles));
    m_model->sort(0);
    QApplication::processEvents();
}

void ThumbsViewer::selectByBrightness(qreal min, qreal max) {
    loadAllThumbs();
    QItemSelection sel;
    for (int row = 0; row < m_model->rowCount(); ++row) {
        QModelIndex idx = m_model->index(row, 0);
        QVariant brightness = m_model->data(idx, BrightnessRole);
        if (brightness.isValid()) {
            qreal val = brightness.toReal();
            if (val >= min && val <= max)
                sel.select(idx, idx);
        }
    }
    selectionModel()->select(sel, QItemSelectionModel::ClearAndSelect);
}

void ThumbsViewer::loadAllThumbs() {
    QProgressDialog progress(tr("Loading thumbnails..."), tr("Abort"), 0, thumbFileInfoList.count(), this);
    int processed = 0;
    for (int i = 0; i < thumbFileInfoList.count(); ++i) {
        progress.setValue(i);
        if (progress.wasCanceled())
            break;
        if (m_model->item(i)->data(LoadedRole).toBool())
            continue;
        loadThumb(i);

        if (isAbortThumbsLoading) {
            break;
        }

        if (++processed > BATCH_SIZE) {
            QApplication::processEvents();
            processed = 0;
        }
    }
}

static Histogram calcHist(const QImage &img)
{
    Histogram hist;
    if (img.isNull()) {
        qWarning() << "Invalid file";
        return hist;
    }
    const QImage image = img.scaled(256, 256).convertToFormat(QImage::Format_RGB888);
    for (int y=0; y<image.height(); y++) {
        const uchar *line = image.scanLine(y);
        for (int x=0; x<image.width(); x++) {
            const int index = x * 3;
            hist.red[line[index + 0]] += 1.f;
            hist.green[line[index + 1]] += 1.f;
            hist.blue[line[index + 2]] += 1.f;
        }
    }
    return hist;
}

static Histogram calcHist(const QString &filePath) {
    QImageReader reader(filePath);
    reader.setScaledSize(QSize(256, 256));
    reader.setAutoTransform(false);
    QImage image = reader.read();
    if (image.isNull()) {
        qWarning() << "Invalid file" << filePath << reader.errorString();
        return {};
    }
    return calcHist(image);
}


void ThumbsViewer::sortBySimilarity() {
    QProgressDialog progress(tr("Loading..."), tr("Abort"), 0, thumbFileInfoList.count(), this);
    progress.show();
    QApplication::processEvents();

    int processed = 0;
    for (int i = 0; i < thumbFileInfoList.count(); ++i) {
        QStandardItem *item = m_model->item(i);

        Q_ASSERT(item);
        if (!item) {
            continue;
        }
        const QString filename = item->data(FileNameRole).toString();
        if (histFiles.contains(filename)) {
            continue;
        }
        histograms.append(calcHist(filename));
        histFiles.append(filename);

        if (++processed > BATCH_SIZE) {
            processed = 0;
            progress.setValue(i);
            QApplication::processEvents();
            if (progress.wasCanceled()) {
                return;
            }
        }
    }

    progress.setLabelText(tr("Comparing..."));
    progress.setValue(0);

    for (int i=0; i<histFiles.size() - 1; i++) {
        float minScore = std::numeric_limits<float>::max();
        int minIndex = i+1;

        for (int j=i+1; j<histFiles.size(); j++) {
            const float score = histograms[i].compare(histograms[j]);
            if (score > minScore) {
                continue;
            }
            minIndex = j;
            minScore = score;

            processed++;
        }
        std::swap(histFiles[i+1], histFiles[minIndex]);
        std::swap(histograms[i+1], histograms[minIndex]);

        if (processed > BATCH_SIZE * 10) {
            processed = 0;
            progress.show();
            progress.setValue(i);
            QApplication::processEvents();
            if (progress.wasCanceled()) {
                return;
            }
        }
    }

    progress.setLabelText(tr("Sorting..."));
    progress.setMaximum(thumbFileInfoList.count() + 1); // + 1 for the call to sort() at the bottom
    progress.setValue(0);
    QHash<QString, int> indices;
    for (int i=0; i<histFiles.size(); i++) {
        indices[histFiles[i]] = i;
    }
    for (int i = 0; i < thumbFileInfoList.count(); ++i) {
        QStandardItem *item = m_model->item(i);
        Q_ASSERT(item);
        if (!item) {
            qWarning() << "Invalid item" << i;
            continue;
        }
        const QString filename = item->data(FileNameRole).toString();
        if (!indices.contains(filename)) {
            qWarning() << "Invalid file" << filename;
            continue;
        }
        item->setData(indices.size() - indices[filename], SortRole);

        if (++processed > BATCH_SIZE) {
            processed = 0;
            progress.show();
            progress.setValue(i);
            QApplication::processEvents();
            if (progress.wasCanceled()) {
                return;
            }
        }
    }
    QApplication::processEvents();

    m_model->setSortRole(SortRole);
    m_model->sort(0);
}

void ThumbsViewer::loadThumbsRange() {
    static bool isInProgress = false;
    static int currentRowCount;
    int currThumb;

    if (isInProgress) {
        isAbortThumbsLoading = true;
        QTimer::singleShot(0, this, SLOT(loadThumbsRange()));
        return;
    }

    isInProgress = true;
    currentRowCount = m_model->rowCount();

    QElapsedTimer timer;
    timer.start();

    for (scrolledForward ? currThumb = thumbsRangeFirst : currThumb = thumbsRangeLast;
         (scrolledForward ? currThumb <= thumbsRangeLast : currThumb >= thumbsRangeFirst);
         scrolledForward ? ++currThumb : --currThumb) {

        if (isAbortThumbsLoading || m_model->rowCount() != currentRowCount || currThumb < 0)
            break;

        if (m_model->item(currThumb)->data(LoadedRole).toBool())
            continue;

        if (!loadThumb(currThumb, true)) {
            QThreadPool::globalInstance()->start([=](){loadThumb(currThumb);});
//            qDebug() << "slow image read"; loadThumb(currThumb);
        }

        if (timer.elapsed() > 10) {
            QApplication::processEvents();
            timer.restart();
        }
    }

    isInProgress = false;

    if (!isClosing) {
        isAbortThumbsLoading = false;
    }
}

QString ThumbsViewer::thumbnailFileName(const QString &originalPath) const
{
    QFileInfo info(originalPath);
    QString canonicalPath = info.canonicalFilePath();
    if (canonicalPath.isEmpty()) {
        qWarning() << originalPath << "does not exist!";
        canonicalPath = info.absoluteFilePath();
    }
    QUrl url = QUrl::fromLocalFile(canonicalPath);
    QCryptographicHash md5(QCryptographicHash::Md5);
    md5.addData(QFile::encodeName(url.adjusted(QUrl::RemovePassword).url()));
    return QString::fromLatin1(md5.result().toHex()) + QStringLiteral(".png");
}

QString ThumbsViewer::locateThumbnail(const QString &originalPath) const
{
#if defined(Q_OS_MAC) || defined(Q_OS_WIN)
    return "";
#endif
    QStringList folders = {
        QStringLiteral("xx-large/"), // max 1024px
        QStringLiteral("x-large/"), // max 512px
        QStringLiteral("large/"), // max 256px, doesn't look too bad when upscaled to max
    };

    if (thumbSize <= 200) {
        folders.append(QStringLiteral("normal/")); // 128px max
    }

    const QString filename = thumbnailFileName(originalPath);
    const QString basePath = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) +
        QLatin1String("/thumbnails/");
    const QFileInfo originalInfo(originalPath);
    for (const QString &folder : folders) {
        QFileInfo info(basePath + folder + filename);
        if (!info.exists()) {
            continue;
        }
        if (originalInfo.metadataChangeTime() > info.lastModified()) {
            continue;
        }
        if (originalInfo.lastModified() > info.lastModified()) {
            continue;
        }
        return info.absoluteFilePath();
    }
    return QString();
}

void ThumbsViewer::storeThumbnail(const QString &originalPath, QImage thumbnail, const QSize &originalSize) const {
#if defined(Q_OS_MAC) || defined(Q_OS_WIN)
    return;
#endif
    const QString canonicalPath = QFileInfo(originalPath).canonicalFilePath();
    if (canonicalPath.isEmpty()) {
        qWarning() << "Asked to store thumbnail for non-existent path" << originalPath;
        return;
    }

    QString folder = QStringLiteral("normal/");
    const int maxSize = qMax(thumbnail.width(), thumbnail.height());
    if (maxSize < 64) {
        qDebug() << "Refusing to store tiny thumbnail" << thumbnail.size();
        return;
    }
    if (maxSize >= 1024) {
        folder = QStringLiteral("xx-large/");
        thumbnail = thumbnail.scaled(1024, 1024, Qt::KeepAspectRatio);
    } else if (maxSize >= 384) {
        folder = QStringLiteral("x-large/");
        thumbnail = thumbnail.scaled(512, 512, Qt::KeepAspectRatio);
    } else if (maxSize > 200) {
        folder = QStringLiteral("large/");
        thumbnail = thumbnail.scaled(256, 256, Qt::KeepAspectRatio);
    } else if (maxSize >= 100) {
        folder = QStringLiteral("normal/");
        thumbnail = thumbnail.scaled(128, 128, Qt::KeepAspectRatio);
    } else {
        qWarning() << "Thumbnail too small" << thumbnail.size();
        return;
    }

    const QString filename = thumbnailFileName(originalPath);
    const QString basePath = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) +
        QLatin1String("/thumbnails/");

    if (!QFileInfo::exists(basePath + folder)) {
        QDir().mkpath(basePath + folder);
    }

    const QString fullPath = basePath + folder + filename;
    QFileInfo info(fullPath);

    QDateTime lastModified = info.lastModified();
    if (info.metadataChangeTime() > info.lastModified()) {
        lastModified = info.metadataChangeTime();
    }
    thumbnail.setText(QStringLiteral("Thumb::MTime"), QString::number(lastModified.toSecsSinceEpoch()));

    QUrl url = QUrl::fromLocalFile(canonicalPath).adjusted(QUrl::RemovePassword);
    thumbnail.setText(QStringLiteral("Thumb::URI"), url.url());

    thumbnail.setText(QStringLiteral("Thumb::Image::Width"), QString::number(originalSize.width()));
    thumbnail.setText(QStringLiteral("Thumb::Image::Height"), QString::number(originalSize.height()));
    thumbnail.setText("Software", "Phototonic");
    thumbnail.convertToColorSpace(QColorSpace::SRgb);

    thumbnail.save(fullPath);
}

bool ThumbsViewer::loadThumb(int currThumb, bool fastOnly) {
    if (!m_model->item(currThumb)) {
        qDebug() << "meeek: loadThumb for invalid row" << currThumb;
        return false;
    }
    if (m_model->item(currThumb)->data(LoadedRole).toBool())
        return true;

    QImageReader thumbReader;
    QString imageFileName = m_model->item(currThumb)->data(FileNameRole).toString();
    QImage thumb;
    bool imageReadOk = false;
    bool shouldStoreThumbnail = false;

    thumbReader.setFileName(imageFileName);
    thumbReader.setQuality(50); // 50 is the threshold where Qt does fast decoding, but still good scaling
    const QSize origThumbSize = thumbReader.size();
    QSize currentThumbSize = origThumbSize;

    QString thumbnailPath = locateThumbnail(imageFileName);
    if (!thumbnailPath.isEmpty()) {
        if (QImageReader(thumbnailPath).canRead()) {
            thumbReader.setFileName(thumbnailPath);
        } else {
            qWarning() << "Invalid thumbnail" << thumbnailPath;
            shouldStoreThumbnail = true;
        }
    } else {
        shouldStoreThumbnail = true;
    }
    if (fastOnly && shouldStoreThumbnail)
        return false;

    if (currentThumbSize.isValid()) {
        if (currentThumbSize.width() != thumbSize || currentThumbSize.height() != thumbSize) {
            currentThumbSize.scale(QSize(thumbSize, thumbSize), Settings::thumbsLayout != Classic ? Qt::KeepAspectRatioByExpanding : Qt::KeepAspectRatio);
        }

        thumbReader.setScaledSize(currentThumbSize);
        imageReadOk = thumbReader.read(&thumb);

        if (imageReadOk && !shouldStoreThumbnail) {
            int w = thumb.text("Thumb::Image::Width").toInt();
            int h = thumb.text("Thumb::Image::Height").toInt();
            if (origThumbSize != QSize(w, h)) {
                if (fastOnly)
                    return false;
                qWarning() << "Invalid size in stored thumbnail" << w << h << "vs" << origThumbSize;
                imageReadOk = false;
            }
        }
        if (!imageReadOk && !shouldStoreThumbnail) { // tried thumbnail but somehow failed, sanitize it
            shouldStoreThumbnail = true;
            thumbReader.setFileName(imageFileName);
            imageReadOk = thumbReader.read(&thumb);
        }
    }

    if (imageReadOk) {
        if (shouldStoreThumbnail) {
            storeThumbnail(imageFileName, thumb, origThumbSize);
        }
        if (Settings::exifThumbRotationEnabled) {
            thumb = thumb.transformed(Metadata::transformation(imageFileName), Qt::SmoothTransformation);
            currentThumbSize = thumb.size();
            currentThumbSize.scale(QSize(thumbSize, thumbSize), Settings::thumbsLayout != Classic ? Qt::KeepAspectRatioByExpanding : Qt::KeepAspectRatio);
        }

        m_model->item(currThumb)->setData(qGray(thumb.scaled(1, 1).pixel(0, 0)) / 255.0, BrightnessRole);

        if (Settings::thumbsLayout != Classic) {
            thumb = SmartCrop::crop(thumb, QSize(thumbSize, thumbSize));
        }

        m_model->item(currThumb)->setIcon(QPixmap::fromImage(thumb));
        m_model->item(currThumb)->setData(true, LoadedRole);
        histograms.append(calcHist(thumb));
        histFiles.append(imageFileName);
        m_model->item(currThumb)->setSizeHint(itemSizeHint());
    } else {
        m_model->item(currThumb)->setIcon(QIcon::fromTheme("image-missing",
                                                                     QIcon(":/images/error_image.png")).pixmap(
                BAD_IMAGE_SIZE, BAD_IMAGE_SIZE));
        currentThumbSize.setHeight(BAD_IMAGE_SIZE);
        currentThumbSize.setWidth(BAD_IMAGE_SIZE);
        return false;
    }
    return true;
}

QStandardItem * ThumbsViewer::addThumb(QString &imageFullPath) {

    Metadata::cache(imageFullPath);
    if (imageTags->dirFilteringActive && imageTags->isImageFilteredOut(imageFullPath)) {
        return nullptr;
    }

    QStandardItem *thumbItem = new QStandardItem();
    QImageReader thumbReader;
    QSize hintSize = itemSizeHint();
    QSize currThumbSize;

    thumbFileInfo = QFileInfo(imageFullPath);
    thumbItem->setData(true, LoadedRole);
    thumbItem->setData(0, SortRole);
    thumbItem->setData(thumbFileInfo.size(), SizeRole);
    thumbItem->setData(thumbFileInfo.lastModified(), TimeRole);
    thumbItem->setData(thumbFileInfo.suffix(), TypeRole);
    thumbItem->setData(thumbFileInfo.filePath(), FileNameRole);
    if (Settings::thumbsLayout != Squares) {
        thumbItem->setTextAlignment(Qt::AlignTop | Qt::AlignHCenter);
        thumbItem->setData(thumbFileInfo.fileName(), Qt::DisplayRole);
    }
    thumbItem->setSizeHint(hintSize);

    thumbReader.setFileName(imageFullPath);
    currThumbSize = thumbReader.size();
    if (currThumbSize.isValid()) {
        if (currThumbSize.width() > thumbSize || currThumbSize.height() > thumbSize) {
            currThumbSize.scale(QSize(thumbSize, thumbSize), Settings::thumbsLayout != Classic ? Qt::KeepAspectRatioByExpanding : Qt::KeepAspectRatio);
        }

        thumbReader.setScaledSize(currThumbSize);
        QImage thumb = thumbReader.read();

        if (Settings::exifThumbRotationEnabled) {
            thumb = thumb.transformed(Metadata::transformation(imageFullPath), Qt::SmoothTransformation);
            currThumbSize = thumb.size();
            currThumbSize.scale(QSize(thumbSize, thumbSize), Settings::thumbsLayout != Classic ? Qt::KeepAspectRatioByExpanding : Qt::KeepAspectRatio);
        }
        thumbItem->setData(qGray(thumb.scaled(1, 1).pixel(0, 0)) / 255.0, BrightnessRole);

        thumbItem->setIcon(QPixmap::fromImage(thumb));
    } else {
        thumbItem->setIcon(
                QIcon::fromTheme("image-missing", QIcon(":/images/error_image.png")).pixmap(BAD_IMAGE_SIZE,
                                                                                            BAD_IMAGE_SIZE));
        currThumbSize.setHeight(BAD_IMAGE_SIZE);
        currThumbSize.setWidth(BAD_IMAGE_SIZE);
    }

    m_model->appendRow(thumbItem);
    return thumbItem;
}

void ThumbsViewer::mousePressEvent(QMouseEvent *event) {
    QListView::mousePressEvent(event);

    if (Settings::reverseMouseBehavior && event->button() == Qt::MiddleButton) {
        if (selectionModel()->selectedIndexes().size() == 1)
                emit(doubleClicked(selectionModel()->selectedIndexes().first()));
    }
}

void ThumbsViewer::resizeEvent(QResizeEvent *event) {
    QListView::resizeEvent(event);
    m_loadThumbTimer.start();
}

void ThumbsViewer::invertSelection() {
    QItemSelection toggleSelection;
    QModelIndex firstIndex = m_model->index(0, 0);
    QModelIndex lastIndex = m_model->index(m_model->rowCount() - 1, 0);
    toggleSelection.select(firstIndex, lastIndex);
    selectionModel()->select(toggleSelection, QItemSelectionModel::Toggle);
}

void ThumbsViewer::setNeedToScroll(bool needToScroll) {
    this->isNeedToScroll = needToScroll;
}
