// showdsp — Tier 6: novelty, sections, danceability (DFA), fades. MIT.
//
// Sources:
//   [1] P. Grosche, M. Müller, ISMIR 2009 (novelty from band differences);
//       J. Foote, ICME 2000 (novelty peaks as boundaries).
//   [2] C.-K. Peng et al. 1994 (Detrended Fluctuation Analysis);
//       S. Streich, P. Herrera, AES 118th 2005 (DFA on music -> danceability).
//       Mapping to a 0..3-ish scale is our own, tuned on the oracle corpus.
//   [3] Fades: own design (envelope threshold crossings).
#pragma once
#include "dsp.h"

namespace showdsp {

// ------------------------------------------------------------ novelty (6.1)
// Band-energy matrix (frames x bands) -> half-rectified log-difference sum,
// smoothed, minus local average (so it hovers near 0 between events).
inline std::vector<float> noveltyCurve(const std::vector<std::vector<float>>& bands,
                                       float fps) {
    const size_t n = bands.size();
    std::vector<float> nov(n, 0.f);
    if (n < 3) return nov;
    const size_t nb = bands[0].size();
    for (size_t t = 1; t < n; t++) {
        float s = 0;
        for (size_t b = 0; b < nb; b++) {
            const float cur = std::log10(1.f + 100.f * bands[t][b]);
            const float prv = std::log10(1.f + 100.f * bands[t - 1][b]);
            const float d = cur - prv;
            if (d > 0) s += d;
        }
        nov[t] = s;
    }
    // smooth (~0.5 s) then subtract a running average (~8 s), rectify
    const int sm = std::max(1, (int)(0.25f * fps));
    std::vector<float> out(n, 0.f), tmp(n, 0.f);
    for (size_t t = 0; t < n; t++) {
        int lo = std::max<int>(0, (int)t - sm), hi = std::min<int>(n - 1, t + sm);
        float s = 0; for (int k = lo; k <= hi; k++) s += nov[k];
        tmp[t] = s / (hi - lo + 1);
    }
    const int avg = std::max(1, (int)(4.f * fps));
    for (size_t t = 0; t < n; t++) {
        int lo = std::max<int>(0, (int)t - avg), hi = std::min<int>(n - 1, t + avg);
        float s = 0; for (int k = lo; k <= hi; k++) s += tmp[k];
        out[t] = std::max(0.f, tmp[t] - s / (hi - lo + 1));
    }
    float mx = 0; for (float v : out) mx = std::max(mx, v);
    if (mx > 0) for (auto& v : out) v /= mx;
    return out;
}

// --------------------------------------------------- section candidates (6.2)
inline std::vector<float> sectionCandidates(const std::vector<float>& nov,
                                            float fps, float minGapS = 8.f,
                                            float minHeight = 0.30f, int maxN = 8) {
    std::vector<size_t> idx(nov.size());
    for (size_t i = 0; i < idx.size(); i++) idx[i] = i;
    std::sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return nov[a] > nov[b]; });
    std::vector<float> out;
    for (size_t i : idx) {
        if (nov[i] < minHeight || (int)out.size() >= maxN) break;
        const float t = i / fps;
        bool ok = true;
        for (float c : out) if (std::fabs(c - t) < minGapS) { ok = false; break; }
        if (ok) out.push_back(t);
    }
    std::sort(out.begin(), out.end());
    return out;
}

// -------------------------------------------------------- danceability (6.3)
// DFA (Peng 1994) over a ~100 Hz envelope; alpha near 0.5 = uncorrelated
// (strongly rhythmic/percussive at these scales), higher = smoother 1/f.
// danceability = kMap / mean(alpha over scales 0.3..6 s), clamped 0..3.
inline float danceability(const std::vector<float>& envelope100Hz) {
    const auto& e = envelope100Hz;
    const size_t n = e.size();
    if (n < 1200) return 0.f;
    // integrated, mean-removed profile
    double m = 0; for (float v : e) m += v; m /= n;
    std::vector<double> y(n);
    double acc = 0;
    for (size_t i = 0; i < n; i++) { acc += e[i] - m; y[i] = acc; }

    // scales (window sizes) log-spaced 0.31s..6.1s at 100 Hz
    std::vector<int> scales;
    for (double s = 31; s <= 610; s *= 1.3) scales.push_back((int)s);
    std::vector<double> logS, logF;
    for (int s : scales) {
        const size_t nw = n / s;
        if (nw < 4) break;
        double f2 = 0;
        for (size_t w = 0; w < nw; w++) {
            // linear detrend of y[w*s .. w*s+s) (least squares, textbook)
            const double* seg = y.data() + w * s;
            double sx = 0, sy = 0, sxx = 0, sxy = 0;
            for (int i = 0; i < s; i++) { sx += i; sy += seg[i]; sxx += (double)i * i; sxy += i * seg[i]; }
            const double den = s * sxx - sx * sx;
            const double b = den != 0 ? (s * sxy - sx * sy) / den : 0;
            const double a = (sy - b * sx) / s;
            for (int i = 0; i < s; i++) {
                const double r = seg[i] - (a + b * i);
                f2 += r * r;
            }
        }
        const double F = std::sqrt(f2 / (nw * s));
        if (F > 0) { logS.push_back(std::log((double)s)); logF.push_back(std::log(F)); }
    }
    if (logS.size() < 3) return 0.f;
    // slope of logF vs logS = alpha
    double sx = 0, sy = 0, sxx = 0, sxy = 0; const size_t k = logS.size();
    for (size_t i = 0; i < k; i++) { sx += logS[i]; sy += logF[i]; sxx += logS[i]*logS[i]; sxy += logS[i]*logF[i]; }
    const double den = k * sxx - sx * sx;
    const double alpha = den != 0 ? (k * sxy - sx * sy) / den : 1.0;
    // mapping tuned on the oracle corpus (EDM ~0.9-1.4 there)
    const double d = 1.35 / std::max(0.4, alpha);
    return (float)std::clamp(d, 0.0, 3.0);
}

// ---------------------------------------------------------------- fades (6.4)
struct Fades { float inEndS = 0.f, outStartS = -1.f; };
inline Fades detectFades(const std::vector<float>& rmsPerFrame, float fps) {
    Fades f;
    if (rmsPerFrame.size() < 10) return f;
    // reference level: 80th percentile of dB
    std::vector<float> dbv(rmsPerFrame.size());
    for (size_t i = 0; i < dbv.size(); i++) dbv[i] = db(rmsPerFrame[i]);
    std::vector<float> s = dbv;
    std::nth_element(s.begin(), s.begin() + (size_t)(s.size() * 0.8), s.end());
    const float ref = s[(size_t)(s.size() * 0.8)];
    const float lowT = ref - 20.f, highT = ref - 3.f;
    // fade-in: first crossing of highT if the track starts below lowT
    if (dbv.front() < lowT) {
        for (size_t i = 0; i < dbv.size(); i++)
            if (dbv[i] >= highT) { f.inEndS = i / fps; break; }
    }
    // fade-out: last crossing down through lowT preceded by highT level
    if (dbv.back() < lowT) {
        for (size_t i = dbv.size(); i-- > 0;)
            if (dbv[i] >= highT) { f.outStartS = i / fps; break; }
    }
    return f;
}

} // namespace showdsp
