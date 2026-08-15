// showdsp — clean-room audio analysis for light shows. MIT license.
// Tier 0: DSP plumbing — FFT, windows, frame utils, small math helpers.
//
// Provenance: textbook material only (Cooley–Tukey radix-2 FFT, Hann
// window, dB conversions). No third-party analysis code consulted.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <complex>
#include <vector>
#include <algorithm>
#include <numeric>

namespace showdsp {

constexpr double kPi = 3.14159265358979323846;

// ---------------------------------------------------------------- FFT (0.2)
// Iterative radix-2 Cooley–Tukey, power-of-two sizes, precomputed twiddles.
// Real input -> magnitude spectrum of N/2+1 bins. Good to ~1e-6 rel error.
class Fft {
public:
    explicit Fft(size_t n) : _n(n) {
        _rev.resize(n);
        size_t log2n = 0; while ((1u << log2n) < n) ++log2n;
        for (size_t i = 0; i < n; i++) {
            size_t r = 0;
            for (size_t b = 0; b < log2n; b++) if (i & (1u << b)) r |= 1u << (log2n - 1 - b);
            _rev[i] = r;
        }
        _tw.resize(n / 2);
        for (size_t i = 0; i < n / 2; i++)
            _tw[i] = std::complex<float>((float)std::cos(-2.0 * kPi * i / n),
                                         (float)std::sin(-2.0 * kPi * i / n));
        _buf.resize(n);
    }
    size_t size() const { return _n; }

    // complex in-place transform of _buf
    void transform() {
        auto& a = _buf;
        const size_t n = _n;
        for (size_t len = 2; len <= n; len <<= 1) {
            const size_t half = len >> 1, step = n / len;
            for (size_t i = 0; i < n; i += len)
                for (size_t j = 0; j < half; j++) {
                    auto u = a[i + j];
                    auto v = a[i + j + half] * _tw[j * step];
                    a[i + j] = u + v;
                    a[i + j + half] = u - v;
                }
        }
    }

    // real signal (n samples) -> magnitudes (n/2+1). Window applied by caller.
    void magnitude(const float* x, float* mag) {
        for (size_t i = 0; i < _n; i++) _buf[_rev[i]] = std::complex<float>(x[i], 0.f);
        transform();
        const size_t nb = _n / 2 + 1;
        for (size_t i = 0; i < nb; i++) mag[i] = std::abs(_buf[i]);
    }

private:
    size_t _n;
    std::vector<size_t> _rev;
    std::vector<std::complex<float>> _tw;
    std::vector<std::complex<float>> _buf;
};

// ------------------------------------------------------------ windows (0.1)
inline std::vector<float> hann(size_t n, bool normalized = true) {
    std::vector<float> w(n);
    double s = 0;
    for (size_t i = 0; i < n; i++) {
        w[i] = 0.5f - 0.5f * (float)std::cos(2.0 * kPi * i / (n - 1));
        s += w[i];
    }
    if (normalized && s > 0)                 // unit-sum so magnitudes are
        for (auto& v : w) v = (float)(2.0 * v / s);  // comparable across sizes
    return w;
}

// ------------------------------------------------------------- utils (0.4)
inline float db(float lin, float floor_db = -120.f) {
    return lin > 0 ? std::max(floor_db, 20.f * std::log10(lin)) : floor_db;
}
inline float power_db(float p, float floor_db = -120.f) {
    return p > 0 ? std::max(floor_db, 10.f * std::log10(p)) : floor_db;
}

// exponential moving average
struct Ema {
    float a = 0.1f, y = 0.f; bool init = false;
    float push(float x) { y = init ? a * x + (1 - a) * y : (init = true, x); return y; }
};

// rolling max normalizer with slow decay (live 0..1 mapping)
struct RollNorm {
    float mx = 1e-9f, decay = 0.9999f;
    float push(float x) { mx = std::max(x, mx * decay); return x / mx; }
};

inline float mean(const float* x, size_t n) {
    return n ? std::accumulate(x, x + n, 0.f) / (float)n : 0.f;
}
inline float stddev(const float* x, size_t n, float m) {
    if (n < 2) return 0.f;
    float s = 0; for (size_t i = 0; i < n; i++) s += (x[i] - m) * (x[i] - m);
    return std::sqrt(s / (float)n);
}

// parabolic interpolation around a discrete peak (textbook: JOS SASP)
// returns offset in [-0.5, 0.5] and interpolated height
inline void parabolic(float ym1, float y0, float yp1, float& off, float& height) {
    const float d = ym1 - 2.f * y0 + yp1;
    off = (d != 0.f) ? 0.5f * (ym1 - yp1) / d : 0.f;
    off = std::clamp(off, -0.5f, 0.5f);
    height = y0 - 0.25f * (ym1 - yp1) * off;
}

// --------------------------------------------------------- frame feed (0.3)
// Push arbitrary chunk sizes; emits a frame of the last `frame` samples
// every `hop` new samples (frames overlap when hop < frame).
class FrameFeed {
public:
    FrameFeed(size_t frame, size_t hop)
        : _hop(hop), _buf(frame, 0.f), _stage(hop, 0.f) {}
    template <typename Fn>
    void push(const float* x, size_t n, Fn&& onFrame) {
        size_t i = 0;
        while (i < n) {
            const size_t take = std::min(n - i, _hop - _fill);
            std::copy(x + i, x + i + take, _stage.begin() + _fill);
            _fill += take; i += take;
            if (_fill == _hop) {
                _fill = 0;
                std::move(_buf.begin() + _hop, _buf.end(), _buf.begin());
                std::copy(_stage.begin(), _stage.end(), _buf.end() - _hop);
                onFrame(_buf.data());
            }
        }
    }
private:
    size_t _hop, _fill = 0;
    std::vector<float> _buf, _stage;
};

} // namespace showdsp
