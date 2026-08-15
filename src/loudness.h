// showdsp — Tier 2: loudness via libebur128 (MIT, vendored). MIT license.
// EBU R128 / ITU-R BS.1770-4 momentary/short-term/integrated/LRA/true peak.
#pragma once
#include <vector>
#include <algorithm>
#include <cmath>
#include "../third_party/libebur128/ebur128.h"

namespace showdsp {

struct LoudnessResult {
    float integratedLufs = -70.f;
    float rangeLu = 0.f;
    float truePeakDb = -120.f;
    float momentaryMaxLufs = -70.f;
    std::vector<float> momentary10Hz;   // LUFS at 10 Hz
    float trackGainDb = 0.f;            // to hit -18 LUFS (RG2-style ref)
};

// stereo interleaved f32 -> full R128 analysis
inline LoudnessResult loudnessAnalyze(const float* interleaved, size_t frames,
                                      int channels, int sr) {
    LoudnessResult r;
    ebur128_state* st = ebur128_init((unsigned)channels, (unsigned long)sr,
        EBUR128_MODE_M | EBUR128_MODE_S | EBUR128_MODE_I |
        EBUR128_MODE_LRA | EBUR128_MODE_TRUE_PEAK);
    if (!st) return r;
    const size_t step = (size_t)(sr / 10);      // 100 ms
    for (size_t pos = 0; pos < frames; pos += step) {
        const size_t n = std::min(step, frames - pos);
        ebur128_add_frames_float(st, interleaved + pos * channels, n);
        double m;
        if (ebur128_loudness_momentary(st, &m) == EBUR128_SUCCESS && m > -700) {
            r.momentary10Hz.push_back((float)m);
            r.momentaryMaxLufs = std::max(r.momentaryMaxLufs, (float)m);
        } else {
            r.momentary10Hz.push_back(-70.f);
        }
    }
    double v;
    if (ebur128_loudness_global(st, &v) == EBUR128_SUCCESS) r.integratedLufs = (float)v;
    if (ebur128_loudness_range(st, &v) == EBUR128_SUCCESS) r.rangeLu = (float)v;
    double tp = 0, tpc;
    for (int c = 0; c < channels; c++)
        if (ebur128_true_peak(st, (unsigned)c, &tpc) == EBUR128_SUCCESS) tp = std::max(tp, tpc);
    r.truePeakDb = tp > 0 ? 20.f * std::log10((float)tp) : -120.f;
    r.trackGainDb = -18.f - r.integratedLufs;
    ebur128_destroy(&st);
    return r;
}

} // namespace showdsp
