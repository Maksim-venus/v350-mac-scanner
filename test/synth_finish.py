#!/usr/bin/env python3
"""「输出成片」 test: turn a known positive into a simulated colour / B&W negative (orange mask,
per-channel H&D density curve, optional dye crosstalk, noise), run the real pipeline
(`--analyze` on an 8-bit 300 dpi preview -> finish parameters, `--develop` on a 16-bit scan) and
compare the recovered positive with the truth. Also checks that grey patches come out neutral.

    python3 test/synth_finish.py MOCK_OR_CLI [positive.tif] [outdir]     (numpy only)
"""
import json, os, struct, subprocess, sys, tempfile, zlib
import numpy as np

def tiff_read(path):
    """uncompressed single-strip little-endian TIFF (ours, or written by tiff_write)"""
    b = open(path, "rb").read()
    assert b[:2] == b"II"
    ifd = struct.unpack_from("<I", b, 4)[0]; n = struct.unpack_from("<H", b, ifd)[0]; t = {}
    for i in range(n):
        tg, ty, cnt, v = struct.unpack_from("<HHII", b, ifd + 2 + 12 * i)
        if ty == 3 and cnt == 1: v &= 0xFFFF
        t[tg] = (ty, cnt, v)
    w, h, spp = t[256][2], t[257][2], t.get(277, (3, 1, 1))[2]
    bps = t[258][2] if t[258][1] == 1 else struct.unpack_from("<H", b, t[258][2])[0]
    off = t[273][2]
    dt = np.uint8 if bps == 8 else np.dtype("<u2")
    a = np.frombuffer(b, dt, w * h * spp, off).reshape(h, w, spp) if spp > 1 else np.frombuffer(b, dt, w * h, off).reshape(h, w)
    return a

def tiff_write(path, a):
    h, w = a.shape[:2]; spp = a.shape[2] if a.ndim == 3 else 1
    bps = 8 if a.dtype == np.uint8 else 16
    data = a.astype(np.uint8 if bps == 8 else "<u2").tobytes()
    tags = [(256, 4, 1, w), (257, 4, 1, h), (258, 3, spp, None), (259, 3, 1, 1), (262, 3, 1, 2 if spp == 3 else 1),
            (273, 4, 1, None), (277, 3, 1, spp), (278, 4, 1, h), (279, 4, 1, len(data)), (284, 3, 1, 1)]
    n = len(tags); bps_off = 8 + 2 + n * 12 + 4; data_off = bps_off + 8
    out = bytearray(b"II*\0" + struct.pack("<I", 8) + struct.pack("<H", n))
    for tg, ty, cnt, v in tags:
        if tg == 258: v = bps_off if spp > 1 else bps
        if tg == 273: v = data_off
        out += struct.pack("<HHI", tg, ty, cnt) + (struct.pack("<HH", v, 0) if ty == 3 and cnt == 1 else struct.pack("<I", v))
    out += struct.pack("<I", 0) + struct.pack("<HHHH", bps, bps, bps, 0)
    open(path, "wb").write(bytes(out) + data)

def png_write(path, rgb8):
    h, w, _ = rgb8.shape
    raw = b"".join(b"\0" + rgb8[y].tobytes() for y in range(h))
    ch = lambda t, d: struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" + ch(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                           + ch(b"IDAT", zlib.compress(raw, 6)) + ch(b"IEND", b""))

cli = sys.argv[1]
src = sys.argv[2] if len(sys.argv) > 2 and sys.argv[2] != "-" else None
outdir = sys.argv[3] if len(sys.argv) > 3 else tempfile.mkdtemp()
os.makedirs(outdir, exist_ok=True)
rng = np.random.default_rng(7)
CBASE = np.array([0.78, 0.46, 0.21])        # orange mask, raw transmittance
GAM = np.array([0.62, 0.60, 0.56])          # per-channel negative contrast (layers differ)
BBASE, BGAM = 0.80, 0.65

def truth_scene():
    """linear scene light (H, W, 3) and a mask of picture pixels"""
    if src:
        a = tiff_read(src).astype(float)
        E = (a + rng.uniform(0, 1, a.shape)) / 256.0
        lamp = a[..., 1] >= 250
        return E, lamp
    # synthetic: grey photo-like texture + colour patches of equal area (neutral on average)
    H, W = 2790, 432
    y, x = np.mgrid[0:H, 0:W] / 30.0
    L = 0.5 + 0.3 * np.sin(x * 0.7 + y * 0.31) * np.cos(y * 0.9) + 0.1 * np.sin(3.1 * x - 2.2 * y)
    E = np.repeat(np.clip(L, 0.02, 1)[..., None] ** 2.2, 3, 2)
    lamp = np.zeros((H, W), bool); lamp[:40] = True; lamp[-60:] = True
    E[40:200] = 1e-4                                    # unexposed leader
    for y0 in range(200 + 426, H - 60, 426 + 20): E[y0:y0 + 20] = 1e-4   # 1.42" frames, black gaps
    E[:, :14] = E[:, -14:] = 1e-4                       # film rebate beside the frames
    return E, lamp

def add_targets(E, frames):
    """grey wedge + colour patches in the second frame; returns patch boxes"""
    a, b = frames[1]
    boxes = []
    y = a + 40
    for i, g in enumerate([0.05, 0.12, 0.25, 0.45, 0.7, 0.9]):          # display grey levels
        x0 = 60 + i * 52
        E[y:y + 60, x0:x0 + 44] = g ** 2.2; boxes.append(("grey%.2f" % g, y, x0))
    cols = [(1, .15, .15), (.15, 1, .15), (.15, .15, 1), (.15, 1, 1), (1, .15, 1), (1, 1, .15)]
    for i, c in enumerate(cols):
        x0 = 60 + i * 52
        E[y + 90:y + 150, x0:x0 + 44] = (np.array(c) * 0.6) ** 2.2
    return boxes

def frames_and_gaps(E, lamp):
    """positive strip: gaps are dark rows; returns picture row ranges"""
    g = np.percentile(E[:, 86:346, 1], 90, axis=1)
    film = ~lamp[:, 216]
    pic = (g > 0.02) & film
    fr, y = [], 0
    while y < len(pic):
        if pic[y]:
            j = y
            while j < len(pic) and pic[j]: j += 1
            if j - y > 200: fr.append((y, j))
            y = j
        else: y += 1
    return fr

def to_negative(E, lamp, bw, crosstalk):
    logE = np.log10(np.clip(E, 1e-4, 1))
    rel = logE + 2.4                                    # scene range of the film ~2.4 decades
    soft = lambda z: np.log1p(np.exp(z * 6)) / 6        # toe
    if bw:
        Ey = (E * [0.3, 0.59, 0.11]).sum(2)
        d = BGAM * soft(np.log10(np.clip(Ey, 1e-4, 1)) + 2.4)
        raw = np.repeat((BBASE * 10 ** -d)[..., None], 3, 2)
    else:
        d = GAM * soft(rel)
        if crosstalk:                                    # unwanted dye absorptions
            M = np.array([[1, .08, .03], [.10, 1, .08], [.04, .12, 1]])
            d = d @ M.T
        raw = CBASE * 10 ** -d
    raw = raw * (1 + 0.004 * rng.standard_normal(raw.shape)) + 0.0008 * rng.standard_normal(raw.shape)
    raw[lamp] = 1.0
    return np.clip(raw, 0, 1)

def write_tiff(path, img, bits):
    if bits == 8: tiff_write(path, (img * 255 + .5).astype(np.uint8))
    else: tiff_write(path, (img * 65535 + .5).astype(np.uint16))

def disp_save(img, path): png_write(path, (np.clip(img, 0, 1) * 255 + .5).astype(np.uint8))

def autolevel(x, mask):
    o = np.empty_like(x)
    for c in range(x.shape[2]):
        lo, hi = np.percentile(x[..., c][mask], [0.1, 99.5]); o[..., c] = np.clip((x[..., c] - lo) / (hi - lo), 0, 1)
    return o

def run(name, bw, crosstalk):
    E, lamp = truth_scene()
    E = E.copy()
    fr = frames_and_gaps(E, lamp)
    boxes = add_targets(E, fr) if len(fr) > 1 else []
    # between the pictures a negative is unexposed (clear base), whatever the slide mount showed
    film_rows = ~lamp[:, E.shape[1] // 2]
    inpic = np.zeros(len(film_rows), bool)
    for a, b in fr: inpic[a:b] = True
    E[film_rows & ~inpic] = 1e-5
    raw = to_negative(E, lamp, bw, crosstalk)
    film = "bneg" if bw else "cneg"
    p8, r16 = f"{outdir}/{name}_preview.tif", f"{outdir}/{name}_raw16.tif"
    write_tiff(p8, raw, 8); write_tiff(r16, raw, 16)
    subprocess.run([cli, "--analyze", p8, "--film", film], check=True, capture_output=True)
    js = json.load(open(p8[:-4] + ".json"))
    fin = js["finish"]
    out = f"{outdir}/{name}_finished.tif"
    subprocess.run([cli, "--develop", r16, out, "--finish-from", p8[:-4] + ".json"], check=True, capture_output=True)
    got = tiff_read(out).astype(float) / 65535
    if got.ndim == 2: got = got[..., None]
    pic = np.zeros(lamp.shape, bool)
    for a, b in fr: pic[a + 15:b - 15, 30:-30] = True
    truth = E if not bw else (E * [0.3, 0.59, 0.11]).sum(2, keepdims=True)
    tdisp = autolevel(truth ** (1 / 2.2), pic)
    corr = [np.corrcoef(got[..., c][pic], tdisp[..., c][pic])[0, 1] for c in range(got.shape[2])]
    # tone curve fidelity: correlation of log-ish values
    res = {"name": name, "finish": fin, "corr": [round(float(c), 4) for c in corr]}
    greys = []
    for lab, y, x0 in boxes:
        v = got[y + 10:y + 50, x0 + 8:x0 + 36].reshape(-1, got.shape[2]).mean(0)
        greys.append((lab, [round(float(t) * 255, 1) for t in v]))
    res["grey_patches_8bit"] = greys
    if not bw and greys:
        res["max_grey_chroma_8bit"] = round(max(max(v) - min(v) for _, v in greys), 1)
    # images: raw negative, plain inversion (old display), finished, truth
    simple = 1 - raw / raw[pic].max(0) if not bw else 1 - raw / raw[pic].max()
    g3 = np.repeat(got, 3, 2) if got.shape[2] == 1 else got
    t3 = np.repeat(tdisp, 3, 2) if tdisp.shape[2] == 1 else tdisp
    W = raw.shape[1]
    sheet = np.concatenate([raw, np.ones((raw.shape[0], 8, 3)), simple, np.ones((raw.shape[0], 8, 3)), g3,
                            np.ones((raw.shape[0], 8, 3)), t3], 1)
    disp_save(sheet, f"{outdir}/{name}_compare.png")
    return res

results = [run("cneg", False, False), run("cneg_crosstalk", False, True), run("bneg", True, False)]
json.dump(results, open(f"{outdir}/results.json", "w"), indent=1)
ok = True
for r in results:
    print(r["name"], "corr", [float(c) for c in r["corr"]], "grey chroma", r.get("max_grey_chroma_8bit"))
    for lab, v in r["grey_patches_8bit"]: print("   ", lab, v)
    if min(r["corr"]) < 0.9: ok = False
    if r["name"] == "cneg" and not src and r.get("max_grey_chroma_8bit", 0) > 8: ok = False
print("compare sheets (raw | plain inversion | finished | truth) in", outdir)
print("PASS" if ok else "FAIL"); sys.exit(0 if ok else 1)
