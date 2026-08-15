# showdsp API documentation

Clean-room audio analysis for light-show software. C++17, header-only
analysis modules under `namespace showdsp`, one small CLI. MIT.

- [Conventions](#conventions)
- [CLI reference](#cli-reference)
- [Quick start (C++)](#quick-start-c)
- [Module reference](#module-reference)
- [Mapping to `music.*` widgets](#mapping-to-music-widgets)
- [Realtime & threading notes](#realtime--threading-notes)

---

## Conventions

| Convention | Value |
|---|---|
| Sample format | `float` (f32), mono for analysis, interleaved stereo for loudness |
| Sample rate | 44 100 Hz throughout (decoder resamples to it) |
| Analysis frame | 2048 samples, hop 512 → **86.13 frames/s** ("fps" below); tonal chain 4096/2048 |
| Spectrum | magnitude, `N/2+1` bins (1025 for the 2048 chain) |
| Time | seconds (`double` for timestamps), media-local |
| Chroma | 12 or 36 bins, **bin 0 = pitch class A**, reference 440 Hz |
| Loudness | LUFS / LU (EBU R128), true peak in dBTP (can exceed 0) |
| Errors | decode returns `bool` + error string; analysis never throws, returns zeros/empties on degenerate input |
| Headers | every module is a standalone `#include`; only `audiofile` and the vendored C deps need linking |

---

## CLI reference

```
showdsp-cli analyze <audio files...>   # offline: one JSON object per line/file
showdsp-cli live <audio file>          # feeds 512-sample chunks as a fake audio card
showdsp-cli selftest                   # 19 synthetic-signal checks, exit 0/1
```

Exit codes: `0` ok, `1` decode/analysis error or selftest failure, `2` usage.

### `analyze` output schema

One JSON object per input file (real example values):

| Path | Type | Units / range | Meaning |
|---|---|---|---|
| `file` | string | — | basename of the input |
| `duration_s` | number | s | decoded duration |
| `rhythm.bpm` | number | BPM, folded 70–180 | arbitrated tempo (see `TempoEstimator`) |
| `rhythm.bpm_confidence` | number | 0–1 | 0.55·stability + 0.45·histogram dominance; **< 0.5 = treat as unreliable** |
| `rhythm.bpm_stability` | number | 0–1 | fraction of 6 s windows agreeing ±2 BPM |
| `rhythm.bpm_candidates` | [[bpm, weight]…] | ≤5 entries, weight 0–1 | distinct histogram peaks, best-first |
| `rhythm.onset_count` | number | — | detected onsets |
| `rhythm.onset_rate_hz` | number | onsets/s | density |
| `rhythm.onsets_s` | [number] | s | onset times (frame-quantized, ±11.6 ms) |
| `tonal.key_edma` | string | e.g. `"F minor"` | key via EDM profiles (Faraldo 2016) |
| `tonal.key_edma_strength` | number | ~0–1 | Pearson corr. of best profile match |
| `tonal.key_krumhansl` / `_strength` | string / number | — | same with Krumhansl profiles |
| `tonal.tuning_hz` | number | Hz | estimated A4 reference (±50 cents around 440) |
| `tonal.chroma12_A_ref` | [12 numbers] | relative | track-average chroma, bin 0 = A |
| `loudness.integrated_lufs` | number | LUFS | EBU R128 gated program loudness |
| `loudness.range_lu` | number | LU | loudness range (LRA) |
| `loudness.true_peak_db` | number | dBTP | max channel true peak (may exceed 0) |
| `loudness.momentary_max_lufs` | number | LUFS | max 400 ms loudness |
| `loudness.track_gain_db` | number | dB | gain to reach −18 LUFS reference |
| `structure.sections_s` | [number] | s | novelty-peak section-change candidates (≤8, ≥8 s apart) |
| `structure.danceability` | number | 0–3 | DFA-based (higher = more danceable) |
| `structure.fade_in_end_s` | number | s | 0 when no fade-in |
| `structure.fade_out_start_s` | number | s | −1 when no fade-out |
| `curves.rate_hz` | number | Hz | rate of the band/novelty curves (≈10.77) |
| `curves.low/mid/high` | [number] | 0–1 | band-energy curves, normalized by `band_max` |
| `curves.loud01_10hz` | [number] | 0–1 | momentary loudness mapped from −60…0 LUFS (10 Hz) |
| `curves.novelty` | [number] | 0–1 | novelty curve (downsampled) |
| `curves.band_max` | [4 numbers] | energy | normalization constants: low 20–150, mid 150–800, himid 800–4k, high 4–16 kHz |
| `qc.clip_pct` | number | % | samples inside flat-top clipped runs |
| `qc.clip_runs` | number | — | count of such runs |
| `qc.hum_db` | number | dB | mains-hum prominence over neighbors (>10 = suspicious) |
| `qc.hum_hz` | number | 50/60/100/120 | strongest hum candidate |
| `qc.gap_count` | number | — | silent holes (>0.15 s below −60 dB) between loud audio |
| `analysis_seconds`, `x_realtime` | number | s, × | wall time and speed factor |

### `live` output schema

| Field | Meaning |
|---|---|
| `callbacks`, `cpu_per_callback_us` | number of 512-sample chunks and mean cost each (measured ≈15.6 µs) |
| `x_realtime` | processing speed factor |
| `onsets_detected`, `onset_latency_ms` | causal onsets; fixed latency = frame/2 + 1 hop ≈ 34.8 ms |
| `bpm_first_lock_s` | time of first accepted BPM (ring 12 s + 2-update hysteresis → ≥14 s) |
| `bpm_final`, `bpm_trace` | last estimate and `[t, bpm]` change points |

---

## Quick start (C++)

### Offline: file → everything

```cpp
#include "audiofile.h"
#include "analyze.h"
using namespace showdsp;

Audio a; std::string err;
if (!decodeFile("track.mp3", a, err)) { /* err */ }
OfflineResult r = analyzeOffline(a);
// r.tempo.bpm, r.tempo.confidence, r.keyEdma.key, r.onsetsS,
// r.loud.integratedLufs, r.sectionsS, r.curveLow, r.qc.clipPct ...
std::string json = offlineJson(r, "track.mp3");
```

### Live: audio-card chunks → widget states

```cpp
#include "live.h"
using namespace showdsp;

LivePipeline lp;                       // 44.1 kHz, frame 2048, hop 512
void audioCallback(const float* mono, size_t n) {   // any chunk size
    lp.push(mono, n, [](const LiveWidgets& w) {
        // w.t (s), w.loud, w.energyLow/Mid/High, w.bright  — all 0..1
        // w.onset (one-hop pulse), w.silence
        // w.bpm (0 until locked), w.beatPhase (0..1 sawtooth)
    });
}
```

The callback fires once per hop (11.6 ms). All values are safe to forward
directly to OSC/DMX; normalization is rolling and self-adapting.

---

## Module reference

### `dsp.h` — plumbing (tier 0)

```cpp
constexpr double kPi;

class Fft {                       // radix-2 Cooley–Tukey, power-of-two only
    explicit Fft(size_t n);
    size_t size() const;
    void magnitude(const float* x, float* mag);  // n reals -> n/2+1 mags
};

std::vector<float> hann(size_t n, bool normalized = true);
// normalized=true: unit-sum window scaled by 2 — a full-scale sine reads
// ~n/2-independent magnitude; keep one convention across chains.

float db(float lin, float floor_db = -120.f);        // 20*log10
float power_db(float p, float floor_db = -120.f);    // 10*log10

struct Ema      { float a, y; float push(float x); };        // 1-pole smoother
struct RollNorm { float mx, decay; float push(float x); };   // rolling-max 0..1

float mean(const float* x, size_t n);
float stddev(const float* x, size_t n, float mean);

void parabolic(float ym1, float y0, float yp1, float& off, float& height);
// 3-point peak interpolation; off in [-0.5, 0.5] bins

class FrameFeed {                 // arbitrary chunks -> overlapped frames
    FrameFeed(size_t frame, size_t hop);
    template <typename Fn> void push(const float* x, size_t n, Fn&& onFrame);
    // onFrame(const float* frame) called once per hop with the last
    // `frame` samples. First frames contain leading zeros (warm-up).
};
```

Costs: `Fft(2048).magnitude` ≈ 8 µs; `FrameFeed::push` is a copy.

### `spectral.h` — instant features (tier 1)

```cpp
struct BandSplit {
    std::vector<float> edgesHz;   // e.g. {20,150,800,4000,16000}
    float sr;                     // set before prepare
    void prepare(size_t bins);    // bins = N/2+1 of your FFT
    void compute(const float* mag, float* out) const;  // energies per band
    size_t bands() const;         // edges-1
};

float rms(const float* x, size_t n);                       // time domain
float centroidHz(const float* mag, size_t nbins, float sr);// brightness, Hz
struct Flux { float push(const float* mag, size_t nbins); };// L2 half-rect
float hfc(const float* mag, size_t nbins);                 // Masri HFC
float rolloffHz(const float* mag, size_t nbins, float sr, float frac = .85f);
float flatness(const float* mag, size_t nbins);            // 0..1 (1 = noise)
float crest(const float* mag, size_t nbins);               // peak/mean
struct SilenceGate { bool s20, s30, s60; void push(float frameRms); };
```

All O(bins); band energies are **unbounded** — normalize with
`curves.band_max` (offline) or `RollNorm` (live) before mapping to DMX.

### `onsets.h` — onset detection (tier 3)

```cpp
struct LogBank {                  // log-spaced triangular filterbank
    float sr = 44100, fmin = 30, fmax = 16000; int bandsPerOctave = 24;
    void prepare(size_t bins);    // ~139 filters after low-end dedup
    void apply(const float* mag, float* out) const;
    size_t bands() const;
};

struct SuperFluxOdf {             // Böck & Widmer DAFx-13 scheme
    LogBank bank; int lag = 2;    // hops of temporal distance
    void prepare(size_t bins);
    float push(const float* mag); // one ODF value per frame, >= 0
};

std::vector<size_t> pickOnsets(const std::vector<float>& odf, float fps,
    float delta = 1.5f,           // threshold in robust (MAD) units
    float preMaxS = .03f, float postMaxS = .03f,
    float preAvgS = .10f, float postAvgS = .07f,
    float combineS = .03f, float warmupS = .1f);
// OFFLINE. Returns frame indices; time = idx / fps.

struct CausalOnsetPicker {        // LIVE, median+k*IQR threshold
    float k = 2.0f; size_t history = 44; float minGapS = .08f; float fps;
    double push(float odfValue, double tNow);
    // >= 0: onset time (fires one hop late — fixed 11.6 ms + frame delay)
};
```

### `tempo.h` — tempo & beat phase (tier 4)

```cpp
struct TempoResult {
    float bpm;                    // folded to 70..180
    float confidence;             // 0..1 (< 0.5: don't trust silently)
    std::vector<std::pair<float,float>> candidates; // (bpm, weight 0..1)
    float stability;              // window agreement 0..1
};

void tempoWindow(const float* oss, size_t n, float ossFps,
                 std::vector<std::pair<float,float>>& outCandidates);
// one ~6 s window -> scored candidates (autocorr + harmonic enhancement
// + pulse-train comb scoring; Percival & Tzanetakis 2014 architecture)

float foldBpm(float bpm, float lo = 70, float hi = 180);   // octave fold
float refineBpm(const float* oss, size_t n, float ossFps, float bpmGuess,
                float rangeBpm = 2.5f, float stepBpm = .05f,
                float* outScore = nullptr);
// comb-score maximization around a guess (±0.05 BPM resolution)

class TempoEstimator {            // OFFLINE
    float ossFps = 86.1328125f;   // rate of your ODF/OSS
    float windowS = 6, hopS = 1.5;
    TempoResult analyze(const std::vector<float>& oss);
    // slides windows, accumulates a folded histogram, then ARBITRATES
    // among distinct candidates by full-signal comb support (this is
    // what resolves 4:5 / 3:4 aliases on syncopated material)
};

class LiveTempo {                 // LIVE
    float ossFps = 86.1328125f; float ringS = 12, updateEveryS = 2;
    void prepare();
    bool push(float ossValue, double tNow); // true when re-evaluated
    float bpm() const;            // 0 until locked (>= ~14 s)
    bool locked() const;
    float beatPhase(double tNow) const;  // 0..1 sawtooth inside the beat
};
```

The OSS input is simply the `SuperFluxOdf` output — one value per hop.
`LiveTempo` applies hysteresis (a new tempo must persist two updates) and
re-anchors the beat phase every update via exponentially-decayed comb
alignment over the last 4 s.

### `tonal.h` — peaks, chroma, key, tuning (tier 5)

```cpp
struct Peak { float hz, mag; };
void spectralPeaks(const float* mag, size_t nbins, float sr,
                   std::vector<Peak>& out, size_t maxPeaks = 60,
                   float minHz = 40, float maxHz = 5000,
                   float magThreshRel = 1e-4f);
// local maxima with LOG-magnitude parabolic interpolation (~1 cent)

struct Hpcp {                     // Gómez 2006-style harmonic PCP
    int size = 36; int harmonics = 4; float refHz = 440;
    float windowSemitones = 4.f/3.f; float decay = 0.6f;
    void compute(const std::vector<Peak>& peaks, float* out) const;
    // out[size], unit-max normalized, bin 0 = A
};

void foldChroma(const float* hpcp, int size, float* out12); // Nx -> 12 bins

struct KeyResult { std::string key, scale; float strength, firstToSecond; };
KeyResult estimateKey(const float* chroma12, const char* profile = "edma");
// profiles: "krumhansl" | "temperley" | "shaath" | "edma"
// key in {A, Bb, B, C, C#, D, Eb, E, F, F#, G, G#}, scale major|minor

const KeyProfile* keyProfiles(size_t& count);   // the published data tables

struct TuningEstimator {          // deviation from A440
    void push(const std::vector<Peak>& peaks);  // accumulate frames
    float cents() const;          // -50..50
    float hz() const;             // 440 * 2^(cents/1200)
};
```

Recipe for a track key: per 4096/2048 frame do `spectralPeaks` →
`Hpcp::compute` → accumulate; at the end average, `foldChroma`,
`estimateKey`. For a rolling live key, keep a 15 s ring of HPCP frames
(expect relative-key confusion — see the E17 measurements).

### `structure.h` — novelty, sections, danceability, fades (tier 6)

```cpp
std::vector<float> noveltyCurve(const std::vector<std::vector<float>>& bands,
                                float fps);      // 0..1, per frame
std::vector<float> sectionCandidates(const std::vector<float>& novelty,
    float fps, float minGapS = 8, float minHeight = .3f, int maxN = 8); // s

float danceability(const std::vector<float>& envelope /* ~86-100 Hz RMS */);
// DFA slope mapped to 0..3 (higher = more danceable)

struct Fades { float inEndS; float outStartS /* -1 = none */; };
Fades detectFades(const std::vector<float>& rmsPerFrame, float fps);
```

### `qc.h` — audio QC (tier 7)

```cpp
struct QcReport { float clipPct; int clipRuns; float humDb; float humHz; int gapCount; };
QcReport qcAnalyze(const float* x, size_t n, float sr,
                   const std::vector<float>& rmsPerFrame, float fps);
double goertzelPower(const float* x, size_t n, float hz, float sr);
```

### `loudness.h` — EBU R128 (tier 2, wraps vendored libebur128)

```cpp
struct LoudnessResult {
    float integratedLufs, rangeLu, truePeakDb, momentaryMaxLufs;
    std::vector<float> momentary10Hz;   // LUFS, 10 Hz
    float trackGainDb;                  // to -18 LUFS
};
LoudnessResult loudnessAnalyze(const float* interleaved, size_t frames,
                               int channels, int sr);
```

### `audiofile.h` — decoding (wraps vendored miniaudio)

```cpp
struct Audio {
    std::vector<float> stereo;    // interleaved, 44.1 kHz
    std::vector<float> mono;      // (L+R)/2
    int sr, channels;             // always 44100 / 2 after decode
    double durationS() const;
};
bool decodeFile(const std::string& path, Audio& out, std::string& err);
// mp3 / flac / wav (miniaudio built-ins); resamples to 44.1 kHz
```

### `analyze.h` — offline pipeline

```cpp
struct OfflineResult { /* every field of the analyze JSON, see CLI table:
    durationS; tempo (TempoResult); onsetsS; onsetRate;
    keyEdma, keyKrumhansl (KeyResult); tuningHz; chroma12;
    loud (LoudnessResult); danceability; fades (Fades);
    sectionsS; novelty; curveLow/Mid/High (0..1, ~10.77 Hz);
    curveLoud01 (10 Hz); bandMax[4]; qc (QcReport); analysisSeconds */ };

OfflineResult analyzeOffline(const Audio& a);              // ~205x realtime
std::string offlineJson(const OfflineResult& r, const std::string& file);
```

### `live.h` — live pipeline

```cpp
struct LiveWidgets {
    double t;                     // stream time, s
    float loud;                   // rolling-normalized RMS, 0..1
    float energyLow, energyMid, energyHigh;   // rolling 0..1
    float bright;                 // centroid / nyquist, 0..1
    bool  onset;                  // one-hop pulse
    float bpm;                    // 0 until locked
    float beatPhase;              // 0..1 while locked
    bool  silence;                // frame RMS < -60 dB
};

class LivePipeline {              // frame 2048 / hop 512 @ 44.1 kHz
    void push(const float* mono, size_t n, Fn&& cb);  // cb per hop
    size_t onsetCount() const;
};
```

### `jsonw.h` — minimal JSON writer

```cpp
class JsonW { beginObj/endObj, beginArr/endArr, key, str, num, boolean,
              kv(k,v), kvb, arr(k, vector<float>); std::string s; };
```

---

## Mapping to `music.*` widgets

(Names from `ESSENTIA-SCENARIOS.md` §E14; lightshow feeds them via
`doc.setControl` or OSC `/ctl/<name>`.)

| Widget | Live source | Offline source |
|---|---|---|
| `music.loud` | `LiveWidgets.loud` | `curves.loud01_10hz` sampled at transport |
| `music.energy.low/mid/high` | `LiveWidgets.energy*` | `curves.low/mid/high` |
| `music.bright` | `LiveWidgets.bright` | centroid curve (extend `OfflineResult` if needed) |
| `music.onset` | `LiveWidgets.onset` (stretch ≥2 engine ticks) | `rhythm.onsets_s` scheduled as cues |
| `music.bpm` | `LiveWidgets.bpm` | `rhythm.bpm` (+ gate on `bpm_confidence`) |
| `music.beat.phase` | `LiveWidgets.beatPhase` | from the BeatNet+ grid (lightshow) |
| `music.silence` | `LiveWidgets.silence` | `curves.loud01_10hz == 0` |
| `music.key.color` | (rolling key possible, unreliable) | `tonal.key_edma` → hue LUT |
| `music.drop`/`build` | — | edmformer sections (lightshow), refined by `structure.sections_s` |

---

## Realtime & threading notes

- **No internal threads, no locks.** Every object is single-threaded by
  design; run `LivePipeline` on your audio thread or a dedicated worker.
- **Hot-path cost**: measured **15.6 µs per 11.6 ms callback** (0.13 % of
  one core) for the full live chain; the periodic `LiveTempo` update adds
  ~1 ms every 2 s (run it off the audio thread if that matters).
- **Allocations**: small per-hop vector copies exist (ODF history, picker
  sort). At 86 Hz this is noise; for hard-realtime audio threads, feed a
  worker thread instead.
- **Determinism**: offline analysis is bit-stable for identical input;
  live output depends only on chunking (any chunk size gives identical
  frames thanks to `FrameFeed`).
- **Warm-up**: the first ~0.1 s of ODF/frames sees zero-padded windows;
  `pickOnsets` skips it (`warmupS`), `LiveTempo` needs its 12 s ring full
  before the first estimate (lock ≥ ~14 s).
- Validation workflow for changes: `showdsp-cli selftest` (synthetic
  pins), then `tests/oracle_dump.py` + `tests/validate.py` against the
  Essentia oracle corpus (tolerances in `AUDIO-CLEANROOM.md` §5).
