// showdsp — Tier 3: onset detection. MIT license.
//
// ODF: spectral flux on a log-spaced triangular filterbank with a maximum
// filter over the previous frame's neighboring bands to suppress
// vibrato/glides — the SuperFlux scheme as PUBLISHED in:
//   [1] S. Böck, G. Widmer, "Maximum Filter Vibrato Suppression for Onset
//       Detection", DAFx-13, 2013.  (parameters re-derived/tuned here)
// Peak picking follows the pre/post max+mean scheme described in:
//   [2] S. Böck, F. Krebs, M. Schedl, "Evaluating the Online Capabilities
//       of Onset Detection Methods", ISMIR 2012.
// The causal variant is our own design (fixed 1-hop confirmation).
#pragma once
#include "dsp.h"

namespace showdsp {

// log-spaced triangular filterbank (bandsPerOctave per octave, dedup at
// low end where neighboring centers collapse onto the same FFT bin)
struct LogBank {
    float sr = 44100.f;
    float fmin = 30.f, fmax = 16000.f;
    int bandsPerOctave = 24;
    size_t nbins = 0;
    // triangle definitions: for each filter, (loBin, ctrBin, hiBin)
    struct Tri { int lo, ctr, hi; };
    std::vector<Tri> tris;

    void prepare(size_t bins) {
        nbins = bins;
        const double hzPerBin = (sr * 0.5) / (double)(bins - 1);
        // log-spaced center frequencies
        std::vector<int> centers;
        const double octaves = std::log2(fmax / fmin);
        const int n = (int)std::ceil(octaves * bandsPerOctave);
        int prev = -1;
        for (int i = 0; i <= n; i++) {
            double f = fmin * std::pow(2.0, (double)i / bandsPerOctave);
            int b = (int)std::lround(f / hzPerBin);
            if (b >= (int)bins) break;
            if (b != prev) { centers.push_back(b); prev = b; }
        }
        tris.clear();
        for (size_t i = 1; i + 1 < centers.size(); i++)
            tris.push_back({centers[i - 1], centers[i], centers[i + 1]});
    }
    size_t bands() const { return tris.size(); }

    // magnitude spectrum -> filterbank magnitudes (triangle-weighted sums)
    void apply(const float* mag, float* out) const {
        for (size_t t = 0; t < tris.size(); t++) {
            const auto& tr = tris[t];
            double s = 0;
            for (int k = tr.lo; k <= tr.hi; k++) {
                double w;
                if (k <= tr.ctr)
                    w = tr.ctr > tr.lo ? (double)(k - tr.lo) / (tr.ctr - tr.lo) : 1.0;
                else
                    w = tr.hi > tr.ctr ? (double)(tr.hi - k) / (tr.hi - tr.ctr) : 1.0;
                s += w * mag[k];
            }
            out[t] = (float)s;
        }
    }
};

// SuperFlux-style ODF: half-rectified difference between the current
// log-compressed filterbank frame and a 3-band maximum filter over the
// frame `lag` hops earlier.
struct SuperFluxOdf {
    LogBank bank;
    int lag = 2;               // hops of temporal distance (mu in [1])
    std::vector<std::vector<float>> hist;  // ring of past log-band frames
    std::vector<float> tmp;

    void prepare(size_t bins) { bank.prepare(bins); }

    float push(const float* mag) {
        const size_t nb = bank.bands();
        tmp.resize(nb);
        bank.apply(mag, tmp.data());
        for (auto& v : tmp) v = std::log10(1.f + 10.f * v); // log compression
        float odf = 0.f;
        if ((int)hist.size() >= lag) {
            const auto& past = hist[hist.size() - lag];
            for (size_t b = 0; b < nb; b++) {
                float mx = past[b];
                if (b > 0)      mx = std::max(mx, past[b - 1]);
                if (b + 1 < nb) mx = std::max(mx, past[b + 1]);
                float d = tmp[b] - mx;
                if (d > 0) odf += d;
            }
        }
        hist.push_back(tmp);
        if (hist.size() > 8) hist.erase(hist.begin());
        return odf;
    }
};

// offline peak picking over a full ODF sequence [2], with a robust
// (median/MAD-based) threshold so noise-floor wiggle does not fire.
inline std::vector<size_t> pickOnsets(const std::vector<float>& odf,
                                      float fps,
                                      float delta = 1.5f,        // in robust-sigma units
                                      float preMaxS = 0.03f, float postMaxS = 0.03f,
                                      float preAvgS = 0.10f, float postAvgS = 0.07f,
                                      float combineS = 0.03f,
                                      float warmupS = 0.1f) {    // skip STFT warmup
    const int preMax = std::max(1, (int)std::lround(preMaxS * fps));
    const int postMax = std::max(1, (int)std::lround(postMaxS * fps));
    const int preAvg = std::max(1, (int)std::lround(preAvgS * fps));
    const int postAvg = std::max(1, (int)std::lround(postAvgS * fps));
    const int combine = std::max(1, (int)std::lround(combineS * fps));
    std::vector<size_t> out;
    if (odf.size() < 8) return out;
    // robust global scale: median + MAD
    std::vector<float> s(odf);
    std::nth_element(s.begin(), s.begin() + s.size() / 2, s.end());
    const float med = s[s.size() / 2];
    for (auto& v : s) v = std::fabs(v - med);
    std::nth_element(s.begin(), s.begin() + s.size() / 2, s.end());
    const float mad = std::max(1e-9f, s[s.size() / 2]);
    const float dAbs = delta * 4.0f * mad;   // ~[2.7 sigma] for gaussian noise
    long last = -combine - 1;
    const long start = (long)std::lround(warmupS * fps);
    for (long n = start; n < (long)odf.size(); n++) {
        bool isMax = true;
        for (long k = std::max(0L, n - preMax); k <= std::min((long)odf.size() - 1, n + postMax); k++)
            if (odf[k] > odf[n]) { isMax = false; break; }
        if (!isMax) continue;
        double sm = 0; long c = 0;
        for (long k = std::max(0L, n - preAvg); k <= std::min((long)odf.size() - 1, n + postAvg); k++) { sm += odf[k]; c++; }
        if (odf[n] < sm / c + dAbs) continue;
        if (n - last < combine) continue;
        last = n;
        out.push_back((size_t)n);
    }
    return out;
}

// causal onset picker (live): fires when the previous hop was a local max
// above a ROBUST threshold (median + k*IQR of recent history — immune to
// its own past spikes, unlike mean/sigma). Detection latency = 1 hop.
struct CausalOnsetPicker {
    float k = 2.0f;
    size_t history = 44;        // ~0.5 s at 86 fps
    float minGapS = 0.08f;
    float fps = 86.13f;

    std::vector<float> h;
    double lastT = -1;
    double push(float v, double tNow) {
        h.push_back(v);
        if (h.size() > history + 2) h.erase(h.begin());
        double fired = -1;
        const size_t n = h.size();
        if (n >= 12) {
            std::vector<float> s(h);
            std::sort(s.begin(), s.end());
            const float p25 = s[n / 4], p50 = s[n / 2], p75 = s[(3 * n) / 4];
            const float iqr = std::max(1e-6f, p75 - p25);
            const float thr = p50 + k * iqr;
            const float prev = h[n - 2], cur = h[n - 1], prev2 = h[n - 3];
            if (prev > thr && prev >= cur && prev > prev2) {
                double t = tNow - 1.0 / fps;
                if (lastT < 0 || t - lastT >= minGapS) { lastT = t; fired = t; }
            }
        }
        return fired;
    }
};

} // namespace showdsp
