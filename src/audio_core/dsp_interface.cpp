// Copyright 2017-2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv3 or any later version
// Refer to the LICENSE.txt file included.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <vector>
#include "audio_core/dsp_interface.h"
#include "audio_core/sink.h"
#include "audio_core/sink_details.h"
#include "common/assert.h"
#include "common/logging/log.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/dumping/backend.h"

namespace AudioCore {

namespace {
/// Amount of CSND frames to wait until they are outputted on their own
/// because DSP is not producing any output.
constexpr u32 CsndFramesBeforeDspIdle = 2;
/// Max amount of CSND pending samples.
constexpr std::size_t MaxCsndPendingSamples = samples_per_frame * 4;

/// Set by `AudioCore::SetEffectsTilt` from the command line, before any sink exists.
std::atomic<bool> effects_tilt{false};
} // namespace

void SetEffectsTilt(bool enable) {
    effects_tilt = enable;
}

DspInterface::DspInterface(Core::System& system_) : system(system_) {}

DspInterface::~DspInterface() = default;

void DspInterface::SetSink(AudioCore::SinkType sink_type, std::string_view audio_device) {
    // Dispose of the current sink first to avoid contention.
    sink.reset();

    sink = AudioCore::GetSinkDetails(sink_type).create_sink(audio_device);
    sink->SetCallback(
        [this](s16* buffer, std::size_t num_frames) { OutputCallback(buffer, num_frames); });
    tilt.SetSampleRate(sink->GetNativeSampleRate());
    time_stretcher.SetOutputSampleRate(sink->GetNativeSampleRate());
}

Sink& DspInterface::GetSink() {
    ASSERT(sink);
    return *sink.get();
}

void DspInterface::EnableStretching(bool enable) {
    enable_time_stretching = enable;
}

void DspInterface::OutputFrame(StereoFrame16 frame) {
    std::scoped_lock lock{csnd_mutex};
    MixCsnd(frame);
    PushSamples(frame);
}

void DspInterface::OutputSample(std::array<s16, 2> sample) {
    std::scoped_lock lock{csnd_mutex};
    MixCsnd({&sample, 1});
    PushSamples({&sample, 1});
}

void DspInterface::OutputCsndFrame(const StereoFrame16& frame) {
    std::scoped_lock lock{csnd_mutex};

    if (dsp_samples_since_csnd_frame == 0) {
        csnd_frames_without_dsp++;
    } else {
        csnd_frames_without_dsp = 0;
    }
    dsp_samples_since_csnd_frame = 0;

    csnd_pending.insert(csnd_pending.end(), frame.begin(), frame.end());

    if (csnd_frames_without_dsp >= CsndFramesBeforeDspIdle) {
        // The DSP is not running, CSND is the only output.
        const std::vector<std::array<s16, 2>> samples(csnd_pending.begin(), csnd_pending.end());
        csnd_pending.clear();
        PushSamples(samples);
        return;
    }

    // The two clocks may drift a little, don't let CSND fall too far behind the DSP output.
    while (csnd_pending.size() > MaxCsndPendingSamples) {
        csnd_pending.pop_front();
    }
}

void DspInterface::MixCsnd(std::span<std::array<s16, 2>> samples) {
    // TODO(PabloMK7): Implement SIMD mixing here if possible.
    dsp_samples_since_csnd_frame += samples.size();
    for (auto& sample : samples) {
        if (csnd_pending.empty()) {
            break;
        }
        const auto& csnd = csnd_pending.front();
        for (std::size_t i = 0; i < 2; i++) {
            sample[i] = static_cast<s16>(std::clamp(sample[i] + csnd[i], -0x8000, 0x7FFF));
        }
        csnd_pending.pop_front();
    }
}

void DspInterface::PushSamples(std::span<const std::array<s16, 2>> samples) {
    if (!sink) {
        return;
    }

    if (sink->ImmediateSubmission()) {
        sink->PushSamples(samples.data(), samples.size());
    } else {
        fifo.Push(samples.data(), samples.size());
    }

    auto video_dumper = system.GetVideoDumper();
    if (video_dumper && video_dumper->IsDumping()) {
        for (const auto& sample : samples) {
            video_dumper->AddAudioSample(sample);
        }
    }
}

void DspInterface::DrainFifoBacklog(std::size_t num_frames) {
    // The emulation thread produces a video frame of audio at a time, so the FIFO only needs enough
    // headroom to smooth those bursts. Audio that stays in the FIFO for a whole window without
    // being needed remains queued during a stall or at stream start. That extra data delays all
    // later audio by its length, for the entire session.
    constexpr std::size_t window_frames = native_sample_rate;
    constexpr std::size_t kept_frames = native_sample_rate / 100;

    fifo_low_fill = std::min(fifo_low_fill, fifo.Size());
    backlog_window_frames += num_frames;
    if (backlog_window_frames < window_frames) {
        return;
    }
    if (fifo_low_fill > 2 * kept_frames) {
        const std::size_t excess = fifo_low_fill - kept_frames;
        fifo.Pop(excess);
        LOG_DEBUG(Audio, "Dropped {} frames of audio backlog", excess);
    }
    fifo_low_fill = std::numeric_limits<std::size_t>::max();
    backlog_window_frames = 0;
}

void DspInterface::EffectsTilt::SetSampleRate(unsigned rate) {
    // Everything below this belongs to the music's body (and to engine rumble); effects keep
    // content above it, so the other seats stay audible without dragging their music in.
    constexpr float cutoff_hz = 300.0f;
    constexpr float q = 0.70710678f;
    constexpr float pi = 3.14159265358979f;
    const float w0 = 2.0f * pi * cutoff_hz / static_cast<float>(rate);
    const float cos_w0 = std::cos(w0);
    const float alpha = std::sin(w0) / (2.0f * q);
    const float a0 = 1.0f + alpha;
    b0 = (1.0f + cos_w0) / (2.0f * a0);
    b1 = -(1.0f + cos_w0) / a0;
    b2 = b0;
    a1 = -2.0f * cos_w0 / a0;
    a2 = (1.0f - alpha) / a0;
    x = {};
    y = {};
}

void DspInterface::EffectsTilt::Process(s16* buffer, std::size_t frames) {
    constexpr float gain = 0.5f; // -6 dB, so four consoles sum without running out of headroom
    for (std::size_t i = 0; i < frames; i++) {
        for (std::size_t c = 0; c < 2; c++) {
            const float in = buffer[i * 2 + c] * gain;
            const float out =
                b0 * in + b1 * x[c][0] + b2 * x[c][1] - a1 * y[c][0] - a2 * y[c][1];
            x[c][1] = x[c][0];
            x[c][0] = in;
            y[c][1] = y[c][0];
            y[c][0] = out;
            buffer[i * 2 + c] = static_cast<s16>(std::clamp(out, -32768.0f, 32767.0f));
        }
    }
}

void DspInterface::OutputCallback(s16* buffer, std::size_t num_frames) {
    // Determine if we should stretch based on the current emulation speed.
    // TODO: Only activate audio stretching when emulation speed goes below 95% threshold
    //       (see #2487) -OS
    if (performing_time_stretching && !enable_time_stretching) {
        // If we just stopped stretching, flush the stretcher before returning to normal output.
        flushing_time_stretcher = true;
    }
    performing_time_stretching = enable_time_stretching.load();

    std::size_t frames_written = 0;
    if (performing_time_stretching) {
        const std::vector<s16> in{fifo.Pop()};
        const std::size_t num_in{in.size() / 2};
        frames_written = time_stretcher.Process(in.data(), num_in, buffer, num_frames);
    } else {
        if (flushing_time_stretcher) {
            time_stretcher.Flush();
            frames_written = time_stretcher.Process(nullptr, 0, buffer, num_frames);
            flushing_time_stretcher = false;

            // Make sure any frames that did not fit are cleared from the time stretcher,
            // so that they do not bleed into the next time the stretcher is enabled.
            time_stretcher.Clear();
        }
        frames_written += fifo.Pop(buffer, num_frames - frames_written);
        DrainFifoBacklog(num_frames);
    }

    if (frames_written > 0) {
        std::memcpy(&last_frame[0], buffer + 2 * (frames_written - 1), 2 * sizeof(s16));
    }

    // Hold last emitted frame; this prevents popping.
    for (std::size_t i = frames_written; i < num_frames; i++) {
        std::memcpy(buffer + 2 * i, &last_frame[0], 2 * sizeof(s16));
    }

    // Implementation of the hardware volume slider
    // A cubic curve is used to approximate a linear change in human-perceived loudness
    const float linear_volume = std::clamp(Settings::Volume(), 0.0f, 1.0f);
    if (linear_volume != 1.0) {
        const float volume_scale_factor = linear_volume * linear_volume * linear_volume;
        for (std::size_t i = 0; i < num_frames; i++) {
            buffer[i * 2 + 0] = static_cast<s16>(buffer[i * 2 + 0] * volume_scale_factor);
            buffer[i * 2 + 1] = static_cast<s16>(buffer[i * 2 + 1] * volume_scale_factor);
        }
    }

    if (effects_tilt) {
        tilt.Process(buffer, num_frames);
    }
}

} // namespace AudioCore
