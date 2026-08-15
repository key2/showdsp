# showdsp vs Essentia — measured comparison

Head-to-head on the same machine, same 5-track EDM corpus (Calvin Harris
*Outside*, Guetta *I'm Good* — a live-audience recording, Guetta *Sexy
Chick*, DJ Asul *MATADORA* — dembow/reggaeton syncopation, Garrix
*Animals*). Essentia 2.1-beta6-dev (Python wheel) vs showdsp @ `71534a6`.
Method at the bottom; every number reproducible.

**Scope disclaimer**: Essentia is a research library with ~250 algorithms
plus a pre-trained model zoo; showdsp implements the ~26 functions a
light-show controller needs. This page compares only that overlap — on
everything else (beat *positions*, chords, melody, ML tagging,
fingerprinting…) Essentia stands alone by design (see the feature matrix).

---

## 1. Offline accuracy (showdsp validated against Essentia as oracle)

### Tempo

| Track | Essentia BPM (conf 0–5.32) | showdsp BPM (conf 0–1) | Δ folded |
|---|---|---|---|
| Outside | 128.0 (2.39) | 128.0 (0.80) | **0.02** |
| I'm Good (live rec.) | 128.0 (2.30) | 128.0 (0.70) | **0.01** |
| Sexy Chick | 129.7 (2.32) | 130.0 (0.85) | **0.32** |
| MATADORA (dembow) | 129.9 (2.15) | 130.0 (0.47) | **0.11** |
| Animals | 128.1 (2.22) | 128.0 (0.77) | **0.12** |

5/5 within ±0.32 BPM. Convergent uncertainty signal: **both** engines
rank MATADORA least confident (2.15 is Essentia's lowest; 0.47 ours) —
two independent implementations agreeing on *what is hard* is itself a
validation.

### Key (both profiles)

| Track | Essentia edma (strength) | showdsp edma (strength) | Krumhansl agreement |
|---|---|---|---|
| Outside | D minor (0.87) | D minor (0.87) | ✓ |
| I'm Good | G minor (0.87) | G minor (0.87) | ✓ |
| Sexy Chick | D major (0.91) | D major (0.77) | ✓ |
| MATADORA | F minor (0.84) | F minor (0.80) | ✓ |
| Animals | F minor (0.97) | F minor (0.89) | ✓ |

**10/10 identical key decisions** (5 tracks × 2 profile families);
strengths track closely despite fully independent HPCP implementations.

### Loudness, dynamics, onsets

| Track | LUFS ess / ours | True peak ess / ours (dBTP) | Danceability ess / ours | Onsets ess / ours |
|---|---|---|---|---|
| Outside | −8.94 / −8.88 | 0.49 / 0.48 | 1.02 / 1.44 | 332 / 591 |
| I'm Good | −7.96 / −7.81 | 0.59 / 0.43 | 1.03 / 1.38 | 304 / 453 |
| Sexy Chick | −17.09 / −16.93 | −6.84 / −6.84 | 1.34 / 2.46 | **27** / 581 |
| MATADORA | −4.67 / −4.58 | 1.60 / 1.62 | 1.35 / 2.17 | 550 / 291 |
| Animals | −8.33 / −8.13 | 0.82 / 0.84 | 0.97 / 1.17 | 739 / 715 |

- **LUFS agree ≤ 0.19 LU**, **true peak ≤ 0.16 dB** (different R128
  implementations: their C++ vs vendored libebur128).
- **Danceability**: scales differ (~1.6×) but **rank correlation 0.80** —
  both order the corpus the same way; use it relatively, not absolutely.
- **Onsets**: counts are threshold philosophy, not correctness — note
  Essentia's SuperFluxExtractor fires only **27 times in 3.5 minutes** on
  Sexy Chick (near-silent) while showdsp behaves normally; F-measure
  0.50–0.72 (±50 ms) on tracks where both fire.

---

## 2. Speed & footprint

### Offline analysis (full feature pass per track)

| Metric | Essentia (python) | showdsp | Factor |
|---|---|---|---|
| Animals (191.6 s track), in-process | 2.6 s (74×) | 1.1 s incl. decode (174×) | **~2.4×** |
| Corpus range | 56–74× realtime | 132–210× realtime (load-dependent) | ~2–3× |
| Cold start (relevant for spawned CLIs) | +0.50 s (`import essentia`) + Python boot | ~0 (native binary) | — |
| End-to-end as a subprocess (Animals) | ≈ 3.1 s | ≈ 1.1 s | **~2.8×** |

Caveat: feature sets differ slightly — Essentia's pass includes full beat
*positions* (RhythmExtractor2013, its priciest stage); showdsp's includes
QC/fades/tuning instead. Treat the factor as "the whole job each side
does for a light show", not algorithm-for-algorithm.

### Live pipeline (512-sample chunks, 11.6 ms budget)

| Metric | Essentia (streaming, python chain) | showdsp `LivePipeline` |
|---|---|---|
| CPU per callback | 51–53 µs | **15.5–15.8 µs** (~3.3×) |
| Realtime factor | ~84× | ~745× |
| BPM lock time | 16 s | 14–22 s |
| Live BPM correct (±2, folded) | 4/5 — **fails Sexy Chick** (98.4 vs 129.7, ¾ alias, never recovers) | 4/5 — **fails MATADORA** (103.95, 4:5 alias; offline arbitration fixes it) |
| Live rolling key | 0–57 % agreement with offline truth | not shipped (measured unreliable — deliberate omission) |
| Onset latency | ~35 ms | ~35 ms (same frame math) |

The live symmetry is instructive: each engine trips on a *different*
syncopated track. Single-window tempo estimation has an irreducible
alias risk — which is why the offline path (candidate arbitration +
confidence) should be the authority whenever the file is known
(`ESSENTIA-SCENARIOS.md` §E17 conclusion).

### Deployment footprint

| | Essentia | showdsp |
|---|---|---|
| Payload | 38 MB python module (+numpy → **100 MB venv**), needs CPython | **0.92 MB** binary |
| Shared-lib deps | Python + wheel-bundled ffmpeg/fftw/etc. | 6 (libc, libm, libstdc++, libgcc, pthread, ld) |
| Source to audit | ~250 algorithms, large C++ tree | ~1.9 k LOC + 2 vendored MIT files |
| License | **AGPLv3** (models CC BY-NC-SA) | **MIT** (deps MIT-0/MIT) |
| Windows cross-compile | waf build, nontrivial | plain CMake, MinGW-friendly |

---

## 3. Feature matrix (honesty section)

| Capability | Essentia | showdsp |
|---|---|---|
| BPM + confidence | ✓ (RhythmExtractor2013, TempoCNN) | ✓ (arbitrated, 0–1 confidence) |
| **Beat positions (grid)** | **✓ ticks + confidence** | ✗ (lightshow gets them from BeatNet+) |
| Onsets | ✓ | ✓ (+ causal live variant) |
| Key / chroma / tuning | ✓ (14 profiles) | ✓ (4 profiles: krumhansl, temperley, shaath, edma) |
| EBU R128 / true peak | ✓ | ✓ (libebur128) |
| Danceability (DFA) | ✓ | ✓ (rank-compatible) |
| Novelty / segmentation | ✓ (several) | ✓ (one, tuned for section cues) |
| Audio QC (clip/hum/gaps) | ✓ (separate algos) | ✓ (single pass) |
| Live pipeline w/ beat phase + hysteresis | ✗ (assemble yourself) | ✓ built-in |
| Chords, melody, pitch (CREPE), source separation | ✓ | ✗ |
| ML tagging (genre/mood/arousal…) | ✓ (model zoo, NC-licensed) | ✗ (by design — weights can't be clean-roomed) |
| Fingerprinting (Chromaprint) | ✓ | ✗ |
| Streaming graph framework, Vamp, bindings | ✓ | ✗ (12 headers, bring your own glue) |
| Battle-testing | 15+ years, AcousticBrainz-scale | one corpus + 19 synthetic pins |

**Use Essentia when**: you need beat grids, ML semantics, breadth, or
research-grade cross-validation — and AGPL/subprocess isolation is fine.
**Use showdsp when**: you need the light-show subset, MIT licensing,
a 1 MB dependency-free binary, ~3× the speed, or an integrated live path.

---

## 4. Methodology / reproduce

```bash
# oracle (essentia venv, python 3.12) — dev-time only
tests/oracle_dump.py <mp3s>            # -> /tmp/kilo/oracle.json
# showdsp
cmake -S . -B build -G Ninja && cmake --build build
./build/showdsp-cli selftest           # 19 synthetic checks
tests/validate.py ./build/showdsp-cli /tmp/kilo/oracle.json <mp3s>
./build/showdsp-cli analyze <mp3>      # offline JSON
./build/showdsp-cli live <mp3>         # live sim (cpu, lock, trace)
```

Notes: same machine, single core, Release/-O2; Essentia in-process
timings exclude Python/library import (measured separately at 0.50 s);
showdsp wall times include process start and decode. Live comparison
uses the E17 prototype (`ESSENTIA-SCENARIOS.md` §E17) for Essentia's
streaming numbers — python-orchestrated, so its 52 µs has interpreter
overhead; a pure-C++ Essentia chain would land somewhere in between.
Corpus is small (n=5) and EDM-biased; the tolerances and conclusions
follow `AUDIO-CLEANROOM.md` §5, which prescribes growing the corpus
(~50 tracks + GiantSteps key/tempo sets) before trusting deltas finer
than these.
