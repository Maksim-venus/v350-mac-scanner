import sys, glob, os, tifffile, numpy as np
d = sys.argv[1]; dpi = int(sys.argv[2]); ok = True
def phase(y):
    if y < 0.1: return 'no-film'
    if y < 1.6: return 'leader'
    if y > 8.9: return 'no-film'
    ph = (y - 1.6) % 1.496
    return 'GAP' if ph < 0.06 else 'content(%.3f into frame)' % (ph - 0.06) if ph < 0.778 else 'content(%.3f before gap)' % (1.496 - ph)
for f in sorted(glob.glob(os.path.join(d, 'frame_*.tif'))):
    a = tifffile.memmap(f) if os.path.getsize(f) > 2e8 else tifffile.imread(f)
    t = (a[:, 0, 0].astype(np.int64) << 16) | (a[:, 0, 1].astype(np.int64) << 8) | a[:, 0, 2]
    cont = np.all(np.diff(t) == 1)
    y0, y1 = t[0] / dpi, t[-1] / dpi
    p0, p1 = phase(y0), phase(y1)
    # a frame is cut if the file starts/ends inside content (not gap/leader/no-film)
    bad = ('content' in p0) or ('content' in p1)
    ok &= cont and not bad
    print(f'{os.path.basename(f)}: {a.shape[1]}x{a.shape[0]} true {y0:.3f}"..{y1:.3f}"  start={p0}  end={p1}  contiguous={cont}')
print('PASS' if ok else 'CHECK')
