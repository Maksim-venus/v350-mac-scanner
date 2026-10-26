import sys, tifffile, numpy as np
a = tifffile.imread(sys.argv[1]); dpi = int(sys.argv[2])
if a.dtype == np.uint16: t = (a[:,0,0].astype(np.int64)<<16) | (a[:,0,1].astype(np.int64)<<8) | a[:,0,2]
else: t = (a[:,0,0].astype(np.int64)<<16) | (a[:,0,1].astype(np.int64)<<8) | a[:,0,2]
d = np.diff(t)
bad = np.where(d != 1)[0]
F = 1.0
def where(tl):
    y = tl/dpi
    if y < 1.6: return 'leader'
    ph = (y-1.6) % 1.496
    return 'GAP' if ph < 0.06 else 'CONTENT'
ok = True
for i in bad:
    loc = where(t[i]); jump = d[i]-1
    print(f'join at out row {i}: true line {t[i]} ({t[i]/dpi:.3f}") jump {jump:+d} lines -> {loc}')
    if loc == 'CONTENT' and abs(jump) > 0: ok = False
print('rows', len(t), 'PASS' if ok else 'FAIL (discontinuity in frame content)')
