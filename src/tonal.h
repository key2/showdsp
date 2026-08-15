// showdsp — Tier 5: spectral peaks, HPCP/chroma, key, tuning. MIT license.
//
// Sources (published papers / research data only):
//   [1] J.O. Smith, "Spectral Audio Signal Processing" (online book) —
//       parabolic peak interpolation.
//   [2] T. Fujishima, "Realtime Chord Recognition of Musical Sound" ICMC
//       1999 (pitch class profile); E. Gómez, "Tonal Description of
//       Polyphonic Audio for Music Content Processing", INFORMS J.
//       Computing 18(3), 2006 (harmonic weighting, cos^2 window HPCP).
//   [3] Key profile DATA (published research tables, cited per profile):
//       Krumhansl 1990; Temperley 1999; Shaath 2011;
//       edma: Á. Faraldo, E. Gómez, S. Jordà, P. Herrera, "Key Estimation
//       in Electronic Dance Music", ECIR 2016 (corpus-derived profiles).
// Implementation is our own; profile values are published research data.
#pragma once
#include "dsp.h"
#include <string>
#include <cstring>

namespace showdsp {

// ------------------------------------------------------- spectral peaks (5.1)
struct Peak { float hz, mag; };

inline void spectralPeaks(const float* mag, size_t nbins, float sr,
                          std::vector<Peak>& out,
                          size_t maxPeaks = 60, float minHz = 40, float maxHz = 5000,
                          float magThreshRel = 1e-4f) {
    out.clear();
    const double hzPerBin = (sr * 0.5) / (double)(nbins - 1);
    float mx = 0; for (size_t k = 0; k < nbins; k++) mx = std::max(mx, mag[k]);
    const float thresh = mx * magThreshRel;
    const size_t k0 = std::max<size_t>(1, (size_t)(minHz / hzPerBin));
    const size_t k1 = std::min(nbins - 2, (size_t)(maxHz / hzPerBin));
    for (size_t k = k0; k <= k1; k++) {
        if (mag[k] <= thresh) continue;
        if (mag[k] >= mag[k - 1] && mag[k] > mag[k + 1]) {
            // parabolic interpolation on LOG magnitude (exact-ish for
            // windowed sinusoids — JOS, Spectral Audio Signal Processing)
            const float lm1 = std::log(mag[k - 1] + 1e-12f);
            const float l0 = std::log(mag[k] + 1e-12f);
            const float lp1 = std::log(mag[k + 1] + 1e-12f);
            float off, lh; parabolic(lm1, l0, lp1, off, lh);
            out.push_back({(float)((k + off) * hzPerBin), std::exp(lh)});
        }
    }
    if (out.size() > maxPeaks) {
        std::partial_sort(out.begin(), out.begin() + maxPeaks, out.end(),
                          [](const Peak& a, const Peak& b) { return a.mag > b.mag; });
        out.resize(maxPeaks);
    }
}

// ----------------------------------------------------------------- HPCP (5.2)
// size = bins per octave-of-pitch-classes (36 = 1/3 semitone resolution).
// Each spectral peak contributes mag^2 into a cos^2 window around its
// pitch class, for `harmonics` subharmonic candidates with exponentially
// decaying weight (a peak at f supports f, f/2, f/3, ... as fundamentals).
struct Hpcp {
    int size = 36;
    int harmonics = 4;
    float refHz = 440.f;
    float windowSemitones = 4.f / 3.f;
    float decay = 0.6f;

    void compute(const std::vector<Peak>& peaks, float* out) const {
        std::fill(out, out + size, 0.f);
        const float binsPerSemitone = size / 12.f;
        for (const auto& p : peaks) {
            if (p.hz <= 20.f) continue;
            for (int h = 1; h <= harmonics; h++) {
                const float f0 = p.hz / (float)h;         // candidate fundamental
                if (f0 < 25.f) break;
                const float w = std::pow(decay, (float)(h - 1)) * p.mag * p.mag;
                // pitch class in bins relative to refHz (A)
                float st = 12.f * std::log2(f0 / refHz);   // semitones from A4
                float pc = std::fmod(st, 12.f); if (pc < 0) pc += 12.f;
                const float ctr = pc * binsPerSemitone;
                const float halfWin = windowSemitones * 0.5f * binsPerSemitone;
                const int lo = (int)std::floor(ctr - halfWin), hi = (int)std::ceil(ctr + halfWin);
                for (int b = lo; b <= hi; b++) {
                    float d = (b - ctr);
                    if (std::fabs(d) > halfWin) continue;
                    float cw = std::cos(kPi * 0.5 * d / halfWin);
                    int bin = ((b % size) + size) % size;
                    out[bin] += w * cw * cw;
                }
            }
        }
        float mx = 0; for (int i = 0; i < size; i++) mx = std::max(mx, out[i]);
        if (mx > 0) for (int i = 0; i < size; i++) out[i] /= mx;
    }
};

// ------------------------------------------------------------------ key (5.3)
// Correlate a 12-bin chroma (folded, A-referenced) against 24 rotated
// major/minor profiles; Pearson correlation as in the standard
// Krumhansl–Schmuckler method (textbook).
struct KeyProfile { const char* name; float major[12]; float minor[12]; };

// PUBLISHED PROFILE DATA (indexes: 0 = tonic, chromatic ascending)
inline const KeyProfile* keyProfiles(size_t& count) {
    static const KeyProfile P[] = {
        // Krumhansl & Kessler probe-tone ratings (Krumhansl 1990, book).
        {"krumhansl",
         {6.35f,2.23f,3.48f,2.33f,4.38f,4.09f,2.52f,5.19f,2.39f,3.66f,2.29f,2.88f},
         {6.33f,2.68f,3.52f,5.38f,2.60f,3.53f,2.54f,4.75f,3.98f,2.69f,3.34f,3.17f}},
        // Temperley, "What's key for key?", Music Perception 17(1), 1999.
        {"temperley",
         {5.0f,2.0f,3.5f,2.0f,4.5f,4.0f,2.0f,4.5f,2.0f,3.5f,1.5f,4.0f},
         {5.0f,2.0f,3.5f,4.5f,2.0f,4.0f,2.0f,4.5f,3.5f,2.0f,1.5f,4.0f}},
        // Shaath, "Estimation of key in digital music recordings", 2011.
        {"shaath",
         {6.6f,2.0f,3.5f,2.3f,4.6f,4.0f,2.5f,5.2f,2.4f,3.7f,2.3f,3.4f},
         {6.5f,2.7f,3.5f,5.4f,2.6f,3.5f,2.5f,5.2f,4.0f,2.7f,4.3f,3.2f}},
        // Faraldo et al., ECIR 2016 — EDM corpus-derived profiles (edma).
        {"edma",
         {0.16519551f,0.04749026f,0.08293076f,0.06687112f,0.09994645f,0.09274123f,
          0.05294487f,0.13159476f,0.05218986f,0.07443653f,0.06940723f,0.0642515f},
         {0.17235348f,0.05336489f,0.0761009f,0.10043649f,0.05621498f,0.08527853f,
          0.0497915f,0.13451001f,0.07458916f,0.05003023f,0.09187879f,0.05545106f}},
    };
    count = sizeof(P) / sizeof(P[0]);
    return P;
}

struct KeyResult {
    std::string key = "?";        // "A".."G#" (A-referenced chroma)
    std::string scale = "major";
    float strength = 0;           // best correlation 0..1-ish
    float firstToSecond = 0;
};

inline float pearson12(const float* a, const float* b, int shift) {
    // correlate a (chroma) with b rotated by shift
    float ma = 0, mb = 0;
    for (int i = 0; i < 12; i++) { ma += a[i]; mb += b[i]; }
    ma /= 12; mb /= 12;
    float num = 0, da = 0, dbb = 0;
    for (int i = 0; i < 12; i++) {
        const float x = a[i] - ma;
        const float y = b[(i - shift + 24) % 12] - mb;
        num += x * y; da += x * x; dbb += y * y;
    }
    const float den = std::sqrt(da * dbb);
    return den > 0 ? num / den : 0.f;
}

// chroma12: 12 bins, bin 0 = A. Returns best key/scale over 24 rotations.
inline KeyResult estimateKey(const float* chroma12, const char* profile = "edma") {
    static const char* kNames[12] = {"A","Bb","B","C","C#","D","Eb","E","F","F#","G","G#"};
    size_t np; const KeyProfile* P = keyProfiles(np);
    const KeyProfile* prof = P;
    for (size_t i = 0; i < np; i++) if (!std::strcmp(P[i].name, profile)) prof = &P[i];
    KeyResult r;
    float best = -2, second = -2; int bestShift = 0; bool bestMinor = false;
    for (int s = 0; s < 12; s++) {
        const float cM = pearson12(chroma12, prof->major, s);
        const float cm = pearson12(chroma12, prof->minor, s);
        if (cM > best) { second = best; best = cM; bestShift = s; bestMinor = false; }
        else if (cM > second) second = cM;
        if (cm > best) { second = best; best = cm; bestShift = s; bestMinor = true; }
        else if (cm > second) second = cm;
    }
    r.key = kNames[bestShift];
    r.scale = bestMinor ? "minor" : "major";
    r.strength = best;
    r.firstToSecond = best > 0 ? (best - second) / best : 0;
    return r;
}

// fold an N-bin HPCP (N = 12*k) to 12 bins, bin 0 = A
inline void foldChroma(const float* hpcp, int size, float* out12) {
    const int k = size / 12;
    for (int i = 0; i < 12; i++) out12[i] = 0;
    for (int b = 0; b < size; b++) {
        // nearest pitch class (bins are centered on classes)
        int pc = (int)std::lround((float)b / k) % 12;
        out12[pc] += hpcp[b];
    }
}

// --------------------------------------------------------------- tuning (5.4)
// Deviation of the tuning from 440 Hz in cents: histogram of peak
// deviations from the tempered grid (Gómez 2006 approach class).
struct TuningEstimator {
    static constexpr int kBins = 100;    // 1-cent resolution over ±50
    double hist[kBins] = {};
    void push(const std::vector<Peak>& peaks) {
        for (const auto& p : peaks) {
            if (p.hz < 60 || p.hz > 4000) continue;
            const float st = 12.f * std::log2(p.hz / 440.f);
            float cents = (st - std::lround(st)) * 100.f;  // -50..50
            int b = (int)std::lround(cents + 50.f);
            if (b >= 0 && b < kBins) hist[b] += p.mag;
        }
    }
    float cents() const {
        int mx = 50; double mv = -1;
        for (int i = 0; i < kBins; i++) if (hist[i] > mv) { mv = hist[i]; mx = i; }
        return (float)(mx - 50);
    }
    float hz() const { return 440.f * std::pow(2.f, cents() / 1200.f); }
};

} // namespace showdsp
