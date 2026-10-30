// Host audio output for hosts that have none yet: frames are dropped and there is no device,
// so the audio driver paces the title's mixer on its own timer (see QueuedAudioSamples).

#include "kernel/host_audio.h"

namespace hc09
{
    void SubmitAudioFrame(const float* /*planar*/, uint32_t /*samplesPerChannel*/) {}

    int QueuedAudioSamples()
    {
        return -1;
    }
}
