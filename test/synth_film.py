#!/usr/bin/env python3
"""Synthetic 300 dpi AFL strips for all four film types, run `v350scan --analyze --film T`,
and check that every true inter-frame gap gets exactly one cut and no cut lands in a picture.

    python3 test/synth_film.py [path/to/v350scan_or_mock]   (numpy needed)
"""
import json, os, struct, subprocess, sys, tempfile
import numpy as np

DPI = 300
W = 450                     # 1.5" wide AFL window at 300 dpi
CBASE = np.array([0.78, 0.46, 0.21])   # colour-negative orange mask (raw transmittance)
BBASE = 0.80                           # B&W negative clear base + fog

def write_tiff(path, img16):
    h, w, _ = img16.shape
    data = img16.astype("<u2").tobytes()
    tags = [(256, 4, 1, w), (257, 4, 1, h), (258, 3, 3, None), (259, 3, 1, 1), (262, 3, 1, 2),
            (273, 4, 1, None), (277, 3, 1, 3), (278, 4, 1, h), (279, 4, 1, len(data)), (284, 3, 1, 1)]
    ifd_off = 8; n = len(tags); bps_off = ifd_off + 2 + n * 12 + 4; data_off = bps_off + 6
    out = bytearray(b"II*\0" + struct.pack("<I", ifd_off) + struct.pack("<H", n))
    for t, ty, cnt, v in tags:
        if t == 258: v = bps_off
        if t == 273: v = data_off
        out += struct.pack("<HHI", t, ty, cnt) + (struct.pack("<HH", v, 0) if ty == 3 and cnt == 1 else struct.pack("<I", v))
    out += struct.pack("<I", 0) + struct.pack("<HHH", 16, 16, 16)
    open(path, "wb").write(bytes(out) + data)

def scene(rng, h, w, kind):
    """one frame's scene brightness 0..1 with texture; kind: normal / dense / thin / dark / bright-sky"""
    y, x = np.mgrid[0:h, 0:w] / 40.0
    t = 0.5 + 0.25 * np.sin(x * rng.uniform(.5, 2) + y * rng.uniform(.3, 1.5)) * np.cos(y * rng.uniform(.5, 2))
    t += 0.12 * rng.standard_normal((h, w)).cumsum(1) / np.sqrt(np.arange(1, w + 1))
    t = np.clip(t, 0, 1)
    if kind == "dark":  t = t * 0.25                       # night scene (thin neg / dark slide)
    if kind == "sky":   t[: h // 2] = 0.92                 # flat bright sky in the top half
    if kind == "blackbox": t[h // 3: 2 * h // 3, w // 4: 3 * w // 4] = 0.0   # large black area
    return t

def make_strip(rng, film):
    """returns raw RGB (0..1) image and list of true gaps (a, b) in inches, plus picture ranges"""
    H = int(9.3 * DPI)
    S = np.full((H, W), np.nan)                            # scene value; nan = no film; -1 = base/unexposed
    y0 = rng.uniform(0.08, 0.2); end = rng.uniform(8.6, 9.0)
    leader = rng.uniform(0.3, 1.6)
    S[int(y0 * DPI): int(end * DPI)] = -1
    gaps, pics = [], []
    y = y0 + leader
    kinds = ["normal", "dense", "thin", "dark", "sky", "blackbox", "normal"]
    rng.shuffle(kinds)
    k = 0
    while True:
        fh = 1.42 + rng.uniform(-0.01, 0.01)
        if y + fh > end - 0.05: break
        a, b = int(y * DPI), int((y + fh) * DPI)
        kind = kinds[k % len(kinds)]; k += 1
        t = scene(rng, b - a, W - 40, kind)
        if kind == "dense": t = 0.5 + 0.5 * t
        if kind == "thin":  t = 0.25 * t
        S[a:b, 20:W - 20] = t
        pics.append((y, y + fh))
        g = rng.uniform(0.03, 0.10)
        gaps.append((y + fh, y + fh + g))
        y += fh + g
    gaps = [g for g in gaps if g[1] < end - 0.05][: len(pics) - 1]
    film_on = ~np.isnan(S)
    base_or_scene = np.where(np.isnan(S), 0, S)
    unexp = base_or_scene < 0
    v = np.clip(base_or_scene, 0, 1)
    img = np.zeros((H, W, 3))
    if film == "cpos":
        col = np.array([1.0, 0.85, 0.7]); img = v[..., None] * col          # unexposed slide = black
    elif film == "bpos":
        img = np.repeat(v[..., None] * 0.9, 3, 2)
    elif film == "cneg":
        d = 2.0 * np.where(unexp, 0, v)                                       # density above base
        img = CBASE[None, None, :] * 10 ** (-d[..., None] * np.array([0.95, 1.0, 0.9]))
    elif film == "bneg":
        d = 2.2 * np.where(unexp, 0, v)
        img = np.repeat((BBASE * 10 ** -d)[..., None], 3, 2)
    img[~film_on] = 1.0                                                       # lamp, no film
    # lamp fall-off across the strip, noise, dust in gaps
    img *= (0.95 + 0.05 * np.cos(np.linspace(-1.2, 1.2, W)))[None, :, None]
    img += rng.normal(0, 0.004, img.shape)
    for _ in range(25):
        yy, xx = rng.integers(0, H), rng.integers(0, W)
        img[yy:yy + 2, xx:xx + 2] *= 0.3
    return np.clip(img, 0, 1), gaps, pics

def check(binary, film, seed):
    rng = np.random.default_rng(seed)
    img, gaps, pics = make_strip(rng, film)
    d = tempfile.mkdtemp()
    p = os.path.join(d, "s.tif"); write_tiff(p, (img * 65535 + 0.5).astype(np.uint16))
    r = subprocess.run([binary, "--analyze", p, "--film", film], capture_output=True, text=True)
    if r.returncode: return False, "rc %d %s" % (r.returncode, r.stderr)
    j = json.load(open(os.path.join(d, "s.json")))
    cuts = j["cuts"]; tol = 0.012
    hit = [sum(1 for c in cuts if a - tol <= c <= b + tol) for a, b in gaps]
    bad = [c for c in cuts if any(a + 0.02 < c < b - 0.02 for a, b in pics)]
    ok = all(h == 1 for h in hit) and not bad
    return ok, "gaps %d found %d/%d, cuts in pictures %s" % (len(gaps), sum(1 for h in hit if h == 1), len(gaps), bad or "none")

if __name__ == "__main__":
    binary = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "v350scan_mock")
    allok = True
    for film in ("cpos", "bpos", "cneg", "bneg"):
        res = [check(binary, film, s) for s in range(20)]
        n = sum(ok for ok, _ in res); allok &= n == len(res)
        print("%s: %d/%d strips OK" % (film, n, len(res)))
        for ok, m in res:
            if not ok: print("   ", m)
    sys.exit(0 if allok else 1)
