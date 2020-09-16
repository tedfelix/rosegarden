/* -*- c-basic-offset: 4 indent-tabs-mode: nil -*- vi:set ts=8 sts=4 sw=4: */

/*
    Rosegarden
    A MIDI and audio sequencer and musical notation editor.
    Copyright 2000-2018 the Rosegarden development team.

    Other copyrights also apply to some parts of this work.  Please
    see the AUTHORS file and individual file headers for details.

    This program is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License as
    published by the Free Software Foundation; either version 2 of the
    License, or (at your option) any later version.  See the file
    COPYING included with this distribution for more information.
*/

#ifndef RG_MIDICONFIGURATIONPAGE_H
#define RG_MIDICONFIGURATIONPAGE_H

#include "TabbedConfigurationPage.h"

#include <QString>
#include <QCheckBox>

class QComboBox;
class QPushButton;
class QSpinBox;
class QWidget;


namespace Rosegarden
{


class RosegardenDocument;
class LineEdit;


class MIDIConfigurationPage : public TabbedConfigurationPage
{
    Q_OBJECT

public:
    MIDIConfigurationPage(RosegardenDocument *doc, QWidget *parent);

    void apply() override;

    // Info for ConfigureDialog.
    static QString iconLabel()  { return tr("MIDI"); }
    static QString title()  { return tr("MIDI Settings"); }
    static QString iconName()  { return "configure-midi"; }

private slots:

    void slotLoadSoundFontClicked(bool);
    void slotPathToLoadChoose();
    void slotSoundFontChoose();

private:

    // ??? These widget names need to match the UI.

    // *** General tab

    /// Base octave number for MIDI pitch display.
    QSpinBox *m_baseOctaveNumber;

    QCheckBox *m_useDefaultStudio;
    bool getUseDefaultStudio() const  { return m_useDefaultStudio->isChecked(); }
    QCheckBox *m_allowResetAllControllers;
    /// Timer value at the beginning to detect changes.
    QString m_originalTimingSource;
    QComboBox *m_sequencerTimingSource;

    QCheckBox *m_loadSoundFont;
    LineEdit *m_pathToLoadCommand;
    QPushButton *m_pathToLoadChoose;
    LineEdit *m_soundFont;
    QPushButton *m_soundFontChoose;


    // *** MIDI Sync tab

    QComboBox *m_midiSync;
    QComboBox *m_mmcTransport;
    QComboBox *m_mtcTransport;
    QCheckBox *m_midiSyncAuto;

};


}

#endif
