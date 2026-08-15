// showdsp — Tier 1: instant spectral features. MIT license.
// All textbook definitions (see AUDIO-CLEANROOM.md tier 1 citations):
// band energies, centroid, flux, HFC (Masri 1996), rolloff, flatness,
// crest, RMS, silence gate.
#pragma once
#include "dsp.h"

namespace showdsp {

// energy in [lo,hi) Hz bands from a magnitude spectrum
struct BandSplit {
    std::vector<float> edgesHz;       // e.g. {20,150,800,4000,16000}
    float sr = 44100.f;
    size_t nbins = 0;                 // spectrum bins (N/2+1)

    std::vector<size_t> binEdge;      // precomputed bin indices
    void prepare(size_t bins) {
        nbins = bins;
        binEdge.clear();
        const float hzPerBin = (sr * 0.5f) / (float)(bins - 1);
        for (float e : edgesHz)
            binEdge.push_back(std::min(bins - 1, (size_t)std::lround(e / hzPerBin)));
    }
    // out[i] = sum of squared magnitudes in band i (energy)
    void compute(const float* mag, float* out) const {
        for (size_t b = 0; b + 1 < binEdge.size(); b++) {
            float s = 0;
            for (size_t k = binEdge[b]; k < binEdge[b + 1]; k++) s += mag[k] * mag[k];
            out[b] = s;
        }
    }
    size_t bands() const { return binEdge.empty() ? 0 : binEdge.size() - 1; }
};

inline float rms(const float* x, size_t n) {
    if (!n) return 0;
    double s = 0; for (size_t i = 0; i < n; i++) s += (double)x[i] * x[i];
    return (float)std::sqrt(s / n);
}

// spectral centroid in Hz
inline float centroidHz(const float* mag, size_t nbins, float sr) {
    double num = 0, den = 0;
    const double hzPerBin = (sr * 0.5) / (double)(nbins - 1);
    for (size_t k = 0; k < nbins; k++) { num += k * hzPerBin * mag[k]; den += mag[k]; }
    return den > 0 ? (float)(num / den) : 0.f;
}

// L2 spectral flux, half-wave rectified (increases only)
struct Flux {
    std::vector<float> prev;
    float push(const float* mag, size_t nbins) {
        if (prev.size() != nbins) { prev.assign(mag, mag + nbins); return 0.f; }
        double s = 0;
        for (size_t k = 0; k < nbins; k++) {
            float d = mag[k] - prev[k];
            if (d > 0) s += (double)d * d;
            prev[k] = mag[k];
        }
        return (float)std::sqrt(s);
    }
};

// high frequency content (Masri): sum k*|X_k|^2
inline float hfc(const float* mag, size_t nbins) {
    double s = 0; for (size_t k = 0; k < nbins; k++) s += (double)k * mag[k] * mag[k];
    return (float)s;
}

// frequency below which `frac` of the spectral energy lies
inline float rolloffHz(const float* mag, size_t nbins, float sr, float frac = 0.85f) {
    double total = 0; for (size_t k = 0; k < nbins; k++) total += (double)mag[k] * mag[k];
    if (total <= 0) return 0.f;
    double acc = 0;
    const double hzPerBin = (sr * 0.5) / (double)(nbins - 1);
    for (size_t k = 0; k < nbins; k++) {
        acc += (double)mag[k] * mag[k];
        if (acc >= frac * total) return (float)(k * hzPerBin);
    }
    return sr * 0.5f;
}

// spectral flatness: geometric mean / arithmetic mean of power, 0..1
inline float flatness(const float* mag, size_t nbins) {
    double logs = 0, sum = 0; size_t n = 0;
    for (size_t k = 1; k < nbins; k++) {
        double p = (double)mag[k] * mag[k] + 1e-20;
        logs += std::log(p); sum += p; n++;
    }
    if (!n || sum <= 0) return 0.f;
    return (float)(std::exp(logs / n) / (sum / n));
}

// crest factor of the magnitude spectrum
inline float crest(const float* mag, size_t nbins) {
    float mx = 0; double m = 0;
    for (size_t k = 0; k < nbins; k++) { mx = std::max(mx, mag[k]); m += mag[k]; }
    return m > 0 ? (float)(mx / (m / nbins)) : 0.f;
}

// per-frame silence flags at fixed dB thresholds
struct SilenceGate {
    bool s20 = false, s30 = false, s60 = false;
    void push(float frameRms) {
        float d = db(frameRms);
        s20 = d < -20.f; s30 = d < -30.f; s60 = d < -60.f;
    }
};

} // namespace showdsp
