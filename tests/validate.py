#!/usr/bin/env python3
"""Compare showdsp-cli output against the Essentia oracle
(AUDIO-CLEANROOM.md §5 tolerances). Usage:
    validate.py <showdsp-cli> <oracle.json> <files...>"""
import sys, json, subprocess

def fold(b):
    if b <= 0: return 0
    while b < 70: b *= 2
    while b >= 180: b /= 2
    return b

def f_measure(ours, theirs, tol=0.05):
    if not ours or not theirs: return 0.0
    used = [False] * len(theirs)
    tp = 0
    for t in ours:
        best, bi = 1e9, -1
        for i, u in enumerate(theirs):
            if not used[i] and abs(u - t) < best:
                best, bi = abs(u - t), i
        if bi >= 0 and best <= tol:
            used[bi] = True; tp += 1
    p = tp / len(ours); r = tp / len(theirs)
    return 2 * p * r / (p + r) if p + r > 0 else 0.0

def main():
    cli, oracle_path, files = sys.argv[1], sys.argv[2], sys.argv[3:]
    oracle = {o["file"]: o for o in json.load(open(oracle_path))}
    rows, fails = [], []
    dance_ours, dance_oracle = [], []
    for f in files:
        out = subprocess.run([cli, "analyze", f], capture_output=True, text=True)
        r = json.loads(out.stdout)
        o = oracle[r["file"]]
        bpm_d = abs(fold(r["rhythm"]["bpm"]) - fold(o["bpm"]))
        key_e = r["tonal"]["key_edma"] == o["key_edma"]
        key_k = r["tonal"]["key_krumhansl"] == o["key_krumhansl"]
        lufs_d = abs(r["loudness"]["integrated_lufs"] - o["integrated_lufs"])
        tp_d = abs(r["loudness"]["true_peak_db"] - o["true_peak_db"])
        fm = f_measure(r["rhythm"]["onsets_s"], o["onsets_s"])
        dance_ours.append(r["structure"]["danceability"])
        dance_oracle.append(o["danceability"])
        name = r["file"][:38]
        rows.append(f"{name:38} bpm±{bpm_d:4.1f} keyE={'Y' if key_e else 'n'} keyK={'Y' if key_k else 'n'} "
                    f"LUFS±{lufs_d:4.2f} TP±{tp_d:4.2f} onsetF={fm:.2f} x{r['x_realtime']:.0f}rt")
        if bpm_d > 2: fails.append(f"{name}: bpm off {bpm_d:.1f}")
        if lufs_d > 0.5: fails.append(f"{name}: LUFS off {lufs_d:.2f}")
        if tp_d > 0.5: fails.append(f"{name}: true peak off {tp_d:.2f}")
    # danceability rank correlation (Spearman, n small)
    def ranks(v):
        s = sorted(range(len(v)), key=lambda i: v[i])
        rk = [0] * len(v)
        for pos, i in enumerate(s): rk[i] = pos
        return rk
    ro, rr = ranks(dance_ours), ranks(dance_oracle)
    n = len(ro)
    rho = 1 - 6 * sum((a - b) ** 2 for a, b in zip(ro, rr)) / (n * (n * n - 1)) if n > 2 else 1.0
    print("\n".join(rows))
    print(f"danceability rank corr: {rho:.2f}  (ours={['%.2f'%d for d in dance_ours]} oracle={['%.2f'%d for d in dance_oracle]})")
    if rho < 0.6: fails.append(f"danceability rank corr {rho:.2f}")
    if fails:
        print("FAILURES:"); [print("  " + f) for f in fails]
        return 1
    print("ALL TOLERANCES MET")
    return 0

if __name__ == "__main__":
    sys.exit(main())
