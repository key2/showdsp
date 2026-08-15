# showdsp

Clean-room, MIT-licensed audio analysis for light-show software — the
implementation of `AUDIO-CLEANROOM.md` (in the sibling essentia checkout).
Extracts the `music.*` widget signals and timeline lanes needed by Light
Show Studio without linking any copyleft code and without any AI models.

## Provenance statement

Implemented exclusively from published papers, public standards, and
research data tables (each cited at the top of the source file that uses
it): SuperFlux onsets (Böck & Widmer, DAFx-13), streamlined tempo
architecture (Percival & Tzanetakis, IEEE/ACM TASLP 2014), HPCP
(Fujishima 1999; Gómez 2006), key profiles (Krumhansl 1990; Temperley
1999; Shaath 2011; Faraldo et al. ECIR 2016 — published corpus-derived
data), DFA (Peng 1994; Streich & Herrera AES 2005), novelty (Foote 2000;
Grosche & Müller 2009), loudness (ITU-R BS.1770 via libebur128, MIT).
No AGPL/GPL source was read or translated. Essentia is used only as a
**black-box numeric oracle** at development time (`tests/validate.py`);
it is never linked or shipped. All tuning constants not fixed by a paper
are our own, chosen against the oracle corpus.

**Full API documentation: [API.md](API.md)** — CLI JSON schemas, every
module/class/function with units and ranges, `music.*` widget mapping,
realtime notes.

## Build & run

```bash
cmake -S . -B build -G Ninja && cmake --build build
./build/showdsp-cli selftest              # synthetic-signal checks
./build/showdsp-cli analyze song.mp3      # offline: full-file JSON
./build/showdsp-cli live song.mp3         # live sim: 512-sample chunks
```

## What it outputs

Offline (`analyze`): BPM + confidence + candidates, onset times, key
(edma/krumhansl profiles) + chroma + tuning, EBU R128 loudness
(integrated/momentary/LRA/true peak) + track gain, novelty + section
candidates, danceability (DFA), fades, band-energy curves with
normalization constants, QC (clipping/hum/gaps). Live (`live`):
rolling-normalized loud/energy/bright, causal onsets (35 ms latency),
rolling BPM with lock + beat phase — the same `music.*` widget semantics
measured in `ESSENTIA-SCENARIOS.md` §E17.

## Validation

`tests/validate.py` runs the Essentia oracle (from its own venv) and this
CLI on the same files and enforces the tolerance table from
`AUDIO-CLEANROOM.md` §5.

## Status — measured against the oracle (5-track EDM corpus, 2026-08-15)

`selftest`: 19/19 synthetic checks. `tests/validate.py` vs Essentia
2.1-beta6, all tolerances met:

| Metric | Result |
|---|---|
| BPM (offline, arbitrated candidates) | 5/5 within ±0.3 of oracle — incl. the dembow-syncopated track the oracle's own live mode gets wrong |
| Key (edma + krumhansl profiles) | 10/10 agreement |
| Integrated LUFS | ≤ 0.19 LU difference |
| True peak | ≤ 0.16 dB difference |
| Danceability | rank correlation 0.80 |
| Onset F-measure (±50 ms) | 0.50–0.72 (detector thresholds differ by design) |
| Offline speed | ~205× realtime (~1 s per 3.5-min track) |
| Live cost | **15.6 µs per 11.6 ms callback** (~745× realtime), BPM lock 14–22 s |

Known limitation: live single-window tempo can pick a small-integer-ratio
alias on heavily syncopated material (offline arbitration resolves it;
live reports it consistently rather than flapping).

## Third-party (all permissive)

| Component | License | Use |
|---|---|---|
| miniaudio | MIT-0 / public domain | decode mp3/flac/wav, resample |
| libebur128 | MIT | EBU R128 loudness, true peak |

Everything else (FFT included) is implemented in this repository.
