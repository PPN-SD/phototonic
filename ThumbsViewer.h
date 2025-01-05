/*
 *  Copyright (C) 2013 Ofer Kashayov <oferkv@live.com>
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

#ifndef THUMBS_VIEWER_H
#define THUMBS_VIEWER_H

class ImagePreview;
class ImageViewer;
class ImageTags;
class InfoView;
class MetadataCache;

class QStandardItem;
class QStandardItemModel;

#include <QBitArray>
#include <QDir>
#include <QFileInfoList>
#include <QListView>
#include <QTimer>

#define BAD_IMAGE_SIZE 64
#define WINDOW_ICON_SIZE 48


struct DuplicateImage
{
    QString filePath;
    int duplicates;
    int id = 0;
};

struct Histogram
{
    float red[256]{};
    float green[256]{};
    float blue[256]{};

    inline float compareChannel(const float hist1[256], const float hist2[256])
    {
        float len1 = 0.f, len2 = 0.f, corr = 0.f;

        for (uint16_t i=0; i<256; i++) {
            len1 += hist1[i];
            len2 += hist2[i];
            corr += std::sqrt(hist1[i] * hist2[i]);
        }

        const float part1 = 1.f / std::sqrt(len1 * len2);

        return std::sqrt(1.f - part1 * corr);
    }

    inline float compare(const Histogram &other)
    {
        return compareChannel(red, other.red) +
            compareChannel(green, other.green) +
            compareChannel(blue, other.blue);
    }
};
Q_DECLARE_METATYPE(Histogram);

struct Constraint
{
    qint64 younger = 0;
    qint64 older = 0;
    qint64 bigger = 0;
    qint64 smaller = 0;
};

class ThumbsViewer : public QListView {
Q_OBJECT

public:
    enum UserRoles {
        FileNameRole = Qt::UserRole + 1,
        SortRole,
        LoadedRole,
        BrightnessRole,
        TypeRole,
        SizeRole,
        TimeRole,
        HistogramRole
    };
    enum ThumbnailLayouts {
        Classic,
        Squares,
        Compact
    };

    ThumbsViewer(QWidget *parent, const std::shared_ptr<MetadataCache> &metadataCache);

    void loadPrepare();

    void applyFilter();

    void reLoad();

    void loadDuplicates();

    void loadFileList();

    void loadSubDirectories();

    void setThumbColors();

    bool setCurrentIndexByName(QString &fileName);

    bool setCurrentIndexByRow(int row);

    void setCurrentRow(int row);

    void setImageViewerWindowTitle();

    void setNeedToScroll(bool needToScroll);

    void selectCurrentIndex();

    QStandardItem *addThumb(QString &imageFullPath);

    void abort(bool permanent = false);

    void selectThumbByRow(int row);

    void selectByBrightness(qreal min, qreal max);

    int getNextRow();

    int getPrevRow();

    int getCurrentRow();

    QStringList getSelectedThumbsList();

    QString getSingleSelectionFilename();
    QString fullPathOf(int idx);
    QIcon icon(int idx);

    void setImageViewer(ImageViewer *imageViewer);
    bool setFilter(const QString &filter, QString *error = nullptr);
    void sortBySimilarity();

    InfoView *infoView;
    ImagePreview *imagePreview;
    ImageTags *imageTags;
    QDir thumbsDir;
    QDir::SortFlags thumbsSortFlags;
    int thumbSize;

signals:
    void status(QString s);

protected:
    void startDrag(Qt::DropActions) override;
    void mousePressEvent(QMouseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    void initThumbs();

    bool loadThumb(int row, bool fastOnly = false);

    void findDupes(bool resetCounters);

    int getFirstVisibleThumb();

    int getLastVisibleThumb();

    void updateThumbsCount();

    void updateFoundDupesState(int duplicates, int filesScanned, int originalImages);

    void updateImageInfoViewer(int row);

    QSize itemSizeHint() const;

    QString thumbnailFileName(const QString &path) const;
    QString locateThumbnail(const QString &path) const;
    void storeThumbnail(const QString &originalPath, QImage thumbnail, const QSize &originalSize) const;

    QFileInfo thumbFileInfo;
    QFileInfoList thumbFileInfoList;
    QList<Histogram> histograms;
    QList<QString> histFiles;
    QPixmap emptyImg;
    QModelIndex currentIndex;
    std::shared_ptr<MetadataCache> metadataCache;
    ImageViewer *imageViewer;
    QHash<QBitArray, DuplicateImage> dupImageHashes;
    bool isAbortThumbsLoading = false;
    bool isClosing = false;
    bool isNeedToScroll = false;
    int currentRow = 0;
    bool scrolledForward = false;
    int thumbsRangeFirst;
    int thumbsRangeLast;

    QTimer m_selectionChangedTimer;
    QTimer m_loadThumbTimer;
    QString m_filter;
    QList<Constraint> m_constraints;
    bool m_busy;
    QStandardItemModel *m_model;

public slots:

    void loadVisibleThumbs(int scrollBarValue = 0);

    void onSelectionChanged();

    void invertSelection();

private slots:

    void loadThumbsRange();

    void loadAllThumbs();
};

#endif // THUMBS_VIEWER_H

