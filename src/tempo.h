// showdsp — Tier 4: tempo estimation + live beat phase. MIT license.
//
// Architecture inspired by the *published description* in:
//   [1] G. Percival, G. Tzanetakis, "Streamlined Tempo Estimation Based on
//       Autocorrelation and Cross-correlation With Pulses", IEEE/ACM
//       TASLP 22(12), 2014.
// Stages: onset-strength signal (we reuse the SuperFlux ODF) -> windowed
// autocorrelation with harmonic enhancement -> comb (pulse-train) scoring
// of candidate periods with phase search -> cross-window accumulation ->
// octave fold 70..180 -> histogram peak + parabolic refinement.
// Confidence (our own design, replaces nothing verbatim): histogram
// dominance x window-winner stability.
// Live: same core on a rolling OSS ring + hysteresis; beat phase via
// exponentially-weighted comb alignment (our own design).
// All numeric constants chosen by us and tuned against the oracle corpus.
#pragma once
#include "dsp.h"
#include <map>

namespace showdsp {

struct TempoResult {
    float bpm = 0;
    float confidence = 0;          // 0..1
    std::vector<std::pair<float,float>> candidates; // (bpm, weight) top-5
    float stability = 0;           // fraction of windows agreeing ±2 BPM
};

namespace detail {

inline float lagInterp(const std::vector<float>& v, float idx) {
    if (idx < 0 || idx >= (float)v.size() - 1) return 0.f;
    const size_t i = (size_t)idx; const float f = idx - i;
    return v[i] * (1 - f) + v[i + 1] * f;
}

// best-phase mean comb sum for period `tau` (in OSS samples) over
// window w[0..n), impulses walking backwards from the end.
inline float combScore(const float* w, size_t n, float tau) {
    if (tau < 2.f || tau >= (float)n) return 0.f;
    float best = 0;
    const int phases = 16;
    for (int p = 0; p < phases; p++) {
        const float phi = tau * p / phases;
        double s = 0; int c = 0;
        for (float t = (float)n - 1 - phi; t >= 0; t -= tau) {
            // linear interp read
            size_t i = (size_t)t; float f = t - i;
            float v = (i + 1 < n) ? w[i] * (1 - f) + w[i + 1] * f : w[i];
            s += v; c++;
        }
        if (c > 0) best = std::max(best, (float)(s / c));
    }
    return best;
}

} // namespace detail

// Analyze one OSS window -> scored BPM candidates.
// ossFps: rate of the onset strength signal.
inline void tempoWindow(const float* w, size_t n, float ossFps,
                        std::vector<std::pair<float,float>>& out /*(bpm,score)*/) {
    out.clear();
    // mean removal
    std::vector<float> x(w, w + n);
    float m = mean(x.data(), n);
    for (auto& v : x) v = std::max(0.f, v - m);

    const float bpmLo = 50, bpmHi = 210;
    const int lagMin = std::max(2, (int)std::floor(ossFps * 60.f / bpmHi));
    const int lagMax = std::min((int)n - 2, (int)std::ceil(ossFps * 60.f / bpmLo));
    if (lagMax <= lagMin + 2) return;

    // autocorrelation over lag range (normalized by overlap length)
    std::vector<float> ac(lagMax + 1, 0.f);
    for (int lag = lagMin; lag <= lagMax; lag++) {
        double s = 0;
        for (size_t i = lag; i < n; i++) s += (double)x[i] * x[i - lag];
        ac[lag] = (float)(s / (double)(n - lag));
    }
    // magnitude compression + harmonic enhancement (add stretched copies)
    float acMax = 0; for (float v : ac) acMax = std::max(acMax, v);
    if (acMax <= 0) return;
    for (auto& v : ac) v = std::sqrt(std::max(0.f, v / acMax));
    std::vector<float> eac(ac.size(), 0.f);
    for (int lag = lagMin; lag <= lagMax; lag++)
        eac[lag] = ac[lag]
                 + 0.5f * detail::lagInterp(ac, 2.f * lag)
                 + 0.25f * detail::lagInterp(ac, 4.f * lag);

    // top local maxima as candidates
    std::vector<std::pair<float,float>> peaks; // (lag, eac)
    for (int lag = lagMin + 1; lag < lagMax; lag++)
        if (eac[lag] >= eac[lag - 1] && eac[lag] > eac[lag + 1]) {
            float off, h; parabolic(eac[lag - 1], eac[lag], eac[lag + 1], off, h);
            peaks.push_back({(float)lag + off, h});
        }
    std::sort(peaks.begin(), peaks.end(),
              [](auto& a, auto& b) { return a.second > b.second; });
    if (peaks.size() > 5) peaks.resize(5);

    // pulse-train (comb) scoring with harmonic support:
    // true tempos score at tau, tau/2 and 2*tau simultaneously.
    for (auto& [lag, h] : peaks) {
        const float c1 = detail::combScore(x.data(), n, lag);
        const float c2 = detail::combScore(x.data(), n, lag * 0.5f);
        const float c4 = detail::combScore(x.data(), n, lag * 2.0f);
        const float score = c1 + 0.35f * c2 + 0.35f * c4;
        const float bpm = ossFps * 60.f / lag;
        out.push_back({bpm, score * h});
    }
}

// fold a BPM into [lo, hi)
inline float foldBpm(float b, float lo = 70.f, float hi = 180.f) {
    if (b <= 0) return 0;
    while (b < lo) b *= 2.f;
    while (b >= hi) b *= 0.5f;
    return b;
}

// fine refinement: maximize the comb score over a small BPM neighborhood
// (the comb alignment is far more period-sensitive than the smoothed
// autocorrelation histogram, which carries a slight low bias).
// Optionally returns the best score for candidate arbitration.
inline float refineBpm(const float* oss, size_t n, float ossFps,
                       float bpmGuess, float rangeBpm = 2.5f, float stepBpm = 0.05f,
                       float* outScore = nullptr) {
    // mean-removed, rectified copy
    std::vector<float> x(oss, oss + n);
    const float m = mean(x.data(), n);
    for (auto& v : x) v = std::max(0.f, v - m);
    float bestBpm = bpmGuess, bestS = -1;
    for (float b = bpmGuess - rangeBpm; b <= bpmGuess + rangeBpm; b += stepBpm) {
        if (b < 40 || b > 220) continue;
        const float tau = ossFps * 60.f / b;
        // also demand support at the half period (off-beats) — sharpens
        // the maximum and resists k/4-type aliases
        const float s = detail::combScore(x.data(), n, tau)
                      + 0.5f * detail::combScore(x.data(), n, tau * 0.5f);
        if (s > bestS) { bestS = s; bestBpm = b; }
    }
    if (outScore) *outScore = bestS;
    return bestBpm;
}

// Offline estimator: slide 6 s windows, accumulate candidate votes into a
// folded histogram, report peak + confidence.
class TempoEstimator {
public:
    float ossFps = 86.1328125f;      // 44100/512
    float windowS = 6.f, hopS = 1.5f;

    TempoResult analyze(const std::vector<float>& oss) {
        TempoResult r;
        const size_t n = oss.size();
        const size_t win = (size_t)(windowS * ossFps);
        const size_t hop = (size_t)(hopS * ossFps);
        if (n < win) return r;

        // histogram 70..180 at 0.25 BPM
        const float lo = 70, hi = 180, step = 0.25f;
        std::vector<float> hist((size_t)((hi - lo) / step) + 1, 0.f);
        std::vector<float> winners;
        std::vector<std::pair<float,float>> cands;

        for (size_t s = 0; s + win <= n; s += hop) {
            tempoWindow(oss.data() + s, win, ossFps, cands);
            if (cands.empty()) continue;
            // winner of this window (max score)
            auto best = *std::max_element(cands.begin(), cands.end(),
                        [](auto& a, auto& b) { return a.second < b.second; });
            winners.push_back(foldBpm(best.first));
            // accumulate all candidates as gaussian bumps (sigma 1 BPM)
            for (auto& [bpm, sc] : cands) {
                const float fb = foldBpm(bpm);
                for (int k = -8; k <= 8; k++) {
                    const float b = fb + k * step;
                    if (b < lo || b >= hi) continue;
                    const float d = (b - fb);
                    hist[(size_t)((b - lo) / step)] += sc * std::exp(-0.5f * d * d);
                }
            }
        }
        if (winners.empty()) return r;

        // top aggregated histogram candidates (1-BPM bins), non-adjacent
        std::map<int,float> agg;
        for (size_t i = 0; i < hist.size(); i++)
            agg[(int)std::lround(lo + i * step)] += hist[i];
        std::vector<std::pair<float,float>> top(agg.size());
        std::transform(agg.begin(), agg.end(), top.begin(),
                       [](auto& kv) { return std::make_pair((float)kv.first, kv.second); });
        std::sort(top.begin(), top.end(), [](auto&a, auto&b){ return a.second > b.second; });
        std::vector<std::pair<float,float>> distinct;
        for (auto& [b, wgt] : top) {
            bool near = false;
            for (auto& [db2, _] : distinct) if (std::fabs(db2 - b) <= 3.f) { near = true; break; }
            if (!near) distinct.push_back({b, wgt});
            if (distinct.size() >= 5) break;
        }
        float tmax = distinct.empty() ? 1.f : distinct[0].second;
        for (auto& t : distinct) t.second /= tmax;
        r.candidates = distinct;

        // arbitrate among candidates: refine each, score with full-OSS comb
        // support; histogram weight acts as a mild prior for near-ties.
        float bestScore = -1;
        for (auto& [cb, cw] : distinct) {
            float sc = 0;
            const float rb = refineBpm(oss.data(), n, ossFps, cb, 2.5f, 0.05f, &sc);
            sc *= (0.85f + 0.15f * cw);
            if (sc > bestScore) { bestScore = sc; r.bpm = rb; }
        }

        // dominance: mass within ±2 BPM of the chosen tempo vs total
        double total = 0, local = 0;
        for (size_t i = 0; i < hist.size(); i++) {
            total += hist[i];
            if (std::abs(lo + i * step - r.bpm) <= 2.f) local += hist[i];
        }
        const float dominance = total > 0 ? (float)(local / total) : 0.f;
        // stability: window winners within ±2 of final (also octave-folded)
        int ok = 0;
        for (float wbpm : winners)
            if (std::abs(foldBpm(wbpm) - foldBpm(r.bpm)) <= 2.f) ok++;
        r.stability = (float)ok / winners.size();
        r.confidence = std::clamp(0.55f * r.stability + 0.45f * dominance, 0.f, 1.f);
        return r;
    }
};

// Live estimator: rolling OSS ring; update() every ~2 s; hysteresis so the
// public BPM only changes when a new estimate persists. Beat phase from
// exponentially-decayed comb alignment at the accepted BPM (own design).
class LiveTempo {
public:
    float ossFps = 86.1328125f;
    float ringS = 12.f;
    float updateEveryS = 2.f;

    void prepare() {
        _ring.assign((size_t)(ringS * ossFps), 0.f);
        _fill = 0;
    }
    // push one OSS sample at time tNow (seconds); returns true if the
    // public estimate was re-evaluated
    bool push(float v, double tNow) {
        if (_ring.empty()) prepare();
        std::move(_ring.begin() + 1, _ring.end(), _ring.begin());
        _ring.back() = v;
        if (_fill < _ring.size()) _fill++;
        bool updated = false;
        if (_fill >= _ring.size() && tNow >= _nextUpdate) {
            _nextUpdate = tNow + updateEveryS;
            update(tNow);
            updated = true;
        }
        // continuous phase advance
        return updated;
    }

    float bpm() const { return _bpm; }
    // 0..1 position inside the current beat (only meaningful once locked)
    float beatPhase(double tNow) const {
        if (_bpm <= 0) return 0.f;
        const double period = 60.0 / _bpm;
        double ph = std::fmod(tNow - _anchor, period);
        if (ph < 0) ph += period;
        return (float)(ph / period);
    }
    bool locked() const { return _bpm > 0; }

private:
    void update(double tNow) {
        std::vector<std::pair<float,float>> cands;
        tempoWindow(_ring.data(), _ring.size(), ossFps, cands);
        if (cands.empty()) return;
        auto best = *std::max_element(cands.begin(), cands.end(),
                    [](auto& a, auto& b) { return a.second < b.second; });
        float est = foldBpm(best.first);
        est = refineBpm(_ring.data(), _ring.size(), ossFps, est);
        // hysteresis: accept if close to current, or seen twice in a row
        if (_bpm > 0 && std::abs(est - _bpm) <= 2.f) {
            _bpm = 0.7f * _bpm + 0.3f * est;   // gentle tracking
        } else if (_pending > 0 && std::abs(est - _pendingBpm) <= 2.f) {
            _bpm = est; _pending = 0;
        } else {
            _pendingBpm = est; _pending = 1;
        }
        if (_bpm > 0) alignPhase(tNow);
    }

    // choose the beat anchor: best comb phase over the last ~4 s with
    // exponential decay favoring recent energy
    void alignPhase(double tNow) {
        const float period = ossFps * 60.f / _bpm;      // OSS samples
        const size_t span = std::min(_ring.size(), (size_t)(4.f * ossFps));
        const float* w = _ring.data() + (_ring.size() - span);
        float bestS = -1, bestPhi = 0;
        const int phases = 32;
        for (int p = 0; p < phases; p++) {
            const float phi = period * p / phases;
            double s = 0; double wsum = 0;
            for (float t = (float)span - 1 - phi; t >= 0; t -= period) {
                size_t i = (size_t)t; float f = t - i;
                float v = (i + 1 < span) ? w[i] * (1 - f) + w[i + 1] * f : w[i];
                double decay = std::exp(-(double)((span - 1) - t) / (2.0 * ossFps));
                s += v * decay; wsum += decay;
            }
            if (wsum > 0 && s / wsum > bestS) { bestS = (float)(s / wsum); bestPhi = phi; }
        }
        // anchor so that a beat falls at (now - bestPhi/ossFps)
        _anchor = tNow - bestPhi / ossFps;
    }

    std::vector<float> _ring;
    size_t _fill = 0;
    double _nextUpdate = 12.0;
    float _bpm = 0, _pendingBpm = 0;
    int _pending = 0;
    double _anchor = 0;
};

} // namespace showdsp
