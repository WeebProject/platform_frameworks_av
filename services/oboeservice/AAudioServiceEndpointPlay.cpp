/*
 * Copyright (C) 2017 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "AAudioService"
//#define LOG_NDEBUG 0
#include <utils/Log.h>

#include <assert.h>
#include <map>
#include <mutex>
#include <utils/Singleton.h>

#include "AAudioEndpointManager.h"
#include "AAudioServiceEndpoint.h"
#include <algorithm>
#include <mutex>
#include <vector>

#include "core/AudioStreamBuilder.h"
#include "AAudioServiceEndpoint.h"
#include "AAudioServiceStreamShared.h"
#include "AAudioServiceEndpointPlay.h"

using namespace android;  // TODO just import names needed
using namespace aaudio;   // TODO just import names needed

#define BURSTS_PER_BUFFER_DEFAULT   2

AAudioServiceEndpointPlay::AAudioServiceEndpointPlay(AAudioService &audioService)
        : mStreamInternalPlay(audioService, true) {
}

AAudioServiceEndpointPlay::~AAudioServiceEndpointPlay() {
}

aaudio_result_t AAudioServiceEndpointPlay::open(const AAudioStreamConfiguration& configuration) {
    aaudio_result_t result = AAudioServiceEndpoint::open(configuration);
    if (result == AAUDIO_OK) {
        mMixer.allocate(getStreamInternal()->getSamplesPerFrame(),
                        getStreamInternal()->getFramesPerBurst());

        int32_t burstsPerBuffer = AAudioProperty_getMixerBursts();
        if (burstsPerBuffer == 0) {
            mLatencyTuningEnabled = true;
            burstsPerBuffer = BURSTS_PER_BUFFER_DEFAULT;
        }
        int32_t desiredBufferSize = burstsPerBuffer * getStreamInternal()->getFramesPerBurst();
        getStreamInternal()->setBufferSize(desiredBufferSize);
    }
    return result;
}

// Mix data from each application stream and write result to the shared MMAP stream.
void *AAudioServiceEndpointPlay::callbackLoop() {
    aaudio_result_t result = AAUDIO_OK;
    int64_t timeoutNanos = getStreamInternal()->calculateReasonableTimeout();

    // result might be a frame count
    while (mCallbackEnabled.load() && getStreamInternal()->isActive() && (result >= 0)) {
        // Mix data from each active stream.
        mMixer.clear();
        { // brackets are for lock_guard
            int index = 0;
            int64_t mmapFramesWritten = getStreamInternal()->getFramesWritten();

            std::lock_guard <std::mutex> lock(mLockStreams);
            for (const sp<AAudioServiceStreamShared>& clientStream : mRegisteredStreams) {
                if (clientStream->isRunning()) {
                    FifoBuffer *fifo = clientStream->getDataFifoBuffer();
                    // Determine offset between framePosition in client's stream vs the underlying
                    // MMAP stream.
                    int64_t clientFramesRead = fifo->getReadCounter();
                    // These two indices refer to the same frame.
                    int64_t positionOffset = mmapFramesWritten - clientFramesRead;
                    clientStream->setTimestampPositionOffset(positionOffset);

                    float volume = 1.0; // to match legacy volume
                    bool underflowed = mMixer.mix(index, fifo, volume);

                    // This timestamp represents the completion of data being read out of the
                    // client buffer. It is sent to the client and used in the timing model
                    // to decide when the client has room to write more data.
                    Timestamp timestamp(fifo->getReadCounter(), AudioClock::getNanoseconds());
                    clientStream->markTransferTime(timestamp);

                    if (underflowed) {
                        clientStream->incrementXRunCount();
                    }
                }
                index++;
            }
        }

        // Write mixer output to stream using a blocking write.
        result = getStreamInternal()->write(mMixer.getOutputBuffer(),
                                            getFramesPerBurst(), timeoutNanos);
        if (result == AAUDIO_ERROR_DISCONNECTED) {
            disconnectRegisteredStreams();
            break;
        } else if (result != getFramesPerBurst()) {
            ALOGW("AAudioServiceEndpoint(): callbackLoop() wrote %d / %d",
                  result, getFramesPerBurst());
            break;
        }
    }

    return NULL; // TODO review
}
