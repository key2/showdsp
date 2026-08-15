// showdsp — live pipeline: audio-card-style chunk feed -> music.* widget
// states. Nothing sees the future. MIT license.
#pragma once
#include "dsp.h"
#include "spectral.h"
#include "onsets.h"
#include "tempo.h"

namespace showdsp {

// widget snapshot after each processed hop
struct LiveWidgets {
    double t = 0;               // stream time (s)
    float loud = 0;             // rolling-normalized RMS 0..1
    float energyLow = 0, energyMid = 0, energyHigh = 0;   // rolling 0..1
    float bright = 0;           // centroid / (sr/2)
    bool onset = false;         // pulse (true for one hop after detection)
    float bpm = 0;              // 0 until locked
    float beatPhase = 0;        // 0..1 while locked
    bool silence = false;       // below -60 dB
};

class LivePipeline {
public:
    float sr = 44100.f;
    static constexpr size_t kFrame = 2048, kHop = 512;

    LivePipeline()
        : _feed(kFrame, kHop), _fft(kFrame), _win(hann(kFrame)),
          _frame(kFrame), _mag(kFrame / 2 + 1) {
        _split.sr = sr;
        _split.edgesHz = {20, 150, 800, 4000, 16000};
        _split.prepare(kFrame / 2 + 1);
        _odf.prepare(kFrame / 2 + 1);
        _picker.fps = sr / kHop;
        _tempo.ossFps = sr / kHop;
        _tempo.prepare();
    }

    // push an audio-card chunk; invokes cb(widgets) after each hop
    template <typename Fn>
    void push(const float* x, size_t n, Fn&& cb) {
        _feed.push(x, n, [&](const float* fr) {
            _t += kHop / (double)sr;
            for (size_t i = 0; i < kFrame; i++) _frame[i] = fr[i] * _win[i];
            _fft.magnitude(_frame.data(), _mag.data());

            LiveWidgets w;
            w.t = _t;
            const float r = rms(fr + kFrame - kHop, kHop);
            w.loud = _loudNorm.push(r);
            w.silence = db(r) < -60.f;

            float b[4];
            _split.compute(_mag.data(), b);
            w.energyLow = _lowNorm.push(b[0]);
            w.energyMid = _midNorm.push(b[1]);
            w.energyHigh = _highNorm.push(b[3]);
            w.bright = centroidHz(_mag.data(), _mag.size(), sr) / (sr * 0.5f);

            const float o = _odf.push(_mag.data());
            w.onset = _picker.push(o, _t) >= 0;
            if (w.onset) _onsetCount++;

            _tempo.push(o, _t);
            w.bpm = _tempo.bpm();
            w.beatPhase = _tempo.beatPhase(_t);
            cb(w);
        });
    }

    size_t onsetCount() const { return _onsetCount; }

private:
    FrameFeed _feed;
    Fft _fft;
    std::vector<float> _win, _frame, _mag;
    BandSplit _split;
    SuperFluxOdf _odf;
    CausalOnsetPicker _picker;
    LiveTempo _tempo;
    RollNorm _loudNorm, _lowNorm, _midNorm, _highNorm;
    double _t = 0;
    size_t _onsetCount = 0;
};

} // namespace showdsp
