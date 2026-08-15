#!/usr/bin/env python3
"""Dump Essentia oracle values (dev-time only, never shipped) for the
validation corpus. Run with the essentia venv python."""
import sys, json, warnings
import numpy as np
warnings.filterwarnings("ignore")
import essentia
essentia.log.infoActive = False
import essentia.standard as es

SR = 44100

def analyze(path):
    audio = es.MonoLoader(filename=path, sampleRate=SR)()
    r = {"file": path.split("/")[-1], "duration_s": round(len(audio) / SR, 2)}
    bpm, ticks, conf, _, _ = es.RhythmExtractor2013(method="multifeature")(audio)
    r["bpm"] = float(bpm); r["bpm_confidence"] = float(conf)
    k, s, st = es.KeyExtractor(profileType="edma")(audio)
    r["key_edma"] = f"{k} {s}"; r["key_edma_strength"] = float(st)
    k, s, st = es.KeyExtractor(profileType="krumhansl")(audio)
    r["key_krumhansl"] = f"{k} {s}"; r["key_krumhansl_strength"] = float(st)
    dnc, _ = es.Danceability()(audio)
    r["danceability"] = float(dnc)
    stereo = es.AudioLoader(filename=path)()[0]
    mom, shortterm, integrated, lrange = es.LoudnessEBUR128()(stereo)
    r["integrated_lufs"] = float(integrated); r["range_lu"] = float(lrange)
    # per-channel true peak on the stereo signal (matches BS.1770 practice)
    tpd = es.TruePeakDetector()
    tp = 0.0
    for ch in range(stereo.shape[1] if stereo.ndim > 1 else 1):
        sig = stereo[:, ch].copy() if stereo.ndim > 1 else stereo
        tp = max(tp, float(np.max(np.abs(tpd(sig)[1]))))
    r["true_peak_db"] = float(20 * np.log10(tp + 1e-12))
    r["onsets_s"] = [float(t) for t in es.SuperFluxExtractor(sampleRate=SR)(audio)]
    return r

if __name__ == "__main__":
    out = [analyze(p) for p in sys.argv[1:]]
    json.dump(out, open("/tmp/kilo/oracle.json", "w"), indent=1)
    print(f"oracle: {len(out)} files -> /tmp/kilo/oracle.json")
