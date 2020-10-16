/***************************************************************************
 * hörbert Software
 * Copyright (C) 2019 WINZKI GmbH & Co. KG
 *
 * Authors of the original version: Igor Yalovenko, Rainer Brang
 * Dec. 2019
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 ****************************************************************************/

#ifndef TRIPLECHECKBOXWIDGET_H
#define TRIPLECHECKBOXWIDGET_H

#include <QWidget>

#include "triplecheckbox.h"

class QHBoxLayout;

/**
 * @brief Container for TripleCheckBox
 */
class TripleCheckBoxWidget : public QWidget
{
    Q_OBJECT
public:
    /**
     * @brief TripleCheckBoxWidget constructor
     * @param parent
     * @param id
     */
    explicit TripleCheckBoxWidget(QWidget *parent = Q_NULLPTR, int id = 0);

signals:
    /**
     * @brief clicked
     */
    void clicked(int );

    /**
     * @brief stateChanged
     */
    void stateChanged(int , int );

public slots:
private:
    int m_id;
    TripleCheckBox *m_box;
    QHBoxLayout *m_layout;
};

#endif // TRIPLECHECKBOXWIDGET_H
