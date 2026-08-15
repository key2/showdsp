// showdsp-cli — analyze | live | selftest. MIT license.
#include <cstdio>
#include <cstring>
#include <chrono>
#include <random>
#include "../src/analyze.h"
#include "../src/live.h"

using namespace showdsp;

// ------------------------------------------------------------------ analyze
static int cmdAnalyze(int argc, char** argv) {
    for (int i = 0; i < argc; i++) {
        Audio a; std::string err;
        if (!decodeFile(argv[i], a, err)) {
            fprintf(stderr, "error: %s\n", err.c_str());
            return 1;
        }
        auto r = analyzeOffline(a);
        std::string base = argv[i];
        auto slash = base.find_last_of('/');
        if (slash != std::string::npos) base = base.substr(slash + 1);
        puts(offlineJson(r, base).c_str());
        fflush(stdout);
    }
    return 0;
}

// --------------------------------------------------------------------- live
static int cmdLive(int argc, char** argv) {
    if (argc < 1) { fprintf(stderr, "usage: showdsp-cli live <file>\n"); return 1; }
    Audio a; std::string err;
    if (!decodeFile(argv[0], a, err)) { fprintf(stderr, "error: %s\n", err.c_str()); return 1; }

    LivePipeline lp;
    const size_t chunk = 512;
    using clock = std::chrono::steady_clock;
    double cpuS = 0; size_t callbacks = 0;
    double bpmLockT = -1;
    std::vector<std::pair<double,float>> bpmTrace;
    float lastBpm = -1;

    for (size_t pos = 0; pos + chunk <= a.mono.size(); pos += chunk) {
        const auto c0 = clock::now();
        lp.push(a.mono.data() + pos, chunk, [&](const LiveWidgets& w) {
            if (w.bpm > 0 && std::fabs(w.bpm - lastBpm) > 0.05f) {
                bpmTrace.push_back({w.t, w.bpm});
                if (bpmLockT < 0) bpmLockT = w.t;
                lastBpm = w.bpm;
            }
        });
        cpuS += std::chrono::duration<double>(clock::now() - c0).count();
        callbacks++;
    }

    JsonW j;
    j.beginObj();
    std::string base = argv[0];
    auto slash = base.find_last_of('/');
    if (slash != std::string::npos) base = base.substr(slash + 1);
    j.kv("file", base);
    j.kv("duration_s", a.durationS(), 5);
    j.kv("callbacks", (double)callbacks, 8);
    j.kv("cpu_per_callback_us", 1e6 * cpuS / std::max<size_t>(1, callbacks), 4);
    j.kv("x_realtime", a.durationS() / std::max(1e-9, cpuS), 4);
    j.kv("onsets_detected", (double)lp.onsetCount(), 8);
    j.kv("onset_latency_ms", 1000.0 * (LivePipeline::kFrame / 2.0 + chunk) / 44100.0, 4);
    j.kv("bpm_first_lock_s", bpmLockT, 4);
    j.kv("bpm_final", bpmTrace.empty() ? 0.0 : bpmTrace.back().second, 5);
    j.key("bpm_trace"); j.beginArr();
    for (auto& [t, b] : bpmTrace) { j.beginArr(); j.num(t, 4); j.num(b, 5); j.endArr(); }
    j.endArr();
    j.endObj();
    puts(j.s.c_str());
    return 0;
}

// ----------------------------------------------------------------- selftest
static int g_fail = 0, g_pass = 0;
#define CHECK(name, cond) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; fprintf(stderr, "FAIL: %s\n", name); } \
} while (0)

static std::vector<float> sine(float hz, float sec, float amp = 0.5f, float sr = 44100) {
    std::vector<float> v((size_t)(sec * sr));
    for (size_t i = 0; i < v.size(); i++) v[i] = amp * std::sin(2.0 * kPi * hz * i / sr);
    return v;
}

static int cmdSelftest() {
    const float sr = 44100.f;

    { // FFT: 1 kHz sine peak lands on the right bin; Parseval sanity
        Fft fft(2048);
        auto w = hann(2048);
        auto s = sine(1000, 0.1f);
        std::vector<float> fr(2048), mag(1025);
        for (int i = 0; i < 2048; i++) fr[i] = s[i] * w[i];
        fft.magnitude(fr.data(), mag.data());
        size_t pk = std::max_element(mag.begin(), mag.end()) - mag.begin();
        const float hzPerBin = sr / 2048.f;
        CHECK("fft.peak_1khz", std::fabs(pk * hzPerBin - 1000.f) < 1.5f * hzPerBin);
        CHECK("fft.centroid_1khz", std::fabs(centroidHz(mag.data(), 1025, sr) - 1000.f) < 30.f);
    }
    { // bands: energy of a 100 Hz sine concentrates in the low band
        Fft fft(2048); auto w = hann(2048);
        auto s = sine(100, 0.1f);
        std::vector<float> fr(2048), mag(1025);
        for (int i = 0; i < 2048; i++) fr[i] = s[i] * w[i];
        fft.magnitude(fr.data(), mag.data());
        BandSplit sp; sp.sr = sr; sp.edgesHz = {20, 150, 800, 4000, 16000}; sp.prepare(1025);
        float b[4]; sp.compute(mag.data(), b);
        CHECK("bands.low_dominates", b[0] > 10 * (b[1] + b[2] + b[3]));
    }
    { // FrameFeed: irregular chunks still produce the right frame count
        FrameFeed f(2048, 512);
        std::vector<float> x(44100, 0.f);
        size_t frames = 0, i = 0;
        std::mt19937 rng(7);
        while (i < x.size()) {
            size_t n = std::min<size_t>(1 + rng() % 700, x.size() - i);
            f.push(x.data() + i, n, [&](const float*) { frames++; });
            i += n;
        }
        CHECK("framefeed.count", frames == 44100 / 512);
    }
    { // onsets: click train every 0.5 s over noise floor
        std::vector<float> x((size_t)(8 * sr), 0.f);
        std::mt19937 rng(3); std::normal_distribution<float> nd(0.f, 0.002f);
        for (auto& v : x) v = nd(rng);
        for (float t = 0.5f; t < 7.8f; t += 0.5f) {
            size_t p = (size_t)(t * sr);
            for (size_t k = 0; k < 300; k++) x[p + k] += 0.8f * std::sin(2.0 * kPi * 3000 * k / sr) * std::exp(-(float)k / 60.f);
        }
        Fft fft(2048); auto w = hann(2048);
        std::vector<float> fr(2048), mag(1025), odfv;
        SuperFluxOdf odf; odf.prepare(1025);
        FrameFeed feed(2048, 512);
        feed.push(x.data(), x.size(), [&](const float* fx) {
            for (int i = 0; i < 2048; i++) fr[i] = fx[i] * w[i];
            fft.magnitude(fr.data(), mag.data());
            odfv.push_back(odf.push(mag.data()));
        });
        auto on = pickOnsets(odfv, sr / 512.f);
        CHECK("onsets.count", std::fabs((double)on.size() - 15.0) <= 2.0);
        if (on.size() >= 3) {
            // spacing ~0.5 s
            double d = (on[2] - on[1]) * 512.0 / sr;
            CHECK("onsets.spacing", std::fabs(d - 0.5) < 0.06);
        } else CHECK("onsets.spacing", false);
    }
    { // tempo: synthetic 128 BPM kick pattern
        const float bpm = 128.f;
        std::vector<float> x((size_t)(30 * sr), 0.f);
        std::mt19937 rng(5); std::normal_distribution<float> nd(0.f, 0.005f);
        for (auto& v : x) v = nd(rng);
        const double beat = 60.0 / bpm;
        for (double t = 0.1; t < 29.5; t += beat) {
            size_t p = (size_t)(t * sr);
            for (size_t k = 0; k < 2000 && p + k < x.size(); k++)
                x[p + k] += 0.9f * std::sin(2.0 * kPi * 60 * k / sr) * std::exp(-(float)k / 400.f);
        }
        Fft fft(2048); auto w = hann(2048);
        std::vector<float> fr(2048), mag(1025), odfv;
        SuperFluxOdf odf; odf.prepare(1025);
        FrameFeed feed(2048, 512);
        feed.push(x.data(), x.size(), [&](const float* fx) {
            for (int i = 0; i < 2048; i++) fr[i] = fx[i] * w[i];
            fft.magnitude(fr.data(), mag.data());
            odfv.push_back(odf.push(mag.data()));
        });
        TempoEstimator te; te.ossFps = sr / 512.f;
        auto r = te.analyze(odfv);
        CHECK("tempo.bpm_128", std::fabs(r.bpm - bpm) < 1.0f);
        CHECK("tempo.confidence", r.confidence > 0.5f);
        CHECK("tempo.fold", std::fabs(foldBpm(64) - 128) < 1e-3 && std::fabs(foldBpm(256) - 128) < 1e-3);
    }
    { // key: A minor triad (A3 C4 E4 sines) -> "A minor"
        std::vector<float> x((size_t)(4 * sr), 0.f);
        for (float hz : {220.f, 261.63f, 329.63f}) {
            auto s = sine(hz, 4.f, 0.3f);
            for (size_t i = 0; i < x.size(); i++) x[i] += s[i];
        }
        Fft fft(4096); auto w = hann(4096);
        std::vector<float> fr(4096), mag(2049);
        Hpcp hp; std::vector<Peak> pk;
        std::vector<double> acc(hp.size, 0.0);
        std::vector<float> hf(hp.size);
        size_t nf = 0;
        FrameFeed feed(4096, 2048);
        feed.push(x.data(), x.size(), [&](const float* fx) {
            for (int i = 0; i < 4096; i++) fr[i] = fx[i] * w[i];
            fft.magnitude(fr.data(), mag.data());
            spectralPeaks(mag.data(), 2049, sr, pk);
            hp.compute(pk, hf.data());
            for (int i = 0; i < hp.size; i++) acc[i] += hf[i];
            nf++;
        });
        std::vector<float> avg(hp.size);
        for (int i = 0; i < hp.size; i++) avg[i] = (float)(acc[i] / std::max<size_t>(1, nf));
        float c12[12]; foldChroma(avg.data(), hp.size, c12);
        auto kk = estimateKey(c12, "krumhansl");
        CHECK("key.a_minor", kk.key == "A" && kk.scale == "minor");
        // tuning: A440-referenced content -> ~0 cents
        TuningEstimator tun; tun.push(pk);
        CHECK("key.tuning_near_0", std::fabs(tun.cents()) <= 3.f);
    }
    { // loudness: -20 dBFS 1 kHz sine ~ -20 LUFS (K-weighting ~0 dB @1k)
        auto sm = sine(1000, 5.f, 0.1f);   // -20 dBFS peak
        std::vector<float> st(sm.size() * 2);
        for (size_t i = 0; i < sm.size(); i++) { st[2*i] = sm[i]; st[2*i+1] = sm[i]; }
        auto r = loudnessAnalyze(st.data(), sm.size(), 2, 44100);
        CHECK("loudness.sine_lufs", std::fabs(r.integratedLufs - (-20.f - 3.01f)) < 1.5f || std::fabs(r.integratedLufs + 20.f) < 1.5f);
        CHECK("loudness.true_peak", std::fabs(r.truePeakDb + 20.f) < 0.5f);
    }
    { // qc: clipped square clips, clean sine doesn't; 50 Hz hum detected
        std::vector<float> sq((size_t)(2 * sr));
        for (size_t i = 0; i < sq.size(); i++) sq[i] = (std::sin(2.0 * kPi * 220 * i / sr) > 0 ? 1.f : -1.f);
        std::vector<float> dummyRms(100, 0.5f);
        auto r1 = qcAnalyze(sq.data(), sq.size(), sr, dummyRms, 86.f);
        CHECK("qc.clip_square", r1.clipPct > 50.f);
        auto sn = sine(220, 2.f, 0.5f);
        auto r2 = qcAnalyze(sn.data(), sn.size(), sr, dummyRms, 86.f);
        CHECK("qc.no_clip_sine", r2.clipPct < 0.1f);
        std::mt19937 rng(9); std::normal_distribution<float> nd(0.f, 0.05f);
        std::vector<float> hum((size_t)(35 * sr));
        for (size_t i = 0; i < hum.size(); i++) hum[i] = nd(rng) + 0.05f * std::sin(2.0 * kPi * 50 * i / sr);
        auto r3 = qcAnalyze(hum.data(), hum.size(), sr, dummyRms, 86.f);
        CHECK("qc.hum_50hz", r3.humHz == 50.f && r3.humDb > 10.f);
    }
    { // danceability: 4/4 kick envelope > white-noise envelope
        std::mt19937 rng(11); std::normal_distribution<float> nd(0.f, 0.3f);
        std::vector<float> envN(86 * 60), envK(86 * 60);
        for (auto& v : envN) v = std::fabs(nd(rng));
        const float beatFrames = 86.f * 60.f / 128.f;
        for (size_t i = 0; i < envK.size(); i++) {
            float ph = std::fmod((float)i, beatFrames) / beatFrames;
            envK[i] = std::exp(-6.f * ph) + 0.05f * std::fabs(nd(rng));
        }
        float dN = danceability(envN), dK = danceability(envK);
        CHECK("dfa.kick_gt_noise", dK > dN);
    }
    { // live pipeline: 128 BPM kicks -> locks near 128, onsets fire
        const float bpm = 128.f;
        std::vector<float> x((size_t)(40 * sr), 0.f);
        std::mt19937 rng(13); std::normal_distribution<float> nd(0.f, 0.004f);
        for (auto& v : x) v = nd(rng);
        for (double t = 0.1; t < 39.5; t += 60.0 / bpm) {
            size_t p = (size_t)(t * sr);
            for (size_t k = 0; k < 2000 && p + k < x.size(); k++)
                x[p + k] += 0.9f * std::sin(2.0 * kPi * 70 * k / sr) * std::exp(-(float)k / 350.f);
        }
        LivePipeline lp;
        float finalBpm = 0;
        for (size_t pos = 0; pos + 512 <= x.size(); pos += 512)
            lp.push(x.data() + pos, 512, [&](const LiveWidgets& w) { finalBpm = w.bpm; });
        CHECK("live.bpm_lock", std::fabs(finalBpm - bpm) < 2.f);
        CHECK("live.onsets", lp.onsetCount() > 60 && lp.onsetCount() < 110);
    }

    printf("selftest: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

int main(int argc, char** argv) {
    if (argc >= 2 && !strcmp(argv[1], "analyze")) return cmdAnalyze(argc - 2, argv + 2);
    if (argc >= 2 && !strcmp(argv[1], "live")) return cmdLive(argc - 2, argv + 2);
    if (argc >= 2 && !strcmp(argv[1], "selftest")) return cmdSelftest();
    fprintf(stderr,
        "showdsp-cli — clean-room audio analysis for light shows (MIT)\n"
        "usage:\n"
        "  showdsp-cli analyze <audio files...>   full-file analysis -> JSON lines\n"
        "  showdsp-cli live <audio file>          512-sample chunk simulation -> JSON\n"
        "  showdsp-cli selftest                   synthetic-signal checks\n");
    return 2;
}
