/* -*- c-basic-offset: 4 indent-tabs-mode: nil -*- vi:set ts=8 sts=4 sw=4: */

/*
    Rosegarden
    A MIDI and audio sequencer and musical notation editor.
    Copyright 2000-2015 the Rosegarden development team.

    Other copyrights also apply to some parts of this work.  Please
    see the AUTHORS file and individual file headers for details.

    This program is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License as
    published by the Free Software Foundation; either version 2 of the
    License, or (at your option) any later version.  See the file
    COPYING included with this distribution for more information.
*/

#ifndef RG_CONTROLRULERTABBAR_H
#define RG_CONTROLRULERTABBAR_H

#include <QTabBar>
#include <QString>

namespace Rosegarden
{

class ControlRulerTabBar : public QTabBar
{
    Q_OBJECT

public:
    ControlRulerTabBar();
    virtual int addTab(const QString &text);
    virtual void paintEvent(QPaintEvent *);
    virtual void mousePressEvent(QMouseEvent *);
//    virtual void handleLeftButtonPress(const ControlMouseEvent *);

signals:
    void tabCloseRequest(int);
    
protected slots:

protected:
    virtual void tabLayoutChange();
    QPixmap m_closeIcon;
    std::vector <QRect*> m_closeButtons;
    static const int hMargin = 5;
};

}

#endif