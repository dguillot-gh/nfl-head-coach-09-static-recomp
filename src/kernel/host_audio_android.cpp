// Host audio output on Android: 48 kHz stereo through AAudio (Android 8.1 and later) or OpenSL ES
// (Android 8.0, whose AAudio is unreliable; the LG V30 runs it). HC09_AUDIO_API=aaudio|opensl
// overrides the choice.
//
// The title's 5.1 frames are mixed down to stereo into a ring buffer that the output's callback
// drains; the callback plays silence when the ring runs dry. QueuedAudioSamples reports the ring's
// fill less a cushion of one device callback plus 20 ms, so the audio driver (which keeps 30 ms
// queued) leaves enough in the ring to ride out a busy game thread on the slower phone.

#include "kernel/host_audio.h"

#include <aaudio/AAudio.h>
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
#include <android/api-level.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace hc09
{
    namespace
    {
        constexpr int32_t kSampleRate = 48000;
        constexpr uint32_t kRingFrames = 16384;  // power of two, ~340 ms
        constexpr int32_t kCushionFrames = kSampleRate * 20 / 1000;
        constexpr uint32_t kSlBufferFrames = 960;  // OpenSL ES: 20 ms per queued buffer, two queued

        // Stereo float frames from SubmitAudioFrame (one producer) to the output callback (one consumer).
        struct Ring
        {
            float data[kRingFrames * 2];
            std::atomic<uint64_t> written{ 0 }, read{ 0 };

            uint32_t Fill() const { return uint32_t(written.load(std::memory_order_acquire) - read.load(std::memory_order_acquire)); }

            // Producer: false (nothing written) when the frame doesn't fit.
            bool Write(const float* stereo, uint32_t frames)
            {
                const uint64_t w = written.load(std::memory_order_relaxed);
                if (w - read.load(std::memory_order_acquire) + frames > kRingFrames)
                    return false;
                for (uint32_t i = 0; i < frames; i++)
                {
                    const uint32_t slot = uint32_t((w + i) & (kRingFrames - 1));
                    data[slot * 2] = stereo[i * 2];
                    data[slot * 2 + 1] = stereo[i * 2 + 1];
                }
                written.store(w + frames, std::memory_order_release);
                return true;
            }

            // Consumer: returns the frames copied; the rest of `out` is left for the caller.
            uint32_t Read(float* out, uint32_t frames)
            {
                const uint64_t r = read.load(std::memory_order_relaxed);
                const uint32_t count = std::min<uint32_t>(frames, uint32_t(written.load(std::memory_order_acquire) - r));
                for (uint32_t i = 0; i < count; i++)
                {
                    const uint32_t slot = uint32_t((r + i) & (kRingFrames - 1));
                    out[i * 2] = data[slot * 2];
                    out[i * 2 + 1] = data[slot * 2 + 1];
                }
                read.store(r + count, std::memory_order_release);
                return count;
            }
        };

        struct Output
        {
            std::mutex mutex;  // opening/closing, from the producer's side
            bool initialized = false;
            bool available = false;
            Ring ring;
            std::atomic<int32_t> burstFrames{ 0 };
            std::atomic<uint32_t> underruns{ 0 };  // callbacks that found the ring short
            std::atomic<bool> reopen{ false };     // the AAudio stream was disconnected

            AAudioStream* aaudio = nullptr;

            SLObjectItf slEngineObject = nullptr, slMixObject = nullptr, slPlayerObject = nullptr;
            SLAndroidSimpleBufferQueueItf slQueue = nullptr;
            int16_t slBuffers[2][kSlBufferFrames * 2];
            uint32_t slNext = 0;
        };

        Output& GetOutput()
        {
            static Output output;
            return output;
        }

        // Fills `frames` stereo frames from the ring, padding a shortfall with silence.
        void Pull(Output& out, float* dest, uint32_t frames)
        {
            const uint32_t got = out.ring.Read(dest, frames);
            if (got < frames)
            {
                std::memset(dest + got * 2, 0, (frames - got) * 2 * sizeof(float));
                out.underruns.fetch_add(1, std::memory_order_relaxed);
            }
        }

        // --- AAudio ---

        aaudio_data_callback_result_t AAudioData(AAudioStream*, void* user, void* audioData, int32_t numFrames)
        {
            Pull(*static_cast<Output*>(user), static_cast<float*>(audioData), uint32_t(numFrames));
            return AAUDIO_CALLBACK_RESULT_CONTINUE;
        }

        void AAudioError(AAudioStream*, void* user, aaudio_result_t error)
        {
            // Headphones plugged or unplugged and the like. The stream can't be closed from this
            // callback: the next submitted frame reopens it.
            std::fprintf(stderr, "[audio] AAudio stream error: %s\n", AAudio_convertResultToText(error));
            static_cast<Output*>(user)->reopen.store(true);
        }

        bool OpenAAudio(Output& out)
        {
            AAudioStreamBuilder* builder = nullptr;
            if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK)
                return false;
            AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
            AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
            AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
            AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_FLOAT);
            AAudioStreamBuilder_setChannelCount(builder, 2);
            AAudioStreamBuilder_setSampleRate(builder, kSampleRate);
            AAudioStreamBuilder_setDataCallback(builder, AAudioData, &out);
            AAudioStreamBuilder_setErrorCallback(builder, AAudioError, &out);
            const aaudio_result_t opened = AAudioStreamBuilder_openStream(builder, &out.aaudio);
            AAudioStreamBuilder_delete(builder);
            if (opened != AAUDIO_OK)
            {
                std::fprintf(stderr, "[audio] AAudio open failed: %s\n", AAudio_convertResultToText(opened));
                out.aaudio = nullptr;
                return false;
            }
            // The device's own rate if AAudio didn't resample (it does from 8.1 on): never seen on
            // the target phones, but it would play at the wrong pitch.
            if (AAudioStream_getSampleRate(out.aaudio) != kSampleRate)
                std::fprintf(stderr, "[audio] AAudio runs at %d Hz\n", AAudioStream_getSampleRate(out.aaudio));
            const int32_t burst = AAudioStream_getFramesPerBurst(out.aaudio);
            // Two bursts of device buffer: low latency without glitching on a late callback.
            AAudioStream_setBufferSizeInFrames(out.aaudio, burst * 2);
            out.burstFrames.store(burst);
            if (AAudioStream_requestStart(out.aaudio) != AAUDIO_OK)
            {
                AAudioStream_close(out.aaudio);
                out.aaudio = nullptr;
                return false;
            }
            std::fprintf(stderr, "[audio] output: AAudio %d Hz stereo, %d-frame bursts\n", AAudioStream_getSampleRate(out.aaudio), burst);
            return true;
        }

        void CloseAAudio(Output& out)
        {
            if (out.aaudio)
            {
                AAudioStream_requestStop(out.aaudio);
                AAudioStream_close(out.aaudio);
                out.aaudio = nullptr;
            }
        }

        // --- OpenSL ES ---

        // Called when a queued buffer finished playing: refill it and queue it again.
        void SlBufferDone(SLAndroidSimpleBufferQueueItf queue, void* user)
        {
            auto& out = *static_cast<Output*>(user);
            float stereo[kSlBufferFrames * 2];
            Pull(out, stereo, kSlBufferFrames);
            int16_t* buffer = out.slBuffers[out.slNext];
            out.slNext ^= 1;
            for (uint32_t i = 0; i < kSlBufferFrames * 2; i++)
                buffer[i] = int16_t(std::lrintf(std::clamp(stereo[i], -1.0f, 1.0f) * 32767.0f));
            (*queue)->Enqueue(queue, buffer, sizeof(out.slBuffers[0]));
        }

        void CloseOpenSl(Output& out)
        {
            for (SLObjectItf* object : { &out.slPlayerObject, &out.slMixObject, &out.slEngineObject })
            {
                if (*object)
                    (**object)->Destroy(*object);
                *object = nullptr;
            }
            out.slQueue = nullptr;
        }

        bool OpenOpenSl(Output& out)
        {
            SLEngineItf engine = nullptr;
            SLPlayItf play = nullptr;
            bool ok = slCreateEngine(&out.slEngineObject, 0, nullptr, 0, nullptr, nullptr) == SL_RESULT_SUCCESS &&
                (*out.slEngineObject)->Realize(out.slEngineObject, SL_BOOLEAN_FALSE) == SL_RESULT_SUCCESS &&
                (*out.slEngineObject)->GetInterface(out.slEngineObject, SL_IID_ENGINE, &engine) == SL_RESULT_SUCCESS &&
                (*engine)->CreateOutputMix(engine, &out.slMixObject, 0, nullptr, nullptr) == SL_RESULT_SUCCESS &&
                (*out.slMixObject)->Realize(out.slMixObject, SL_BOOLEAN_FALSE) == SL_RESULT_SUCCESS;
            if (ok)
            {
                SLDataLocator_AndroidSimpleBufferQueue queueLocator{ SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, 2 };
                SLDataFormat_PCM format{ SL_DATAFORMAT_PCM, 2, SL_SAMPLINGRATE_48, SL_PCMSAMPLEFORMAT_FIXED_16,
                    SL_PCMSAMPLEFORMAT_FIXED_16, SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT, SL_BYTEORDER_LITTLEENDIAN };
                SLDataSource source{ &queueLocator, &format };
                SLDataLocator_OutputMix mixLocator{ SL_DATALOCATOR_OUTPUTMIX, out.slMixObject };
                SLDataSink sink{ &mixLocator, nullptr };
                const SLInterfaceID ids[] = { SL_IID_ANDROIDSIMPLEBUFFERQUEUE };
                const SLboolean required[] = { SL_BOOLEAN_TRUE };
                ok = (*engine)->CreateAudioPlayer(engine, &out.slPlayerObject, &source, &sink, 1, ids, required) == SL_RESULT_SUCCESS &&
                    (*out.slPlayerObject)->Realize(out.slPlayerObject, SL_BOOLEAN_FALSE) == SL_RESULT_SUCCESS &&
                    (*out.slPlayerObject)->GetInterface(out.slPlayerObject, SL_IID_PLAY, &play) == SL_RESULT_SUCCESS &&
                    (*out.slPlayerObject)->GetInterface(out.slPlayerObject, SL_IID_ANDROIDSIMPLEBUFFERQUEUE, &out.slQueue) == SL_RESULT_SUCCESS &&
                    (*out.slQueue)->RegisterCallback(out.slQueue, SlBufferDone, &out) == SL_RESULT_SUCCESS;
            }
            if (ok)
            {
                // Prime both buffers (silence until the ring fills), then play.
                out.burstFrames.store(int32_t(kSlBufferFrames));
                std::memset(out.slBuffers, 0, sizeof(out.slBuffers));
                for (const auto& buffer : out.slBuffers)
                    ok = ok && (*out.slQueue)->Enqueue(out.slQueue, buffer, sizeof(buffer)) == SL_RESULT_SUCCESS;
                ok = ok && (*play)->SetPlayState(play, SL_PLAYSTATE_PLAYING) == SL_RESULT_SUCCESS;
            }
            if (!ok)
            {
                std::fprintf(stderr, "[audio] OpenSL ES initialization failed\n");
                CloseOpenSl(out);
                return false;
            }
            std::fprintf(stderr, "[audio] output: OpenSL ES 48 kHz stereo, 2 x %u-frame buffers\n", kSlBufferFrames);
            return true;
        }

        bool Initialize(Output& out)
        {
            if (std::getenv("HC09_NO_AUDIO"))
                return false;
            const char* api = std::getenv("HC09_AUDIO_API");
            const bool useAAudio = api ? std::strcmp(api, "aaudio") == 0 : android_get_device_api_level() >= 27;
            return useAAudio ? OpenAAudio(out) || OpenOpenSl(out) : OpenOpenSl(out);
        }
    }

    int QueuedAudioSamples()
    {
        Output& out = GetOutput();
        if (!out.available)
            return -1;
        return std::max(0, int(out.ring.Fill()) - out.burstFrames.load(std::memory_order_relaxed) - kCushionFrames);
    }

    void SubmitAudioFrame(const float* planar, uint32_t samples)
    {
        Output& out = GetOutput();
        {
            std::lock_guard lock(out.mutex);
            if (!out.initialized)
            {
                out.initialized = true;
                out.available = Initialize(out);
            }
            if (out.reopen.exchange(false) && out.aaudio)
            {
                CloseAAudio(out);
                out.available = OpenAAudio(out) || OpenOpenSl(out);
            }
        }
        if (!out.available)
            return;

        // ITU downmix of 5.1 to stereo (LFE omitted), as on Windows.
        constexpr uint32_t kMaxSamples = 512;
        samples = std::min(samples, kMaxSamples);
        constexpr float kSide = 0.7071f;
        const float* fl = planar;
        const float* fr = planar + samples;
        const float* c = planar + samples * 2;
        const float* sl = planar + samples * 4;
        const float* sr = planar + samples * 5;
        float stereo[kMaxSamples * 2];
        for (uint32_t i = 0; i < samples; i++)
        {
            stereo[i * 2] = std::clamp(fl[i] + kSide * (c[i] + sl[i]), -1.0f, 1.0f);
            stereo[i * 2 + 1] = std::clamp(fr[i] + kSide * (c[i] + sr[i]), -1.0f, 1.0f);
        }
        const bool queued = out.ring.Write(stereo, samples);

        // HC09_AUDIO_STATS=1: every 5 s, the peak submitted, frames dropped (ring full), callbacks
        // that ran short (heard as a gap) and the ring's fill.
        static const bool stats = std::getenv("HC09_AUDIO_STATS") != nullptr;
        if (stats)
        {
            static float peak = 0.0f;
            static uint32_t counted = 0, frames = 0, dropped = 0, minFill = ~0u, maxFill = 0;
            for (uint32_t i = 0; i < samples * 6; i++)
                peak = std::max(peak, std::abs(planar[i]));
            const uint32_t fill = out.ring.Fill();
            minFill = std::min(minFill, fill);
            maxFill = std::max(maxFill, fill);
            dropped += queued ? 0 : 1;
            counted += samples;
            frames++;
            if (counted >= uint32_t(kSampleRate) * 5)
            {
                std::fprintf(stderr, "[audio] submitted peak %.3f, %u frames, %u dropped, %u underruns, ring %.1f-%.1f ms\n", peak,
                    frames, dropped, out.underruns.exchange(0), minFill * 1000.0 / kSampleRate, maxFill * 1000.0 / kSampleRate);
                peak = 0.0f;
                counted = frames = dropped = maxFill = 0;
                minFill = ~0u;
            }
        }
    }
}
