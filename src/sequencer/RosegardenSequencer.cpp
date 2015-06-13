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

#include "RosegardenSequencer.h"

//#include <sys/types.h>
//#include <sys/stat.h>
//#include <fcntl.h>
//#include <sys/mman.h>
//#include <unistd.h>
//#include <errno.h>

#include <QVector>

#include "misc/Debug.h"
#include "misc/Strings.h"
#include "sound/ControlBlock.h"
#include "sound/SoundDriver.h"
#include "sound/SoundDriverFactory.h"
#include "sound/MappedInstrument.h"
#include "sound/MappedEventInserter.h"
#include "base/Profiler.h"
#include "sound/PluginFactory.h"

#include "gui/application/RosegardenApplication.h"
#include "gui/application/RosegardenMainWindow.h"

// #define DEBUG_ROSEGARDEN_SEQUENCER

namespace Rosegarden
{

RosegardenSequencer *
RosegardenSequencer::m_instance = 0;

QMutex
RosegardenSequencer::m_instanceMutex;

//#define LOCKED QMutexLocker rgseq_locker(&m_mutex); SEQUENCER_DEBUG << "Locked in " << __PRETTY_FUNCTION__ << " at " << __LINE__

#define LOCKED QMutexLocker rgseq_locker(&m_mutex)

// The default latency and read-ahead values are actually sent
// down from the GUI every time playback or recording starts
// so the local values are kind of meaningless.
//
RosegardenSequencer::RosegardenSequencer() :
    m_driver(0),
    m_transportStatus(STOPPED),
    m_songPosition(0, 0),
    m_lastFetchSongPosition(0, 0),
    m_readAhead(0, 80000000),          // default value
    m_audioMix(0, 60000000),          // default value
    m_audioRead(0, 100000000),          // default value
    m_audioWrite(0, 200000000),          // default value
    m_smallFileSize(128),
    m_loopStart(0, 0),
    m_loopEnd(0, 0),
    m_studio(new MappedStudio()),
    m_transportToken(1),
    m_isEndOfCompReached(false),
    m_mutex(QMutex::Recursive) // recursive
{
    // Initialise the MappedStudio
    //
    initialiseStudio();

    // Creating this object also initialises the Rosegarden ALSA/JACK
    // interface for both playback and recording. MappedStudio
    // audio faders are also created.
    //
    m_driver = SoundDriverFactory::createDriver(m_studio);
    m_studio->setSoundDriver(m_driver);

    if (!m_driver) {
        SEQUENCER_DEBUG << "RosegardenSequencer object could not be allocated"
                        << endl;
        m_transportStatus = QUIT;
        return;
    }

    m_driver->setAudioBufferSizes(m_audioMix, m_audioRead, m_audioWrite,
                                  m_smallFileSize);

    m_driver->setExternalTransportControl(this);
}

RosegardenSequencer::~RosegardenSequencer()
{
    SEQUENCER_DEBUG << "RosegardenSequencer - shutting down" << endl;
    m_driver->shutdown();
    delete m_studio;
    delete m_driver;
}

RosegardenSequencer *
RosegardenSequencer::getInstance()
{
    m_instanceMutex.lock();
    if (!m_instance) {
#ifdef DEBUG_ROSEGARDEN_SEQUENCER        
        SEQUENCER_DEBUG << "RosegardenSequencer::getInstance: Creating" << endl;
#endif
        m_instance = new RosegardenSequencer();
    }
    m_instanceMutex.unlock();
    return m_instance;
}

void
RosegardenSequencer::lock()
{
    m_mutex.lock();
}

void
RosegardenSequencer::unlock()
{
    m_mutex.unlock();
}

// "Public" (ex-DCOP, locks required) functions first

void
RosegardenSequencer::quit()
{
    LOCKED;

#ifdef DEBUG_ROSEGARDEN_SEQUENCER        
    SEQUENCER_DEBUG << "RosegardenSequencer::quit()";
#endif
    // and break out of the loop next time around
    m_transportStatus = QUIT;
}


// We receive a starting time from the GUI which we use as the
// basis of our first fetch of events from the GUI core.  Assuming
// this works we set our internal state to PLAYING and go ahead
// and play the piece until we get a signal to stop.
//
bool
RosegardenSequencer::play(const RealTime &time,
                             const RealTime &readAhead,
                             const RealTime &audioMix,
                             const RealTime &audioRead,
                             const RealTime &audioWrite,
                             long smallFileSize)
{
    LOCKED;

    if (m_transportStatus == PLAYING ||
        m_transportStatus == STARTING_TO_PLAY)
        return true;

    // Check for record toggle (punch out)
    //
    if (m_transportStatus == RECORDING) {
        m_transportStatus = PLAYING;
        return punchOut();
    }

    // To play from the given song position sets up the internal
    // play state to "STARTING_TO_PLAY" which is then caught in
    // the main event loop
    //
    m_songPosition = time;

    SequencerDataBlock::getInstance()->setPositionPointer(m_songPosition);

    if (m_transportStatus != RECORDING &&
        m_transportStatus != STARTING_TO_RECORD) {
        m_transportStatus = STARTING_TO_PLAY;
    }

    m_driver->stopClocks();

    // Set up buffer size
    //
    m_readAhead = readAhead;
    if (m_readAhead == RealTime::zeroTime)
        m_readAhead.sec = 1;

    m_audioMix = audioMix;
    m_audioRead = audioRead;
    m_audioWrite = audioWrite;
    m_smallFileSize = smallFileSize;

    m_driver->setAudioBufferSizes(m_audioMix, m_audioRead, m_audioWrite,
                                  m_smallFileSize);

    // report
    //
#ifdef DEBUG_ROSEGARDEN_SEQUENCER        
    SEQUENCER_DEBUG << "RosegardenSequencer::play() - starting to play\n";
#endif    
//!!!
//    dumpFirstSegment();

    // keep it simple
    return true;
}

bool
RosegardenSequencer::record(const RealTime &time,
                            const RealTime &readAhead,
                            const RealTime &audioMix,
                            const RealTime &audioRead,
                            const RealTime &audioWrite,
                            long smallFileSize,
                            long recordMode)
{
    LOCKED;

    TransportStatus localRecordMode = (TransportStatus) recordMode;

#ifdef DEBUG_ROSEGARDEN_SEQUENCER        
    SEQUENCER_DEBUG << "RosegardenSequencer::record - recordMode is " << recordMode << ", transport status is " << m_transportStatus << endl;
#endif
    // punch in recording
    if (m_transportStatus == PLAYING) {
        if (localRecordMode == STARTING_TO_RECORD) {
#ifdef DEBUG_ROSEGARDEN_SEQUENCER        
            SEQUENCER_DEBUG << "RosegardenSequencer::record: punching in" << endl;
#endif
            localRecordMode = RECORDING; // no need to start playback
        }
    }

    // For audio recording we need to retrieve audio
    // file names from the GUI
    //
    if (localRecordMode == STARTING_TO_RECORD ||
        localRecordMode == RECORDING) {

#ifdef DEBUG_ROSEGARDEN_SEQUENCER        
        SEQUENCER_DEBUG << "RosegardenSequencer::record()"
                        << " - starting to record" << endl;
#endif
        // This function is (now) called synchronously from the GUI
        // thread, which is why we needed to obtain the sequencer lock
        // above.  This means we can safely call back into GUI
        // functions, so long as we don't call anything that will need
        // to call any other locking sequencer functions.

        QVector<InstrumentId> armedInstruments =
            RosegardenMainWindow::self()->getArmedInstruments();

        QVector<InstrumentId> audioInstruments;
        for (int i = 0; i < armedInstruments.size(); ++i) {
            if (armedInstruments[i] >= AudioInstrumentBase &&
                armedInstruments[i] < MidiInstrumentBase) {
                audioInstruments.push_back(armedInstruments[i]);
            }
        }

        QVector<QString> audioFileNames;

        if (audioInstruments.size() > 0) {

            audioFileNames =
                RosegardenMainWindow::self()->createRecordAudioFiles
                (audioInstruments);

            if (audioFileNames.size() != audioInstruments.size()) {
#ifdef DEBUG_ROSEGARDEN_SEQUENCER        
                SEQUENCER_DEBUG << "ERROR: RosegardenSequencer::record(): Failed to create correct number of audio files (wanted " << audioInstruments.size() << ", got " << audioFileNames.size() << ")";
#endif
                stop();
                return false;
            }
        }

        std::vector<InstrumentId> armedInstrumentsVec;
        std::vector<QString> audioFileNamesVec;
        for (int i = 0; i < armedInstruments.size(); ++i) {
            armedInstrumentsVec.push_back(armedInstruments[i]);
        }
        for (int i = 0; i < audioFileNames.size(); ++i) {
            audioFileNamesVec.push_back(audioFileNames[i]);
        }

        // Get the Sequencer to prepare itself for recording - if
        // this fails we stop.
        //
        if (m_driver->record(RECORD_ON,
                             &armedInstrumentsVec,
                             &audioFileNamesVec) == false) {
            stop();
            return false;
        }
    } else {
        // unrecognised type - return a problem
        return false;
    }

    // Now set the local transport status to the record mode
    //
    //
    m_transportStatus = localRecordMode;

    if (localRecordMode == RECORDING) { // punch in
        return true;
    } else {

        // Ensure that playback is initialised
        //
        m_driver->initialisePlayback(m_songPosition);

        return play(time, readAhead, audioMix, audioRead, audioWrite, smallFileSize);
    }
}

void
RosegardenSequencer::stop()
{
    LOCKED;

    // set our state at this level to STOPPING (pending any
    // unfinished NOTES)
    m_transportStatus = STOPPING;

    // report
    //
#ifdef DEBUG_ROSEGARDEN_SEQUENCER        
    SEQUENCER_DEBUG << "RosegardenSequencer::stop() - stopping" << endl;
#endif
    // process pending NOTE OFFs and stop the Sequencer
    m_driver->stopPlayback();

    // the Sequencer doesn't need to know these once
    // we've stopped.
    //
    m_songPosition.sec = 0;
    m_songPosition.nsec = 0;
    m_lastFetchSongPosition.sec = 0;
    m_lastFetchSongPosition.nsec = 0;

//    cleanupMmapData();

    Profiles::getInstance()->dump();

    incrementTransportToken();
}

bool
RosegardenSequencer::punchOut()
{
    LOCKED;

    // Check for record toggle (punch out)
    //
    if (m_transportStatus == RECORDING) {
        m_driver->punchOut();
        m_transportStatus = PLAYING;
        return true;
    }
    return false;
}

// Sets the Sequencer object and this object to the new time
// from where playback can continue.
//
void
RosegardenSequencer::jumpTo(const RealTime &pos)
{
    LOCKED;

#ifdef DEBUG_ROSEGARDEN_SEQUENCER        
    SEQUENCER_DEBUG << "RosegardenSequencer::jumpTo(" << pos << ")\n";
#endif
    if (pos < RealTime::zeroTime) return;

    m_driver->stopClocks();

    RealTime oldPosition = m_songPosition;

    m_songPosition = m_lastFetchSongPosition = pos;

    SequencerDataBlock::getInstance()->setPositionPointer(m_songPosition);

    m_driver->resetPlayback(oldPosition, m_songPosition);

    if (m_driver->isPlaying()) {

        // Now prebuffer as in startPlaying:

        MappedEventList c;
        fetchEvents(c, m_songPosition, m_songPosition + m_readAhead, true);

        // process whether we need to or not as this also processes
        // the audio queue for us
        //
        m_driver->processEventsOut(c, m_songPosition, m_songPosition + m_readAhead);
    }

    incrementTransportToken();

    //    SEQUENCER_DEBUG << "RosegardenSequencer::jumpTo: pausing to simulate high-load environment" << endl;
    //    ::sleep(1);

    m_driver->startClocks();

    return ;
}

void
RosegardenSequencer::setLoop(const RealTime &loopStart,
                             const RealTime &loopEnd)
{
    LOCKED;

    m_loopStart = loopStart;
    m_loopEnd = loopEnd;

    m_driver->setLoop(loopStart, loopEnd);
}



// Return the status of the sound systems (audio and MIDI)
//
unsigned int
RosegardenSequencer::getSoundDriverStatus(const QString &guiVersion)
{
    LOCKED;

    unsigned int driverStatus = m_driver->getStatus();
    if (guiVersion == VERSION)
        driverStatus |= VERSION_OK;
    else {
        SEQUENCER_DEBUG << "WARNING: RosegardenSequencer::getSoundDriverStatus: "
        << "GUI version \"" << guiVersion
        << "\" does not match sequencer version \"" << VERSION
        << "\"";
    }
    return driverStatus;
}


// Add an audio file to the sequencer
bool
RosegardenSequencer::addAudioFile(const QString &fileName, int id)
{
    LOCKED;
    
    //call SoundDriver->addAudioFile()
    return m_driver->addAudioFile(fileName.toUtf8().data(), id);
}

bool
RosegardenSequencer::removeAudioFile(int id)
{
    LOCKED;

    return m_driver->removeAudioFile(id);
}

void
RosegardenSequencer::clearAllAudioFiles()
{
    LOCKED;

    m_driver->clearAudioFiles();
}

void
RosegardenSequencer::setMappedInstrument(int type, unsigned int id)
{
    LOCKED;

    InstrumentId mID = (InstrumentId)id;
    Instrument::InstrumentType mType =
        (Instrument::InstrumentType)type;

    m_driver->setMappedInstrument(
        new MappedInstrument (mType, 0, mID));

}

void
RosegardenSequencer::processMappedEvent(MappedEvent mE)
{
    QMutexLocker locker(&m_asyncQueueMutex);
    m_asyncOutQueue.push_back(new MappedEvent(mE));
//    SEQUENCER_DEBUG << "processMappedEvent: Have " << m_asyncOutQueue.size()
//                    << " events in async out queue" << endl;
}

int
RosegardenSequencer::canReconnect(Device::DeviceType type)
{
    LOCKED;

    return m_driver->canReconnect(type);
}

bool
RosegardenSequencer::addDevice(Device::DeviceType type,
                               DeviceId id,
                               InstrumentId baseInstrumentId,
                               MidiDevice::DeviceDirection direction)
{
    LOCKED;

    return m_driver->addDevice(type, id, baseInstrumentId, direction);
}

void
RosegardenSequencer::removeDevice(unsigned int deviceId)
{
    LOCKED;

    m_driver->removeDevice(deviceId);
}

void
RosegardenSequencer::removeAllDevices()
{
    LOCKED;

    m_driver->removeAllDevices();
}

void
RosegardenSequencer::renameDevice(unsigned int deviceId, QString name)
{
    LOCKED;

    m_driver->renameDevice(deviceId, name);
}

unsigned int
RosegardenSequencer::getConnections(Device::DeviceType type,
                                    MidiDevice::DeviceDirection direction)
{
    LOCKED;

    return m_driver->getConnections(type, direction);
}

QString
RosegardenSequencer::getConnection(Device::DeviceType type,
                                   MidiDevice::DeviceDirection direction,
                                   unsigned int connectionNo)
{
    LOCKED;

    return m_driver->getConnection(type, direction, connectionNo);
}

QString
RosegardenSequencer::getConnection(DeviceId id)
{
    LOCKED;

    return m_driver->getConnection(id);
}

void
RosegardenSequencer::setConnection(unsigned int deviceId,
                                   QString connection)
{
    LOCKED;

    m_driver->setConnection(deviceId, connection);
}

void
RosegardenSequencer::setPlausibleConnection(unsigned int deviceId,
                                            QString connection)
{
    LOCKED;

    bool recordDevice = false; // we only want this from connectSomething()
    m_driver->setPlausibleConnection(deviceId, connection, recordDevice);
}

void
RosegardenSequencer::connectSomething()
{
    LOCKED;
    
    m_driver->connectSomething();
}

unsigned int
RosegardenSequencer::getTimers()
{
    LOCKED;

    return m_driver->getTimers();
}

QString
RosegardenSequencer::getTimer(unsigned int n)
{
    LOCKED;

    return m_driver->getTimer(n);
}

QString
RosegardenSequencer::getCurrentTimer()
{
    LOCKED;

    return m_driver->getCurrentTimer();
}

void
RosegardenSequencer::setCurrentTimer(QString timer)
{
    LOCKED;

    m_driver->setCurrentTimer(timer);
}

void
RosegardenSequencer::setLowLatencyMode(bool ll)
{
    LOCKED;

    m_driver->setLowLatencyMode(ll);
}

RealTime
RosegardenSequencer::getAudioPlayLatency()
{
    LOCKED;

    return m_driver->getAudioPlayLatency();
}

RealTime
RosegardenSequencer::getAudioRecordLatency()
{
    LOCKED;

    return m_driver->getAudioRecordLatency();
}


void
RosegardenSequencer::setMappedProperty(int id,
        const QString &property,
        float value)
{
    LOCKED;


    //    SEQUENCER_DEBUG << "setProperty: id = " << id
    //                    << " : property = \"" << property << "\""
    //		    << ", value = " << value << endl;


    MappedObject *object = m_studio->getObjectById(id);

    if (object)
        object->setProperty(property, value);
}

void
RosegardenSequencer::setMappedProperties(const MappedObjectIdList &ids,
        const MappedObjectPropertyList &properties,
        const MappedObjectValueList &values)
{
    LOCKED;

    MappedObject *object = 0;
    MappedObjectId prevId = 0;

    for (size_t i = 0;
            i < ids.size() && i < properties.size() && i < values.size();
            ++i) {

        if (i == 0 || ids[i] != prevId) {
            object = m_studio->getObjectById(ids[i]);
            prevId = ids[i];
        }

        if (object) {
            object->setProperty(properties[i], values[i]);
        }
    }
}

void
RosegardenSequencer::setMappedProperty(int id,
                                       const QString &property,
                                       const QString &value)
{
    LOCKED;

#ifdef DEBUG_ROSEGARDEN_SEQUENCER        
    SEQUENCER_DEBUG << "setProperty: id = " << id
                    << " : property = \"" << property << "\""
                    << ", value = " << value << endl;
#endif
    MappedObject *object = m_studio->getObjectById(id);

    if (object) object->setStringProperty(property, value);
}

QString
RosegardenSequencer::setMappedPropertyList(int id, const QString &property,
        const MappedObjectPropertyList &values)
{
    LOCKED;

#ifdef DEBUG_ROSEGARDEN_SEQUENCER        
    SEQUENCER_DEBUG << "setPropertyList: id = " << id
                    << " : property list size = \"" << values.size()
                    << "\"" << endl;
#endif
    MappedObject *object = m_studio->getObjectById(id);

    if (object) {
        try {
            object->setPropertyList(property, values);
        } catch (QString err) {
            return err;
        }
        return "";
    }

//    return "(object not found)";

    //!!! This is where the "object not found" error is coming from when changing
    // the category combo.  I suspect something isn't wired quite right in here
    // somewhere in the chain, and that's what's causing this error to come up,
    // but testing with this simply disabled, everything seems to be working as
    // expected if we ignore the error and move right along.  I have to admit I
    // have only a very tenuous grasp on any of this, however.
    return "";
}

int
RosegardenSequencer::getMappedObjectId(int type)
{
    LOCKED;

    int value = -1;

    MappedObject *object =
        m_studio->getObjectOfType(
            MappedObject::MappedObjectType(type));

    if (object) {
        value = int(object->getId());
    }

    return value;
}


std::vector<QString>
RosegardenSequencer::getPropertyList(int id,
                                     const QString &property)
{
    LOCKED;

    std::vector<QString> list;

    MappedObject *object =
        m_studio->getObjectById(id);

    if (object) {
        list = object->getPropertyList(property);
    }

#ifdef DEBUG_ROSEGARDEN_SEQUENCER        
    SEQUENCER_DEBUG << "getPropertyList - return " << list.size()
                    << " items" << endl;
#endif
    return list;
}

std::vector<QString>
RosegardenSequencer::getPluginInformation()
{
    LOCKED;

    std::vector<QString> list;

    PluginFactory::enumerateAllPlugins(list);

    return list;
}

QString
RosegardenSequencer::getPluginProgram(int id, int bank, int program)
{
    LOCKED;

    MappedObject *object = m_studio->getObjectById(id);

    if (object) {
        MappedPluginSlot *slot =
            dynamic_cast<MappedPluginSlot *>(object);
        if (slot) {
            return slot->getProgram(bank, program);
        }
    }

    return QString();
}

unsigned long
RosegardenSequencer::getPluginProgram(int id, const QString &name)
{
    LOCKED;

    MappedObject *object = m_studio->getObjectById(id);

    if (object) {
        MappedPluginSlot *slot =
            dynamic_cast<MappedPluginSlot *>(object);
        if (slot) {
            return slot->getProgram(name);
        }
    }

    return 0;
}

void
RosegardenSequencer::setMappedPort(int pluginId,
                                      unsigned long portId,
                                      float value)
{
    LOCKED;

    MappedObject *object =
        m_studio->getObjectById(pluginId);

    MappedPluginSlot *slot =
        dynamic_cast<MappedPluginSlot *>(object);

    if (slot) {
        slot->setPort(portId, value);
    } else {
#ifdef DEBUG_ROSEGARDEN_SEQUENCER        
        SEQUENCER_DEBUG << "no such slot" << endl;
#endif
    }
}

float
RosegardenSequencer::getMappedPort(int pluginId,
                                      unsigned long portId)
{
    LOCKED;

    MappedObject *object =
        m_studio->getObjectById(pluginId);

    MappedPluginSlot *slot =
        dynamic_cast<MappedPluginSlot *>(object);

    if (slot) {
        return slot->getPort(portId);
    } else {
#ifdef DEBUG_ROSEGARDEN_SEQUENCER        
        SEQUENCER_DEBUG << "no such slot" << endl;
#endif
    }

    return 0;
}

// Creates an object of a type
//
int
RosegardenSequencer::createMappedObject(int type)
{
    LOCKED;

    MappedObject *object =
        m_studio->createObject(MappedObject::MappedObjectType(type));

    if (object) {
#ifdef DEBUG_ROSEGARDEN_SEQUENCER        
        SEQUENCER_DEBUG << "createMappedObject - type = "
                        << type << ", object id = "
                        << object->getId() << endl;
#endif
        return object->getId();
    }

    return 0;
}

// Destroy an object
//
bool
RosegardenSequencer::destroyMappedObject(int id)
{
    LOCKED;

    return m_studio->destroyObject(MappedObjectId(id));
}

// Connect two objects
//
void
RosegardenSequencer::connectMappedObjects(int id1, int id2)
{
    LOCKED;

    m_studio->connectObjects(MappedObjectId(id1),
                             MappedObjectId(id2));

    // When this happens we need to resynchronise our audio processing,
    // and this is the easiest (and most brutal) way to do it.
    if (m_transportStatus == PLAYING ||
            m_transportStatus == RECORDING) {
        RealTime seqTime = m_driver->getSequencerTime();
        jumpTo(seqTime);
    }
}

// Disconnect two objects
//
void
RosegardenSequencer::disconnectMappedObjects(int id1, int id2)
{
    LOCKED;

    m_studio->disconnectObjects(MappedObjectId(id1),
                                MappedObjectId(id2));
}

// Disconnect an object from everything
//
void
RosegardenSequencer::disconnectMappedObject(int id)
{
    LOCKED;

    m_studio->disconnectObject(MappedObjectId(id));
}

unsigned int
RosegardenSequencer::getSampleRate() const
{
    QMutexLocker locker(const_cast<QMutex *>(&m_mutex));

    if (m_driver) return m_driver->getSampleRate();

    return 0;
}

void
RosegardenSequencer::clearStudio()
{
    LOCKED;

#ifdef DEBUG_ROSEGARDEN_SEQUENCER        
    SEQUENCER_DEBUG << "clearStudio()" << endl;
#endif
    m_studio->clear();
    SequencerDataBlock::getInstance()->clearTemporaries();

}

// Set the MIDI Clock period in microseconds
//
void
RosegardenSequencer::setQuarterNoteLength(RealTime rt)
{
    LOCKED;

#ifdef DEBUG_ROSEGARDEN_SEQUENCER        
    SEQUENCER_DEBUG << "RosegardenSequencer::setQuarterNoteLength"
                    << rt << endl;
#endif
    m_driver->setMIDIClockInterval(rt / 24);
}

QString
RosegardenSequencer::getStatusLog()
{
    LOCKED;

    return m_driver->getStatusLog();
}


void RosegardenSequencer::dumpFirstSegment()
{
    LOCKED;

    SEQUENCER_DEBUG << "Dumping 1st segment data :" << endl;

    unsigned int i = 0;
    
    std::set<MappedEventBuffer *> segs = m_metaIterator.getSegments();
    if (segs.empty()) {
        SEQUENCER_DEBUG << "(no segments)" << endl;
        return;
    }

    MappedEventBuffer *firstMappedEventBuffer = *segs.begin();

    MappedEventBuffer::iterator it(firstMappedEventBuffer);

    for (; !it.atEnd(); ++it) {

        MappedEvent evt = (*it);
        SEQUENCER_DEBUG << i << " : inst = " << evt.getInstrument()
                        << " - type = " << evt.getType()
                        << " - data1 = " << (unsigned int)evt.getData1()
                        << " - data2 = " << (unsigned int)evt.getData2()
                        << " - time = " << evt.getEventTime()
                        << " - duration = " << evt.getDuration()
                        << " - audio mark = " << evt.getAudioStartMarker()
                        << endl;
        
        ++i;
    }

    SEQUENCER_DEBUG << "Dumping 1st segment data - done\n";

}

void
RosegardenSequencer::segmentModified(MappedEventBuffer *mapper)
{
    if (!mapper) return;

 #ifdef DEBUG_ROSEGARDEN_SEQUENCER        
   SEQUENCER_DEBUG << "RosegardenSequencer::segmentModified(" << mapper << ")\n";
#endif
   LOCKED;
   /* We don't force an immediate rewind while recording.  It would be
      "the right thing" soundwise, but historically we haven't,
      there's been no demand and nobody knows what subtle problems
      might be introduced. */
   bool immediate = (m_transportStatus == PLAYING);
   m_metaIterator.resetIteratorForSegment(mapper, immediate);
}

void
RosegardenSequencer::segmentAdded(MappedEventBuffer *mapper)
{
    if (!mapper) return;

    LOCKED;

#ifdef DEBUG_ROSEGARDEN_SEQUENCER        
    SEQUENCER_DEBUG << "RosegardenSequencer::segmentAdded(" << mapper << ")\n";
#endif
    // m_metaIterator takes ownership of the mapper, shared with other
    // MappedBufMetaIterators
    m_metaIterator.addSegment(mapper);
}

void
RosegardenSequencer::segmentAboutToBeDeleted(MappedEventBuffer *mapper)
{
    if (!mapper) return;

    LOCKED;

#ifdef DEBUG_ROSEGARDEN_SEQUENCER        
    SEQUENCER_DEBUG << "RosegardenSequencer::segmentAboutToBeDeleted(" << mapper << ")\n";
#endif
    
    // This deletes mapper just if no other metaiterator owns it.
    m_metaIterator.removeSegment(mapper);
}

void
RosegardenSequencer::compositionAboutToBeDeleted()
{
    LOCKED;

#ifdef DEBUG_ROSEGARDEN_SEQUENCER        
    SEQUENCER_DEBUG << "RosegardenSequencer::compositionAboutToBeDeleted()\n";
#endif
    m_metaIterator.clear();
}

void
RosegardenSequencer::remapTracks()
{
    LOCKED;

#ifdef DEBUG_ROSEGARDEN_SEQUENCER        
    SEQUENCER_DEBUG << "RosegardenSequencer::remapTracks";
#endif
    rationalisePlayingAudio();
}

bool
RosegardenSequencer::getNextTransportRequest(TransportRequest &request,
                                             RealTime &time)
{
    QMutexLocker locker(&m_transportRequestMutex);

    if (m_transportRequests.empty()) return false;
    TransportPair pair = *m_transportRequests.begin();
    m_transportRequests.pop_front();
    request = pair.first;
    time = pair.second;

    //!!! review transport token management -- jumpToTime has an
    // extra incrementTransportToken() below

    return true;  // fix "control reaches end of non-void function warning"
}

MappedEventList
RosegardenSequencer::pullAsynchronousMidiQueue()
{
    QMutexLocker locker(&m_asyncQueueMutex);
    MappedEventList mq = m_asyncInQueue;
    m_asyncInQueue = MappedEventList();
    return mq;
}

// END of public API



// Get a slice of events from the composition into a MappedEventList.
void
RosegardenSequencer::fetchEvents(MappedEventList &mappedEventList,
                                    const RealTime &start,
                                    const RealTime &end,
                                    bool firstFetch)
{
    // Always return nothing if we're stopped
    //
    if ( m_transportStatus == STOPPED || m_transportStatus == STOPPING )
        return ;

    getSlice(mappedEventList, start, end, firstFetch);
    applyLatencyCompensation(mappedEventList);
}


void
RosegardenSequencer::getSlice(MappedEventList &mappedEventList,
                                 const RealTime &start,
                                 const RealTime &end,
                                 bool firstFetch)
{
    //    SEQUENCER_DEBUG << "RosegardenSequencer::getSlice (" << start << " -> " << end << ", " << firstFetch << ")" << endl;

    if (firstFetch || (start < m_lastStartTime)) {
#ifdef DEBUG_ROSEGARDEN_SEQUENCER        
        SEQUENCER_DEBUG << "[calling jumpToTime on start]" << endl;
#endif
        m_metaIterator.jumpToTime(start);
    }

    MappedEventInserter inserter(mappedEventList);

    m_metaIterator.fetchEvents(inserter, start, end);

    // don't do this, it breaks recording because
    // playing stops right after it starts.
//  m_isEndOfCompReached = eventsRemaining;

    m_lastStartTime = start;
}


void
RosegardenSequencer::applyLatencyCompensation(MappedEventList &mappedEventList)
{
    RealTime maxLatency = m_driver->getMaximumPlayLatency();
    if (maxLatency == RealTime::zeroTime)
        return ;

    for (MappedEventList::iterator i = mappedEventList.begin();
            i != mappedEventList.end(); ++i) {

        RealTime instrumentLatency =
            m_driver->getInstrumentPlayLatency((*i)->getInstrument());

        //	SEQUENCER_DEBUG << "RosegardenSequencer::applyLatencyCompensation: maxLatency " << maxLatency << ", instrumentLatency " << instrumentLatency << ", moving " << (*i)->getEventTime() << " to " << (*i)->getEventTime() + maxLatency - instrumentLatency;

        (*i)->setEventTime((*i)->getEventTime() +
                           maxLatency - instrumentLatency);
    }
}


// The first fetch of events from the core/ and initialisation for
// this session of playback.  We fetch up to m_readAhead ahead at
// first at then top up at each slice.
//
bool
RosegardenSequencer::startPlaying()
{
    // Fetch up to m_readAhead microseconds worth of events
    m_lastFetchSongPosition = m_songPosition + m_readAhead;

    // This will reset the Sequencer's internal clock
    // ready for new playback
    m_driver->initialisePlayback(m_songPosition);

    MappedEventList c;
    fetchEvents(c, m_songPosition, m_songPosition + m_readAhead, true);

    // process whether we need to or not as this also processes
    // the audio queue for us
    m_driver->processEventsOut(c, m_songPosition, m_songPosition + m_readAhead);

    std::vector<MappedEvent> audioEvents;
    m_metaIterator.getAudioEvents(audioEvents);
    m_driver->initialiseAudioQueue(audioEvents);

    //SEQUENCER_DEBUG << "RosegardenSequencer::startPlaying: pausing to simulate high-load environment" << endl;
    //::sleep(2);

    // and only now do we signal to start the clock
    m_driver->startClocks();

    incrementTransportToken();

    return true; // !m_isEndOfCompReached;
}

bool
RosegardenSequencer::keepPlaying()
{
    Profiler profiler("RosegardenSequencer::keepPlaying");

    MappedEventList c;

    RealTime fetchEnd = m_songPosition + m_readAhead;
    if (isLooping() && fetchEnd >= m_loopEnd) {
        fetchEnd = m_loopEnd - RealTime(0, 1);
    }
    if (fetchEnd > m_lastFetchSongPosition) {
        fetchEvents(c, m_lastFetchSongPosition, fetchEnd, false);
    }

    // Again, process whether we need to or not to keep
    // the Sequencer up-to-date with audio events
    //
    m_driver->processEventsOut(c, m_lastFetchSongPosition, fetchEnd);

    if (fetchEnd > m_lastFetchSongPosition) {
        m_lastFetchSongPosition = fetchEnd;
    }

    return true; // !m_isEndOfCompReached; - until we sort this out, we don't stop at end of comp.
}

// Return current Sequencer time in GUI compatible terms
//
void
RosegardenSequencer::updateClocks()
{
    Profiler profiler("RosegardenSequencer::updateClocks");

    m_driver->runTasks();

    //SEQUENCER_DEBUG << "RosegardenSequencer::updateClocks" << endl;

    // If we're not playing etc. then that's all we need to do
    //
    if (m_transportStatus != PLAYING &&
            m_transportStatus != RECORDING)
        return ;

    RealTime newPosition = m_driver->getSequencerTime();

    // Go around the loop if we've reached the end
    //
    if (isLooping() && newPosition >= m_loopEnd) {

        RealTime oldPosition = m_songPosition;

        // Remove the loop width from the song position and send
        // this position to the GUI
        //
        newPosition = m_songPosition = m_lastFetchSongPosition = m_loopStart;

        m_driver->stopClocks();

        // Reset playback using this jump
        //
        m_driver->resetPlayback(oldPosition, m_songPosition);

        MappedEventList c;
        fetchEvents(c, m_songPosition, m_songPosition + m_readAhead, true);

        m_driver->processEventsOut(c, m_songPosition, m_songPosition + m_readAhead);

        m_driver->startClocks();
    } else {
        m_songPosition = newPosition;

        if (m_songPosition <= m_driver->getStartPosition())
            newPosition = m_driver->getStartPosition();
    }

    RealTime maxLatency = m_driver->getMaximumPlayLatency();
    if (maxLatency != RealTime::zeroTime) {
        //	SEQUENCER_DEBUG << "RosegardenSequencer::updateClocks: latency compensation moving " << newPosition << " to " << newPosition - maxLatency;
        newPosition = newPosition - maxLatency;
    }

    // Remap the position pointer
    //
    SequencerDataBlock::getInstance()->setPositionPointer(newPosition);
}

void
RosegardenSequencer::sleep(const RealTime &rt)
{
    m_driver->sleep(rt);
}

void
RosegardenSequencer::processRecordedMidi()
{
#ifdef DEBUG_ROSEGARDEN_SEQUENCER
    SEQUENCER_DEBUG << "RosegardenSequencer::processRecordedMidi";
#endif

    MappedEventList recordList;

    // Get the MIDI events from the ALSA driver
    m_driver->getMappedEventList(recordList);

    if (recordList.empty()) return;

    // Handle "thru" first to reduce latency.

    if (ControlBlock::getInstance()->isMidiRoutingEnabled()) {
        // Make a copy so we don't mess up the list for recording.
        MappedEventList thruList = recordList;

        // Remove events that match the thru filter
        applyFiltering(&thruList, ControlBlock::getInstance()->getThruFilter(), true);

        // Route the MIDI thru events to MIDI out.  Use the instrument and
        // track information from each event.
        routeEvents(&thruList, false);
    }

#ifdef DEBUG_ROSEGARDEN_SEQUENCER
    SEQUENCER_DEBUG << "RosegardenSequencer::processRecordedMidi: have " << mC.size() << " events";
#endif

    // Remove events that match the record filter
    applyFiltering(&recordList, ControlBlock::getInstance()->getRecordFilter(), false);

    // Store the events
    SequencerDataBlock::getInstance()->addRecordedEvents(&recordList);
}

void
RosegardenSequencer::routeEvents(MappedEventList *mC, bool useSelectedTrack)
{
    ControlBlock *control = ControlBlock::getInstance();
    if (useSelectedTrack) {
        InstrumentAndChannel 
            info = control->getInsAndChanForSelectedTrack();
        
        for (MappedEventList::iterator i = mC->begin();
                i != mC->end(); ++i) {
            (*i)->setInstrument(info.m_id);
            (*i)->setRecordedChannel(info.m_channel);
        }
    } else {
        for (MappedEventList::iterator i = mC->begin();
                i != mC->end(); ++i) {
            InstrumentAndChannel 
                info = control->getInsAndChanForEvent
                ((*i)->getRecordedDevice(), (*i)->getRecordedChannel());
            
            (*i)->setInstrument(info.m_id);
            (*i)->setRecordedChannel(info.m_channel);
        }
    }
    m_driver->processEventsOut(*mC);
}

// Send an update
//
void
RosegardenSequencer::processRecordedAudio()
{
    // Nothing to do here: the recording time is sent back to the GUI
    // in the sequencer mapper as a normal case.
}

void
RosegardenSequencer::processAsynchronousEvents()
{
    // outgoing ad-hoc async events
    std::deque<MappedEvent *> q;
    m_asyncQueueMutex.lock();
    if (!m_asyncOutQueue.empty()) {
        q = m_asyncOutQueue;
        m_asyncOutQueue.clear();
#ifdef DEBUG_ROSEGARDEN_SEQUENCER        
        SEQUENCER_DEBUG << "processAsynchronousEvents: Have " << q.size()
                        << " events in async out queue" << endl;
#endif
    }
    m_asyncQueueMutex.unlock();
    MappedEventList mC;
    while (!q.empty()) {
        mC.insert(q.front());
        m_driver->processEventsOut(mC);
        q.pop_front();
        mC.clear();
    }

    // incoming ad-hoc async events
    m_driver->getMappedEventList(mC);
    if (!mC.empty()) {
        m_asyncQueueMutex.lock();
        m_asyncInQueue.merge(mC);
        m_asyncQueueMutex.unlock();
        if (ControlBlock::getInstance()->isMidiRoutingEnabled()) {
            applyFiltering(&mC, ControlBlock::getInstance()->getThruFilter(), true);
            // Send the incoming events back out using the instrument and
            // track for the selected track.
            routeEvents(&mC, true);
        }
    }

    // Process any pending events (Note Offs or Audio) as part of same
    // procedure.
    //
    m_driver->processPending();
}

void
RosegardenSequencer::applyFiltering(MappedEventList *mC,
                                       MidiFilter filter,
                                       bool filterControlDevice)
{
    // For each event in the list
    for (MappedEventList::iterator i = mC->begin();
         i != mC->end();
         /* increment in loop */) {

        // Hold on to the current event for processing.
        MappedEventList::iterator j = i;
        // Move to the next in case the current is erased.
        ++i;

        // If this event matches the filter, erase it from the list
        if (((*j)->getType() & filter) ||
                (filterControlDevice && ((*j)->getRecordedDevice() ==
                                         Device::CONTROL_DEVICE))) {
            mC->erase(j);
        }
    }
}

// Initialise the virtual studio with a few audio faders and
// create a plugin manager.  For the moment this is pretty
// arbitrary but eventually we'll drive this from the gui
// and rg file "Studio" entries.
//
void
RosegardenSequencer::initialiseStudio()
{
    // clear down the studio before we start adding anything
    //
    m_studio->clear();
}

void
RosegardenSequencer::checkForNewClients()
{
    // Don't do this check if any of these conditions hold
    //
    if (m_transportStatus == PLAYING ||
        m_transportStatus == RECORDING)
        return ;

    if (m_driver->checkForNewClients()) {
#ifdef DEBUG_ROSEGARDEN_SEQUENCER        
        SEQUENCER_DEBUG << "client list changed" << endl;
#endif
    }
}


void
RosegardenSequencer::rationalisePlayingAudio()
{
    std::vector<MappedEvent> audioEvents;
    m_metaIterator.getAudioEvents(audioEvents);
    m_driver->initialiseAudioQueue(audioEvents);
}


ExternalTransport::TransportToken
RosegardenSequencer::transportChange(TransportRequest request)
{
    QMutexLocker locker(&m_transportRequestMutex);

    TransportPair pair(request, RealTime::zeroTime);
    m_transportRequests.push_back(pair);

#ifdef DEBUG_ROSEGARDEN_SEQUENCER        
    SEQUENCER_DEBUG << "RosegardenSequencer::transportChange: " << request;
#endif
    if (request == TransportNoChange)
        return m_transportToken;
    else
        return m_transportToken + 1;
}

ExternalTransport::TransportToken
RosegardenSequencer::transportJump(TransportRequest request,
                                      RealTime rt)
{
    QMutexLocker locker(&m_transportRequestMutex);

    TransportPair pair(request, rt);
    m_transportRequests.push_back(pair);

#ifdef DEBUG_ROSEGARDEN_SEQUENCER        
    SEQUENCER_DEBUG << "RosegardenSequencer::transportJump: " << request << ", " << rt;
#endif
    if (request == TransportNoChange)
        return m_transportToken + 1;
    else
        return m_transportToken + 2;
}

bool
RosegardenSequencer::isTransportSyncComplete(TransportToken token)
{
    QMutexLocker locker(&m_transportRequestMutex);

#ifdef DEBUG_ROSEGARDEN_SEQUENCER        
    SEQUENCER_DEBUG << "RosegardenSequencer::isTransportSyncComplete: token " << token << ", current token " << m_transportToken;
#endif
    return m_transportToken >= token;
}

void
RosegardenSequencer::incrementTransportToken()
{
    ++m_transportToken;
#ifdef DEBUG_ROSEGARDEN_SEQUENCER        
    SEQUENCER_DEBUG << "RosegardenSequencer::incrementTransportToken: incrementing to " << m_transportToken << endl;
#endif
}

}