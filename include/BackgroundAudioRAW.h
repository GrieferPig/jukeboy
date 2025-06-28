/*
    BackgroundAudio
    Plays an audio file using IRQ driven decompression.  Main loop() writes
    data to the buffer but isn't blocked while playing

    Copyright (c) 2024 Earle F. Philhower, III <earlephilhower@yahoo.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once
#include <Arduino.h>
#include "WrappedAudioOutputBase.h"
#include "BackgroundAudioBuffers.h"
#include "BackgroundAudioGain.h"

#define SECTOR_SIZE 512 // Size of a disk sector in bytes, used for alignment
#define _fixedSampleRate 44100
#define _fixedChannels 2  // Stereo
#define _fixedBitDepth 16 // 16-bit

class BackgroundAudioRAWClass
{
public:
    /**
        @brief Construct a RAW audio player with a given AudioOutputBase

        @param [in] d AudioOutputBase device (MixerInput or I2S or PWM, etc.) to decode tp
    */
    BackgroundAudioRAWClass(AudioOutputBase &d)
    {
        _out = &d;
    }

    ~BackgroundAudioRAWClass() {}

    /**
        @brief Set the gain multiplier (volume) for the stream.  Takes effect immediately.

        @param [in] scale Floating point value from 0.0....16.0 to multiply all audio data by
    */
    void setGain(float scale)
    {
        _gain = (int32_t)(scale * (1 << 16));
    }

    bool begin()
    {
        // We will use natural frame size to minimize mismatch
        _out->setBuffers(17, framelen);       // 512 words of 16-bit stereo samples (2048 bytes), 16 pending buffers plus one for the current frame
        _out->onTransmit(&_cb, (void *)this); // The pump we will use to generate our audio
        _out->setBitsPerSample(_fixedBitDepth);
        _out->setStereo(true);
        _out->setFrequency(_fixedSampleRate);
        _out->begin();
        return true;
    }

    /**
        @brief Stops the WAV decoder process and the calls the output device's end to shut it down, too.
    */
    void end()
    {
        _out->flush(); // Ensure all data is sent to the audio output
        _out->end();
    }

    /**
        @brief Writes a block of raw data to the decoder's buffer

        @details
        Copies up to `len` bytes into the raw buffer for the object.  Can be called before `begin`,
        and can write less fewer than the number of bytes requested if there is not enough space.
        Will not block.

        For ROM buffers this does not actually copy data, only the pointer.  Therefore, for ROM
        buffers you cannot concatenate data by calling multiple writes because the second write
        will throw out all the data from the first one.  Use `flush` and `write` when ready for
        a completely new buffer.

        @param [in] data Uncompressed input data
        @param [in] len Number of bytes to write

        @return Number of bytes written
    */
    size_t write(const void *data, size_t len)
    {
        ApplyGain((int16_t *)data, framelen * 2, _gain); // framelen * 2 int16_t samples
        return _out->write((const uint8_t *)data, len);
    }

    /**
        @brief Return the number of frames (2048b, 512 stereo samples) can be written to the output device

        @return Bytes that can be written
    */
    size_t availableForWrite()
    {
        return _out->availableForWrite() / 2048;
    }

private:
    static void _cb(void *ptr)
    {
        // ((BackgroundAudioRAWClass *)ptr)->pump(); // No pump needed, we just write data directly to the output
    }

private:
    AudioOutputBase *_out;
    static const size_t framelen = SECTOR_SIZE; // Number of stereo 16bit sample frames per frame (512 samples, 2048 bytes)
    int32_t _gain = 1 << 16;                    // Default gain is 1.0 (no change)
};