/**
 * Copyright 2017 The Android Open Source Project
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

#include <trace.h>
#include <inttypes.h>
#include <oboe/OboeUtilities.h>
#include <common/AudioClock.h>

#include "PlayAudioEngine.h"
#include "logging_macros.h"

constexpr int32_t kAudioSampleChannels = 2; // Stereo
constexpr int64_t kNanosPerMillisecond = 1000000; // Use int64_t to avoid overflows in calculations

PlayAudioEngine::PlayAudioEngine() {

    // Initialize the trace functions, this enables you to output trace statements without
    // blocking. See https://developer.android.com/studio/profile/systrace-commandline.html
    Trace::initialize();

    mSampleChannels = kAudioSampleChannels;
    createPlaybackStream();
}

PlayAudioEngine::~PlayAudioEngine() {

    closeOutputStream();
}

/**
 * Set the audio device which should be used for playback. Can be set to OBOE_UNSPECIFIED if
 * you want to use the default playback device (which is usually the built-in speaker if
 * no other audio devices, such as headphones, are attached).
 *
 * @param deviceId the audio device id, can be obtained through an {@link AudioDeviceInfo} object
 * using Java/JNI.
 */
void PlayAudioEngine::setDeviceId(int32_t deviceId) {

    mPlaybackDeviceId = deviceId;

    // If this is a different device from the one currently in use then restart the stream
    int32_t currentDeviceId = mPlayStream->getDeviceId();
    if (deviceId != currentDeviceId) restartStream();
}

/**
 * Creates an audio stream for playback. The audio device used will depend on mPlaybackDeviceId.
 */
void PlayAudioEngine::createPlaybackStream() {

    OboeStreamBuilder builder;
    setupPlaybackStreamParameters(&builder);

    oboe_result_t result = builder.openStream(&mPlayStream);

    if (result == OBOE_OK && mPlayStream != nullptr) {

        mSampleRate = mPlayStream->getSampleRate();
        mFramesPerBurst = mPlayStream->getFramesPerBurst();

        // Set the buffer size to the burst size - this will give us the minimum possible latency
        mPlayStream->setBufferSizeInFrames(mFramesPerBurst);

        // TODO: Implement Oboe_convertStreamToText
        // PrintAudioStreamInfo(mPlayStream);
        prepareOscillators();

        // Create a latency tuner which will automatically tune our buffer size.
        mLatencyTuner = new OboeLatencyTuner(*mPlayStream);

        // Start the stream - the dataCallback function will start being called
        result = mPlayStream->requestStart();
        if (result != OBOE_OK) {
            LOGE("Error starting stream. %s", Oboe_convertResultToText(result));
        }

        mIsLatencyDetectionSupported = (mPlayStream->getTimestamp(CLOCK_MONOTONIC, 0, 0) !=
                                        OBOE_ERROR_UNIMPLEMENTED);

    } else {
        LOGE("Failed to create stream. Error: %s", Oboe_convertResultToText(result));
    }
}

void PlayAudioEngine::prepareOscillators() {
    mSineOscLeft.setup(440.0, mSampleRate, 0.25);
    mSineOscRight.setup(660.0, mSampleRate, 0.25);
}

/**
 * Sets the stream parameters which are specific to playback, including device id and the
 * callback class, which must be set for low latency playback.
 * @param builder The playback stream builder
 */
void PlayAudioEngine::setupPlaybackStreamParameters(OboeStreamBuilder *builder) {
    builder->setDeviceId(mPlaybackDeviceId);
    builder->setChannelCount(mSampleChannels);

    // We request EXCLUSIVE mode since this will give us the lowest possible latency.
    // If EXCLUSIVE mode isn't available the builder will fall back to SHARED mode.
    builder->setSharingMode(OBOE_SHARING_MODE_EXCLUSIVE);
    builder->setPerformanceMode(OBOE_PERFORMANCE_MODE_LOW_LATENCY);
    builder->setCallback(this);
}

void PlayAudioEngine::closeOutputStream() {

    if (mPlayStream != nullptr) {
        oboe_result_t result = mPlayStream->requestStop();
        if (result != OBOE_OK) {
            LOGE("Error stopping output stream. %s", Oboe_convertResultToText(result));
        }

        result = mPlayStream->close();
        if (result != OBOE_OK) {
            LOGE("Error closing output stream. %s", Oboe_convertResultToText(result));
        }
    }
}

void PlayAudioEngine::setToneOn(bool isToneOn) {
    mIsToneOn = isToneOn;
}

/**
 * Every time the playback stream requires data this method will be called.
 *
 * @param audioStream the audio stream which is requesting data, this is the mPlayStream object
 * @param audioData an empty buffer into which we can write our audio data
 * @param numFrames the number of audio frames which are required
 * @return Either OBOE_CALLBACK_RESULT_CONTINUE if the stream should continue requesting data
 * or OBOE_CALLBACK_RESULT_STOP if the stream should stop.
 */
oboe_data_callback_result_t
PlayAudioEngine::onAudioReady(OboeStream *audioStream, void *audioData, int32_t numFrames) {

    int32_t bufferSize = audioStream->getBufferSizeInFrames();

    if (mBufferSizeSelection == kBufferSizeAutomatic){
        mLatencyTuner->tune();
    } else if (bufferSize != (mBufferSizeSelection * mFramesPerBurst)) {
        audioStream->setBufferSizeInFrames(mBufferSizeSelection * mFramesPerBurst);
        bufferSize = audioStream->getBufferSizeInFrames();
    }

    /**
     * The following output can be seen by running a systrace. Tracing is preferable to logging
     * inside the callback since tracing does not block.
     *
     * See https://developer.android.com/studio/profile/systrace-commandline.html
     */
    int32_t underrunCount = audioStream->getXRunCount();

    Trace::beginSection("numFrames %d, Underruns %d, buffer size %d",
                        numFrames, underrunCount, bufferSize);
    int32_t samplesPerFrame = mSampleChannels;

    // If the tone is on we need to use our synthesizer to render the audio data for the sine waves
    if (audioStream->getFormat() == OBOE_AUDIO_FORMAT_PCM_FLOAT){
        if (mIsToneOn) {
            mSineOscRight.render(static_cast<float *>(audioData),
                                 samplesPerFrame, numFrames);
            if (mSampleChannels == 2) {
                mSineOscLeft.render(static_cast<float *>(audioData) + 1,
                                    samplesPerFrame, numFrames);
            }
        } else {
            memset(static_cast<uint8_t *>(audioData), 0,
                   sizeof(float) * samplesPerFrame * numFrames);
        }
    } else {
        if (mIsToneOn) {
            mSineOscRight.render(static_cast<int16_t *>(audioData),
                                 samplesPerFrame, numFrames);
            if (mSampleChannels == 2) {
                mSineOscLeft.render(static_cast<int16_t *>(audioData) + 1,
                                    samplesPerFrame, numFrames);
            }
        } else {
            memset(static_cast<uint8_t *>(audioData), 0,
                   sizeof(int16_t) * samplesPerFrame * numFrames);
        }
    }

    if (mIsLatencyDetectionSupported) {
        calculateCurrentOutputLatencyMillis(audioStream, &mCurrentOutputLatencyMillis);
    }

    Trace::endSection();
    return OBOE_CALLBACK_RESULT_CONTINUE;
}

/**
 * Calculate the current latency between writing a frame to the output stream and
 * the same frame being presented to the audio hardware.
 *
 * Here's how the calculation works:
 *
 * 1) Get the time a particular frame was presented to the audio hardware
 * @see OboeStream::getTimestamp
 * 2) From this extrapolate the time which the *next* audio frame written to the stream
 * will be presented
 * 3) Assume that the next audio frame is written at the current time
 * 4) currentLatency = nextFramePresentationTime - nextFrameWriteTime
 *
 * @param stream The stream being written to
 * @param latencyMillis pointer to a variable to receive the latency in milliseconds between
 * writing a frame to the stream and that frame being presented to the audio hardware.
 * @return OBOE_OK or a negative error. It is normal to receive an error soon after a stream
 * has started because the timestamps are not yet available.
 */
oboe_result_t
PlayAudioEngine::calculateCurrentOutputLatencyMillis(OboeStream *stream, double *latencyMillis) {

    // Get the time that a known audio frame was presented for playing
    int64_t existingFrameIndex;
    int64_t existingFramePresentationTime;
    oboe_result_t result = stream->getTimestamp(CLOCK_MONOTONIC,
                                                &existingFrameIndex,
                                                &existingFramePresentationTime);

    if (result == OBOE_OK) {

        // Get the write index for the next audio frame
        int64_t writeIndex = stream->getFramesWritten();

        // Calculate the number of frames between our known frame and the write index
        int64_t frameIndexDelta = writeIndex - existingFrameIndex;

        // Calculate the time which the next frame will be presented
        int64_t frameTimeDelta = (frameIndexDelta * OBOE_NANOS_PER_SECOND) / mSampleRate;
        int64_t nextFramePresentationTime = existingFramePresentationTime + frameTimeDelta;

        // Assume that the next frame will be written at the current time
        int64_t nextFrameWriteTime = AudioClock::getNanoseconds(CLOCK_MONOTONIC);

        // Calculate the latency
        *latencyMillis = (double) (nextFramePresentationTime - nextFrameWriteTime)
                         / kNanosPerMillisecond;
    } else {
        LOGE("Error calculating latency: %s", Oboe_convertResultToText(result));
    }

    return result;
}

/**
 * If there is an error with a stream this function will be called. A common example of an error
 * is when an audio device (such as headphones) is disconnected. In this case you should not
 * restart the stream within the callback, instead use a separate thread to perform the stream
 * recreation and restart.
 *
 * @param audioStream the stream with the error
 * @param error the error which occured, a human readable string can be obtained using
 * Oboe_convertResultToText(error);
 *
 * @see OboeStreamCallback
 */
void PlayAudioEngine::onError(OboeStream *audioStream, oboe_result_t error) {
    if (error == OBOE_ERROR_DISCONNECTED) {
        // Handle stream restart on a separate thread
        std::function<void(void)> restartStream = std::bind(&PlayAudioEngine::restartStream, this);
        mStreamRestartThread = new std::thread(restartStream);
    }
}

void PlayAudioEngine::restartStream() {

    LOGI("Restarting stream");

    if (mRestartingLock.try_lock()) {
        closeOutputStream();
        createPlaybackStream();
        mRestartingLock.unlock();
    } else {
        LOGW("Restart stream operation already in progress - ignoring this request");
        // We were unable to obtain the restarting lock which means the restart operation is currently
        // active. This is probably because we received successive "stream disconnected" events.
        // Internal issue b/63087953
    }
}

double PlayAudioEngine::getCurrentOutputLatencyMillis() {
    return mCurrentOutputLatencyMillis;
}

void PlayAudioEngine::setBufferSizeInBursts(int32_t numBursts) {
    mBufferSizeSelection = numBursts;
}
