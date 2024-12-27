#include "IconProvider.h"

QIcon IconProvider::icon(const QFileInfo &info) const
{
    if (info.filePath().endsWith('/') || info.isDir()) {
        return m_dirIcon;
    }
    return m_unknownIcon;
/*
    const QString fileName = info.fileName();
    const int lastDot = info.fileName().lastIndexOf('.');
    if (lastDot != -1 && lastDot != 0) {
        const QString suffix = fileName.mid(lastDot);
        QIcon &icon = m_cache[suffix];
        if (!icon.isNull()) {
            return icon;
        }
        QMimeType mime = m_mimeDb.mimeTypeForFile(info, QMimeDatabase::MatchExtension);
        icon = QIcon::fromTheme(mime.iconName());
        if (icon.isNull()) {
            icon = QIcon::fromTheme(mime.genericIconName());
        }
        if (icon.isNull()) {
            icon = m_unknownIcon;
        }
        m_cache[suffix] = icon;
        return icon;
    }

    return m_unknownIcon;
*/
}
