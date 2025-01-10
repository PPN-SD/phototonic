/*
 *  Copyright (C) 2013-2018 Ofer Kashayov <oferkv@live.com>
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

#ifndef EXTERNAL_APPS_DIALOG_H
#define EXTERNAL_APPS_DIALOG_H

class QLineEdit;
class QTableView;
class QStandardItemModel;
#include <QDialog>

class ExternalAppsDialog : public QDialog {
    Q_OBJECT

public:
    ExternalAppsDialog(QWidget *parent);

private slots:
    void add();
    void entry();
    void ok();
    void remove();

private:
    void addTableModelItem(QStandardItemModel *model, QString &key, QString &val);

private:
    QTableView *appsTable;
    QStandardItemModel *appsTableModel;
    QLineEdit *m_wallpaperCommand;
};

#endif // EXTERNAL_APPS_DIALOG_H