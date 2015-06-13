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

#include "AudioFileTimeStretcher.h"
#include "AudioTimeStretcher.h"
#include "AudioFileManager.h"
#include "WAVAudioFile.h"
#include "base/RealTime.h"

#include <QApplication>

#include <iostream>
#include <fstream>

#ifdef __FreeBSD__
#include <stdlib.h>
#else
#include <alloca.h>
#endif

namespace Rosegarden {


AudioFileTimeStretcher::AudioFileTimeStretcher(AudioFileManager *manager) :
    m_manager(manager),
    m_timestretchCancelled(false)
{
}

AudioFileTimeStretcher::~AudioFileTimeStretcher()
{
}

AudioFileId
AudioFileTimeStretcher::getStretchedAudioFile(AudioFileId source,
                                              float ratio)
{
    AudioFile *sourceFile = m_manager->getAudioFile(source);
    if (!sourceFile) {
	throw SoundFile::BadSoundFileException
	    ("<unknown source>",
	     "Source file not found in AudioFileTimeStretcher::getStretchedAudioFile");
    }

    std::cerr << "AudioFileTimeStretcher: got source file id " << source
              << ", name " << sourceFile->getFilename() << std::endl;

    AudioFile *file = m_manager->createDerivedAudioFile(source, "stretch");
    if (!file) {
	throw AudioFileManager::BadAudioPathException(m_manager->getAudioPath());
    }

    std::cerr << "AudioFileTimeStretcher: got derived file id " << file->getId()
              << ", name " << file->getFilename() << std::endl;

    std::ifstream streamIn(sourceFile->getFilename().toLocal8Bit(),
			   std::ios::in | std::ios::binary);
    if (!streamIn) {
	throw SoundFile::BadSoundFileException
	    (file->getFilename(),
	     "Failed to open source stream for time stretcher");
    }
    
    //!!!
    //...
    // Need to make SoundDriver::getAudioRecFileFormat available?
    // -- the sound file classes should just have a float interface
    // (like libsndfile, or hey!, we could use libsndfile...)

    WAVAudioFile writeFile
        (file->getFilename(),
         sourceFile->getChannels(),
         sourceFile->getSampleRate(),
         sourceFile->getSampleRate() * 4 * sourceFile->getChannels(),
         4 * sourceFile->getChannels(),
         32);

    if (!writeFile.write()) {
        throw AudioFileManager::BadAudioPathException
            (file->getFilename());
    }
    
    int obs = 1024;
    int ibs = obs / ratio;
    int ch = sourceFile->getChannels();
    int sr = sourceFile->getSampleRate();
	    
    AudioTimeStretcher stretcher(sr, ch, ratio, true, obs);

    // We'll first prime the timestretcher with half its window size
    // of silence, an amount which we then discard at the start of the
    // output (as well as its own processing latency).  Really the
    // timestretcher should handle this itself and report it in its
    // own latency calculation

    size_t padding = stretcher.getWindowSize()/2;

    char *ebf = (char *)alloca
        (ch * ibs * sourceFile->getBytesPerFrame());
    
    std::vector<float *> dbfs;
    for (int c = 0; c < ch; ++c) {
        dbfs.push_back((float *)alloca((ibs > int(padding) ? size_t(ibs) : padding)
                                       * sizeof(float)));
    }
    
    float **ibfs = (float **)alloca(ch * sizeof(float *));
    float **obfs = (float **)alloca(ch * sizeof(float *));
            
    for (int c = 0; c < ch; ++c) {
        ibfs[c] = dbfs[c];
    }
        
    for (int c = 0; c < ch; ++c) {
        obfs[c] = (float *)alloca(obs * sizeof(float));
    }
        
    char *oebf = (char *)alloca(ch * obs * sizeof(float));
        
    int totalIn = 0, totalOut = 0;
        
    for (int c = 0; c < ch; ++c) {
        for (size_t i = 0; i < padding; ++i) {
            ibfs[c][i] = 0.f;
        }
    }
    stretcher.putInput(ibfs, padding);

    RealTime totalTime = sourceFile->getLength();
    long fileTotalIn = RealTime::realTime2Frame
        (totalTime, sourceFile->getSampleRate());
    int progressCount = 0;
	
    long expectedOut = ceil(fileTotalIn * ratio);

    m_timestretchCancelled = false;
    bool inputExhausted = false;

    sourceFile->scanTo(&streamIn, RealTime::zeroTime);

    while (1) {
            
        if (m_timestretchCancelled) {
            std::cerr << "AudioFileTimeStretcher::getStretchedAudioFile: cancelled" << std::endl;
            throw CancelledException();
        }
            
        unsigned int thisRead = 0;

        if (!inputExhausted) {
            thisRead = sourceFile->getSampleFrames(&streamIn, ebf, ibs);
            if (int(thisRead) < ibs) inputExhausted = true;
        }
            
        if (thisRead == 0) {
            if (totalOut >= expectedOut) break;
            else {
                // run out of input data, continue feeding zeroes until
                // we have enough output data
                for (int c = 0; c < ch; ++c) {
                    for (int i = 0; i < ibs; ++i) {
                        ibfs[c][i] = 0.f;
                    }
                }
                thisRead = ibs;
            }
        }
            
        if (!sourceFile->decode((unsigned char *)ebf,
                                thisRead * sourceFile->getBytesPerFrame(),
                                sr, ch,
                                thisRead, dbfs, false)) {
            std::cerr << "ERROR: Stupid audio file class failed to decode its own output" << std::endl;
            break;
        }
            
        stretcher.putInput(ibfs, thisRead);
        totalIn += thisRead;
            
        unsigned int available = stretcher.getAvailableOutputSamples();
            
        while (available > 0) {
                
            unsigned int count = available;
            if (count > (unsigned int)obs) count = (unsigned int)obs;

            if (padding > 0) {
                if (count <= padding) {
                    stretcher.getOutput(obfs, count);
                    padding -= count;
                    available -= count;
                    continue;
                } else {
                    stretcher.getOutput(obfs, padding);
                    count -= padding;
                    available -= padding;
                    padding = 0;
                }
            }
                
            stretcher.getOutput(obfs, count);
                
            char *encodePointer = oebf;
            for (unsigned int i = 0; i < count; ++i) {
                for (int c = 0; c < ch; ++c) {
                    float sample = obfs[c][i];
                    *(float *)encodePointer = sample;
                    encodePointer += sizeof(float);
                }
            }
                
            if (totalOut < expectedOut &&
                totalOut + int(count) > expectedOut) {
                count = expectedOut - totalOut;
            }

            writeFile.appendSamples(oebf, count);
            totalOut += count;
            available -= count;

            if (totalOut >= expectedOut) break;
        }
            
        if (++progressCount == 100) {
            int progress = int
                ((100.f * float(totalIn)) / float(fileTotalIn));
            emit setValue(progress);
            qApp->processEvents();
            progressCount = 0;
        }
    }		
        
    emit setValue(100);
    qApp->processEvents();
    writeFile.close();
    
    std::cerr << "AudioFileTimeStretcher::getStretchedAudioFile: success, id is "
                 << file->getId() << std::endl;

    return file->getId();
}

void
AudioFileTimeStretcher::slotStopTimestretch()
{
    m_timestretchCancelled = true;
}


}

#include "AudioFileTimeStretcher.moc"
