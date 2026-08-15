// showdsp — offline pipeline: full-file analysis -> JSON. MIT license.
// One 2048/512 STFT chain feeds bands/centroid/ODF/tempo/novelty; a
// 4096/2048 chain feeds tonal; libebur128 runs on the stereo signal.
#pragma once
#include "dsp.h"
#include "spectral.h"
#include "onsets.h"
#include "tempo.h"
#include "tonal.h"
#include "structure.h"
#include "qc.h"
#include "loudness.h"
#include "audiofile.h"
#include "jsonw.h"
#include <chrono>

namespace showdsp {

struct OfflineResult {
    double durationS = 0;
    // rhythm
    TempoResult tempo;
    std::vector<float> onsetsS;
    float onsetRate = 0;
    // tonal
    KeyResult keyEdma, keyKrumhansl;
    float tuningHz = 440;
    std::vector<float> chroma12;
    // loudness / dynamics
    LoudnessResult loud;
    float danceability = 0;
    Fades fades;
    // structure
    std::vector<float> sectionsS;
    std::vector<float> novelty;        // downsampled to ~10.8 Hz
    // curves at ~10.8 Hz (every 4th frame @ 86.13 fps)
    std::vector<float> curveLow, curveMid, curveHigh, curveLoud01;
    float bandMax[4] = {};             // normalization constants (low,mid,himid,high)
    // qc
    QcReport qc;
    double analysisSeconds = 0;
};

inline OfflineResult analyzeOffline(const Audio& a) {
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    OfflineResult R;
    R.durationS = a.durationS();
    const float sr = (float)a.sr;
    const float fps = sr / 512.f;

    // ---------------- chain A: 2048/512 ----------------
    Fft fft(2048);
    auto win = hann(2048);
    std::vector<float> frame(2048), mag(1025);
    BandSplit split; split.sr = sr;
    split.edgesHz = {20, 60, 150, 400, 800, 2000, 4000, 8000, 16000};
    split.prepare(1025);
    SuperFluxOdf odf; odf.prepare(1025);

    std::vector<float> rmsCurve, odfCurve, centroidCurve;
    std::vector<std::vector<float>> bandMatrix;

    FrameFeed feedA(2048, 512);
    feedA.push(a.mono.data(), a.mono.size(), [&](const float* x) {
        rmsCurve.push_back(rms(x, 2048));
        for (size_t i = 0; i < 2048; i++) frame[i] = x[i] * win[i];
        fft.magnitude(frame.data(), mag.data());
        std::vector<float> b(split.bands());
        split.compute(mag.data(), b.data());
        bandMatrix.push_back(b);
        centroidCurve.push_back(centroidHz(mag.data(), 1025, sr));
        odfCurve.push_back(odf.push(mag.data()));
    });
    const size_t nFrames = rmsCurve.size();
    if (!nFrames) return R;

    // ---- band curves + normalization constants
    // widget bands: low=20-150 (b0+b1), mid=150-800 (b2+b3),
    // himid=800-4000 (b4+b5), high=4000-16000 (b6+b7)
    std::vector<float> low(nFrames), mid(nFrames), himid(nFrames), high(nFrames);
    for (size_t t = 0; t < nFrames; t++) {
        const auto& b = bandMatrix[t];
        low[t] = b[0] + b[1]; mid[t] = b[2] + b[3];
        himid[t] = b[4] + b[5]; high[t] = b[6] + b[7];
        R.bandMax[0] = std::max(R.bandMax[0], low[t]);
        R.bandMax[1] = std::max(R.bandMax[1], mid[t]);
        R.bandMax[2] = std::max(R.bandMax[2], himid[t]);
        R.bandMax[3] = std::max(R.bandMax[3], high[t]);
    }
    auto ds = [&](const std::vector<float>& v, float norm) {
        std::vector<float> o; o.reserve(v.size() / 8 + 1);
        for (size_t i = 0; i < v.size(); i += 8)
            o.push_back(norm > 0 ? v[i] / norm : 0.f);
        return o;
    };
    R.curveLow = ds(low, R.bandMax[0]);
    R.curveMid = ds(mid, R.bandMax[1]);
    R.curveHigh = ds(high, R.bandMax[3]);

    // ---- onsets
    auto onsetIdx = pickOnsets(odfCurve, fps);
    for (size_t i : onsetIdx) R.onsetsS.push_back(i / fps);
    R.onsetRate = R.durationS > 0 ? R.onsetsS.size() / (float)R.durationS : 0;

    // ---- tempo (ODF doubles as the OSS)
    TempoEstimator te; te.ossFps = fps;
    R.tempo = te.analyze(odfCurve);

    // ---- novelty + sections
    R.novelty = noveltyCurve(bandMatrix, fps);
    R.sectionsS = sectionCandidates(R.novelty, fps);
    { // downsample novelty for storage
        std::vector<float> nds; nds.reserve(R.novelty.size() / 8 + 1);
        for (size_t i = 0; i < R.novelty.size(); i += 8) nds.push_back(R.novelty[i]);
        R.novelty.swap(nds);
    }

    // ---- danceability, fades
    R.danceability = danceability(rmsCurve);
    R.fades = detectFades(rmsCurve, fps);

    // ---------------- chain B: tonal 4096/2048 ----------------
    Fft fftB(4096);
    auto winB = hann(4096);
    std::vector<float> frameB(4096), magB(2049);
    Hpcp hp;
    TuningEstimator tun;
    std::vector<Peak> peaks;
    std::vector<double> chromaAcc(hp.size, 0.0);
    std::vector<float> hpcpFrame(hp.size);
    size_t nTonal = 0;
    FrameFeed feedB(4096, 2048);
    feedB.push(a.mono.data(), a.mono.size(), [&](const float* x) {
        for (size_t i = 0; i < 4096; i++) frameB[i] = x[i] * winB[i];
        fftB.magnitude(frameB.data(), magB.data());
        spectralPeaks(magB.data(), 2049, sr, peaks);
        if (peaks.empty()) return;
        tun.push(peaks);
        hp.compute(peaks, hpcpFrame.data());
        for (int i = 0; i < hp.size; i++) chromaAcc[i] += hpcpFrame[i];
        nTonal++;
    });
    if (nTonal) {
        std::vector<float> avg(hp.size);
        for (int i = 0; i < hp.size; i++) avg[i] = (float)(chromaAcc[i] / nTonal);
        float mx = *std::max_element(avg.begin(), avg.end());
        if (mx > 0) for (auto& v : avg) v /= mx;
        R.chroma12.resize(12);
        foldChroma(avg.data(), hp.size, R.chroma12.data());
        R.keyEdma = estimateKey(R.chroma12.data(), "edma");
        R.keyKrumhansl = estimateKey(R.chroma12.data(), "krumhansl");
        R.tuningHz = tun.hz();
    }

    // ---------------- loudness (stereo) ----------------
    R.loud = loudnessAnalyze(a.stereo.data(), a.stereo.size() / 2, 2, a.sr);
    // 0..1 loud curve for the music.loud widget (map -60..0 LUFS)
    R.curveLoud01.reserve(R.loud.momentary10Hz.size());
    for (float m : R.loud.momentary10Hz)
        R.curveLoud01.push_back(std::clamp((m + 60.f) / 60.f, 0.f, 1.f));

    // ---------------- qc ----------------
    R.qc = qcAnalyze(a.mono.data(), a.mono.size(), sr, rmsCurve, fps);

    R.analysisSeconds = std::chrono::duration<double>(clock::now() - t0).count();
    return R;
}

inline std::string offlineJson(const OfflineResult& R, const std::string& file) {
    JsonW j;
    j.beginObj();
    j.kv("file", file);
    j.kv("duration_s", R.durationS, 5);
    j.key("rhythm"); j.beginObj();
      j.kv("bpm", R.tempo.bpm, 5);
      j.kv("bpm_confidence", R.tempo.confidence, 3);
      j.kv("bpm_stability", R.tempo.stability, 3);
      j.key("bpm_candidates"); j.beginArr();
      for (auto& [b, w] : R.tempo.candidates) { j.beginArr(); j.num(b, 5); j.num(w, 3); j.endArr(); }
      j.endArr();
      j.kv("onset_count", (double)R.onsetsS.size(), 6);
      j.kv("onset_rate_hz", R.onsetRate, 4);
      j.arr("onsets_s", R.onsetsS, 5);
    j.endObj();
    j.key("tonal"); j.beginObj();
      j.kv("key_edma", R.keyEdma.key + " " + R.keyEdma.scale);
      j.kv("key_edma_strength", R.keyEdma.strength, 3);
      j.kv("key_krumhansl", R.keyKrumhansl.key + " " + R.keyKrumhansl.scale);
      j.kv("key_krumhansl_strength", R.keyKrumhansl.strength, 3);
      j.kv("tuning_hz", R.tuningHz, 5);
      j.arr("chroma12_A_ref", R.chroma12, 3);
    j.endObj();
    j.key("loudness"); j.beginObj();
      j.kv("integrated_lufs", R.loud.integratedLufs, 4);
      j.kv("range_lu", R.loud.rangeLu, 3);
      j.kv("true_peak_db", R.loud.truePeakDb, 4);
      j.kv("momentary_max_lufs", R.loud.momentaryMaxLufs, 4);
      j.kv("track_gain_db", R.loud.trackGainDb, 3);
    j.endObj();
    j.key("structure"); j.beginObj();
      j.arr("sections_s", R.sectionsS, 4);
      j.kv("danceability", R.danceability, 3);
      j.kv("fade_in_end_s", R.fades.inEndS, 4);
      j.kv("fade_out_start_s", R.fades.outStartS, 4);
    j.endObj();
    j.key("curves"); j.beginObj();
      j.kv("rate_hz", 44100.0 / 512.0 / 8.0, 5);
      j.arr("low", R.curveLow, 3);
      j.arr("mid", R.curveMid, 3);
      j.arr("high", R.curveHigh, 3);
      j.key("loud01_10hz"); j.beginArr();
      for (float v : R.curveLoud01) j.num(v, 3);
      j.endArr();
      j.key("novelty"); j.beginArr();
      for (float v : R.novelty) j.num(v, 3);
      j.endArr();
      j.key("band_max"); j.beginArr();
      for (float v : R.bandMax) j.num(v, 5);
      j.endArr();
    j.endObj();
    j.key("qc"); j.beginObj();
      j.kv("clip_pct", R.qc.clipPct, 3);
      j.kv("clip_runs", (double)R.qc.clipRuns, 6);
      j.kv("hum_db", R.qc.humDb, 3);
      j.kv("hum_hz", R.qc.humHz, 4);
      j.kv("gap_count", (double)R.qc.gapCount, 6);
    j.endObj();
    j.kv("analysis_seconds", R.analysisSeconds, 4);
    j.kv("x_realtime", R.analysisSeconds > 0 ? R.durationS / R.analysisSeconds : 0, 4);
    j.endObj();
    return j.s;
}

} // namespace showdsp
