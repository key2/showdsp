// showdsp — Tier 7: audio QC (clipping, hum, gaps). MIT license.
// Own designs; true peak comes from libebur128 (see loudness.h).
#pragma once
#include "dsp.h"

namespace showdsp {

struct QcReport {
    float clipPct = 0;            // % of samples in flat-top clipped runs
    int clipRuns = 0;
    float humDb = -120;           // strongest mains-hum prominence (dB over neighborhood)
    float humHz = 0;
    int gapCount = 0;             // silent holes between loud sections
};

// Goertzel single-bin power (textbook)
inline double goertzelPower(const float* x, size_t n, float hz, float sr) {
    const double w = 2.0 * kPi * hz / sr;
    const double c = 2.0 * std::cos(w);
    double s0 = 0, s1 = 0, s2 = 0;
    for (size_t i = 0; i < n; i++) { s0 = x[i] + c * s1 - s2; s2 = s1; s1 = s0; }
    return s1 * s1 + s2 * s2 - c * s1 * s2;
}

inline QcReport qcAnalyze(const float* x, size_t n, float sr,
                          const std::vector<float>& rmsPerFrame, float fps) {
    QcReport r;
    // ---- clipping: runs of >=4 samples pinned near full scale
    const float t = 0.985f;
    size_t run = 0, clipped = 0;
    for (size_t i = 0; i < n; i++) {
        if (std::fabs(x[i]) >= t) { run++; }
        else {
            if (run >= 4) { clipped += run; r.clipRuns++; }
            run = 0;
        }
    }
    if (run >= 4) { clipped += run; r.clipRuns++; }
    r.clipPct = n ? 100.f * clipped / n : 0.f;

    // ---- hum: mains candidates vs neighbors, on up to 30 s from the middle
    const size_t start = n > (size_t)(30 * sr) ? n / 2 - (size_t)(15 * sr) : 0;
    const size_t len = std::min(n - start, (size_t)(30 * sr));
    if (len > sr) {
        for (float hz : {50.f, 60.f, 100.f, 120.f}) {
            const double p = goertzelPower(x + start, len, hz, sr);
            double nb = 0; int c = 0;
            for (float d : {-7.f, -4.f, 4.f, 7.f}) { nb += goertzelPower(x + start, len, hz + d, sr); c++; }
            nb /= c;
            const float prom = (float)(10.0 * std::log10((p + 1e-12) / (nb + 1e-12)));
            if (prom > r.humDb) { r.humDb = prom; r.humHz = hz; }
        }
    }

    // ---- gaps: >0.15 s below -60 dB while surrounded by > -30 dB audio
    const int minGap = (int)(0.15f * fps);
    int below = 0; bool seenLoud = false;
    for (size_t i = 0; i < rmsPerFrame.size(); i++) {
        const float d = db(rmsPerFrame[i]);
        if (d > -30.f) {
            if (below >= minGap && seenLoud) r.gapCount++;
            below = 0; seenLoud = true;
        } else if (d < -60.f) below++;
        else below = 0;
    }
    return r;
}

} // namespace showdsp
