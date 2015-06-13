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

#include "InternalSegmentMapper.h"
#include "base/BaseProperties.h"
#include "base/Composition.h"
#include "base/ControllerContext.h"
#include "base/Event.h"
#include "base/Exception.h"
#include "base/MidiTypes.h"
#include "base/NotationTypes.h"
#include "base/Profiler.h"
#include "base/RealTime.h"
#include "base/Segment.h"
#include "base/SegmentPerformanceHelper.h"
#include "base/TriggerSegment.h"
#include "document/RosegardenDocument.h"
#include "misc/Debug.h"
#include "sound/ControlBlock.h"
#include "sound/MappedEvent.h"
#include "sound/Midi.h" // For MIDI_SYSTEM_EXCLUSIVE

#include <limits>
#include <algorithm>

// #define DEBUG_INTERNAL_SEGMENT_MAPPER 1

namespace Rosegarden
{

InternalSegmentMapper::InternalSegmentMapper(RosegardenDocument *doc,
                                             Segment *segment)
    : SegmentMapper(doc, segment),
      m_channelManager(doc->getInstrument(segment)),
      m_triggeredEvents(new Segment)
{}

InternalSegmentMapper::
~InternalSegmentMapper(void)
{
    if(m_triggeredEvents) { delete m_triggeredEvents; }
}

RealTime
InternalSegmentMapper::
toRealTime(Composition &comp, timeT t)
{
    return 
        comp.getElapsedRealTime(t) + m_segment->getRealTimeDelay();
}


void InternalSegmentMapper::fillBuffer()
{
    Composition &comp = m_doc->getComposition();
    Track* track = comp.getTrackById(m_segment->getTrack());
#ifdef DEBUG_INTERNAL_SEGMENT_MAPPER
    SEQUENCER_DEBUG
        << "InternalSegmentMapper::fillBuffer track number"
        << (int)track->getId()
        << endl;
#endif

    timeT segmentStartTime = m_segment->getStartTime();
    timeT segmentEndTime = m_segment->getEndMarkerTime();
    timeT segmentDuration = segmentEndTime - segmentStartTime;
    timeT repeatEndTime = segmentEndTime;

    int repeatCount = getSegmentRepeatCount();

    if (repeatCount > 0)
        repeatEndTime = m_segment->getRepeatEndTime();

    resize(0);

#ifdef DEBUG_INTERNAL_SEGMENT_MAPPER
    SEQMAN_DEBUG
        << "InternalSegmentMapper::fillBuffer"
        << (void *)this
        << "Segment"
        << (void *)m_segment
        << "with repeat count"
        << repeatCount
        << endl;
#endif
    
    // Clear out stuff from before.
    m_triggeredEvents->clear(); 
    m_controllerCache.clear();
    m_noteOffs = NoteoffContainer();

    for (int repeatNo = 0; repeatNo <= repeatCount; ++repeatNo) {

        // For triggered segments.  We write their notes into
        // *m_triggeredEvents and then process those notes at their
        // appropriate times.  implied iterates over
        // *m_triggeredEvents.
        Segment::iterator implied     = m_triggeredEvents->begin();
        // The delay in performance time due to which repeat we are
        // on.  Eg, on the second time thru we play everything one
        // segment duration later and so forth.
        timeT timeForRepeats = repeatNo * segmentDuration;
        
        for (Segment::iterator j = m_segment->begin();
             m_segment->isBeforeEndMarker(j) ||
                 (implied != m_triggeredEvents->end());
             // No step here.  We'll step the appropriate iterator
             // later in the loop.
             ) {
            bool usingImplied = false;
            // timeT of the best candidate, treated as if the first
            // time thru.  Timing for repeats will be handled later.
            timeT bestBaseTime = std::numeric_limits<int>::max();

            // Consider the earliest unprocessed "normal" event.
            if (m_segment->isBeforeEndMarker(j)) {
                bestBaseTime = (*j)->getAbsoluteTime();
            }
            
            // k is a pointer to the note iterator we will actually
            // use.  Initialize it to the default of the segment's
            // own.
            Segment::iterator *k = &j;

            // Now consider triggered events (again the earliest
            // unprocessed one).  Break ties in favor of "real" notes.
            if (implied != m_triggeredEvents->end() &&
                (!m_segment->isBeforeEndMarker(j) ||
                 (*implied)->getAbsoluteTime() < bestBaseTime)) {
                k = &implied;
                usingImplied = true;
                bestBaseTime = (*implied)->getAbsoluteTime();
            }

            // If the earlier event now is a noteoff, use it.  We
            // compare to the performance time since noteoffs already
            // take repeat-times into count.
            if (haveEarlierNoteoff(bestBaseTime + timeForRepeats)) {
                popInsertNoteoff(track->getId(), comp);
                continue;
            }

            // We handle nested ornament expansion elsewhere, so
            // trigger events won't be found in implied.
            if (!usingImplied) { 

                long triggerId = -1;
                (**k)->get<Int>(BaseProperties::TRIGGER_SEGMENT_ID, triggerId);

                if (triggerId >= 0) {

                    TriggerSegmentRec *rec =
                        comp.getTriggerSegmentRec(triggerId);
                    // We will invalidate `implied' so we arrange to
                    // re-find it later.  Since we're always treating
                    // a normal note here, we always use the findTime
                    // method.
                    timeT refTime = (*j)->getAbsoluteTime();
                    ControllerContextParams
                        params(refTime, getInstrument(), m_segment,
                               m_triggeredEvents, m_controllerCache, 0);

                    // Add triggered events into m_triggeredEvents.
                    // This invalidates `implied'.
                    bool insertedSomething =
                        rec->ExpandInto(m_triggeredEvents,
                                        j, m_segment, &params);
                    if (insertedSomething) {
                        // Re-find `implied'
                        implied =
                            Segment::iterator
                            (m_triggeredEvents->findTime(refTime));

                        // Recalculate how much buffer space to
                        // reserve.  !!! Probably should calculate the
                        // extra from m_triggeredEvents rather than
                        // rec->getSegment()
                        int spaceNeeded =
                            addSize(calculateSize(), rec->getSegment());
                        // Reserve more space if we will need it.
                        if (spaceNeeded > capacity()) {
                            reserve(spaceNeeded);
                        }
                    }
                        
                    // whatever happens, we don't want to write this one
                    ++j; 

                    // Since we're no longer sure what the next event
                    // is, restart the loop.
                    continue;
                }
            }

            // Ignore rests
            //
            if (!(**k)->isa(Note::EventRestType)) {

                SegmentPerformanceHelper helper
                (usingImplied ? *m_triggeredEvents : *m_segment);

                timeT playTime =
                    helper.getSoundingAbsoluteTime(*k) + timeForRepeats;
                if (playTime >= repeatEndTime) break;

                timeT playDuration = helper.getSoundingDuration(*k);

                // Ignore notes without duration -- they're probably in a tied
                // series but not as first note
                //
                if (playDuration > 0 || !(**k)->isa(Note::EventType)) {

                    if (playTime + playDuration > repeatEndTime)
                        playDuration = repeatEndTime - playTime;

                    playTime = playTime + m_segment->getDelay();
                    const RealTime eventTime = toRealTime(comp, playTime);

                    // slightly quicker than calling helper.getRealSoundingDuration()
                    RealTime endTime =
                        toRealTime(comp, playTime + playDuration);
                    const RealTime duration = endTime - eventTime;

                    try {
                        // Create mapped event and put it in buffer.
                        // The instrument will be set later by
                        // ChannelManager, so we set it to zero here.
                        MappedEvent e(0,
                                      ***k,  // three stars! what an accolade
                                      eventTime,
                                      duration);

                        // Somewhat hacky: The MappedEvent ctor makes
                        // events that needn't be inserted invalid.
                        if (e.isValid()) {
                            e.setTrackId(track->getId());

                            if ((**k)->isa(Controller::EventType) ||
                                (**k)->isa(PitchBend::EventType)) {
                                m_controllerCache.storeLatestValue((**k));
                            }
                            
                            if ((**k)->isa(Note::EventType)) {
                                if (m_segment->getTranspose() != 0) {
                                    e.setPitch(e.getPitch() +
                                               m_segment->getTranspose());
                                }
                                enqueueNoteoff(playTime + playDuration,
                                               e.getPitch());
                            }
                            mapAnEvent(&e); 
                        } else {}
                        
                    } catch (...) {
#ifdef DEBUG_INTERNAL_SEGMENT_MAPPER
                        SEQMAN_DEBUG << "SegmentMapper::fillBuffer - caught exception while trying to create MappedEvent\n";
#endif
                    }
                }
            }

            ++*k; // increment either i or j, whichever one we just used
        }
    }

    // After all the other events, there may still be Noteoffs.
    while (!m_noteOffs.empty()) {
        popInsertNoteoff(track->getId(), comp);
    }

    bool anything = (size() != 0);

    RealTime minRealTime;
    RealTime maxRealTime;
    if (anything) {
        minRealTime = getBuffer()[0].getEventTime();
        maxRealTime = getBuffer()[size() - 1].getEventTime();

        // Fix for bug #1378.  Start slightly before the first note so
        // that program etc is sent then.  We'll allow it to be before
        // zeroTime, since MappedBufMetaIterator can handle early
        // start-times.
        static const RealTime preparationTime = RealTime::fromSeconds(0.5);
        minRealTime = minRealTime - preparationTime;
    } else {
        minRealTime = maxRealTime = RealTime::zeroTime;
    }

    m_channelManager.setRequiredInterval(minRealTime, maxRealTime,
                                         RealTime::zeroTime, RealTime(1,0));

    if (!ControlBlock::getInstance()->isTrackMuted(track->getId())) {
        // Track is unmuted, so get a channel interval to play on.
        // This also releases the old channel interval (possibly
        // getting it again)
        m_channelManager.reallocate(false);
    } else {
        // But if track is muted, don't waste a channel interval on
        // it.  If that changes later, the first played note will
        // trigger a search for one.
        m_channelManager.freeChannelInterval();
    }


    // We have changed the contents, so force a reinit.  Even if the
    // length is the same, the current controllers for a given time
    // may have changed.
    m_channelManager.setDirty();
    setStartEnd(minRealTime, maxRealTime);
}

    /** Functions about the noteoff queue **/

bool
InternalSegmentMapper::
haveEarlierNoteoff(timeT t)
{
    return
        (!m_noteOffs.empty()) &&
        (m_noteOffs.begin()->first <= t);
}

void
InternalSegmentMapper::
enqueueNoteoff(timeT time, int pitch)
{
    typedef NoteoffContainer::iterator iterator;
    // Remove any existing pending noteoff of that pitch.  Since
    // remove_if doesn't work on std::map, we use this written-out
    // equivalent.
    iterator iter = m_noteOffs.begin();
    while(iter != m_noteOffs.end()) {
        if (iter->second == pitch) {
            iterator tmp = iter;
            ++iter;
            m_noteOffs.erase(tmp);
        } else {
            ++iter;
        }
    }

    // Enqueue this noteoff
    m_noteOffs.insert(Noteoff(time, pitch));
}


void
InternalSegmentMapper::
popInsertNoteoff(int trackid, Composition &comp)
{
    // Look at top element
    timeT internalTime = m_noteOffs.begin()->first;
    int pitch          = m_noteOffs.begin()->second;

    // A noteoff looks like a note with velocity = 0.
    // Our noteoffs already have performance pitch, so
    // don't add segment's transpose.
    MappedEvent event(0, MappedEvent::MidiNote, pitch, 0);
    event.setEventTime(toRealTime(comp, internalTime));
    event.setTrackId(trackid);
    mapAnEvent(&event);

    // pop
    m_noteOffs.erase(m_noteOffs.begin());
}

int
InternalSegmentMapper::addSize(int size, Segment *s)
{
    int repeatCount = getSegmentRepeatCount();
    // Double the size because we may get a noteoff for every noteon
    return size + (repeatCount + 1) * 2 * int(s->size());
}

int
InternalSegmentMapper::calculateSize()
{
    if (!m_segment) { return 0; }
    return addSize(0, m_segment);
}

// Make the channel ready to be played on.  
void
InternalSegmentMapper::
makeReady(MappedInserterBase &inserter, RealTime time)
{
    Callbacks callbacks(this);
    m_channelManager.setInstrument(m_doc->getInstrument(m_segment));
    m_channelManager.makeReady(inserter, time, &callbacks,
                               m_segment->getTrack());
}

void
InternalSegmentMapper::doInsert(MappedInserterBase &inserter, MappedEvent &evt,
                               RealTime start, bool dirtyIter)
{
    if (dirtyIter) {
        m_channelManager.setInstrument(m_doc->getInstrument(m_segment));
    }
    Callbacks callbacks(this);
    m_channelManager.doInsert(inserter, evt, start, &callbacks,
                              dirtyIter, m_segment->getTrack());
}

int
InternalSegmentMapper::
getControllerValue(timeT searchTime, const std::string eventType,
                   int controllerId)
{
    return
        m_controllerCache.
        getControllerValue(getInstrument(), m_segment, m_triggeredEvents,
                           searchTime, eventType, controllerId);
}

bool
InternalSegmentMapper::
shouldPlay(MappedEvent *evt, RealTime sliceStart)
{
    // #1048388:
    // Ensure sysex heeds mute status, but ensure clocks etc still get
    // through
    if (evt->getType() == MappedEvent::MidiSystemMessage &&
        evt->getData1() != MIDI_SYSTEM_EXCLUSIVE)
        { return true; }
    
    // Otherwise if it's muted it doesn't play.
    if (mutedEtc()) { return false; }

    // Otherwise it should play if it's not already all done sounding.
    // The timeslice logic will have already excluded events that
    // start too late.
    return !evt->EndedBefore(sliceStart);
}

/***  InternalSegmentMapper::Callbacks ***/

ControllerAndPBList
InternalSegmentMapper::Callbacks::
getControllers(Instrument *instrument, RealTime start)
{
    Profiler profiler("InternalSegmentMapper::Callbacks::getControllers", false);
    timeT startTime =
        m_mapper->m_doc->getComposition().getElapsedTimeForRealTime(start);

    // If time is at or before all events, we can just use static values.
    if (startTime <= m_mapper->m_segment->getStartTime()) {
        return ControllerAndPBList(instrument->getStaticControllers());
    }
        
    ControllerAndPBList returnValue;
    // Get a value for each controller type.  Always get a value, just
    // in case the controller used to be set on this channel.
    StaticControllers &list = instrument->getStaticControllers();
    for (StaticControllerConstIterator cIt = list.begin();
         cIt != list.end(); ++cIt) {
        MidiByte controlId = cIt->first;
        MidiByte controlValue =
            m_mapper->
            getControllerValue(startTime,
                               Controller::EventType,
                               controlId);
        returnValue.m_controllers.
            push_back(std::pair<MidiByte, MidiByte>(controlId,
                                                    controlValue));
    }

    // Do the same for PitchBend, of which we only treat one (ie, we
    // ignore GM2's other PitchBend types)
    {
        MidiByte controlValue =
            m_mapper->
            getControllerValue(startTime,
                               PitchBend::EventType,
                               0);

        returnValue.m_havePitchbend = true;
        returnValue.m_pitchbend     = controlValue;
    }
    return returnValue;
}

}