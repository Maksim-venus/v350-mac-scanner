#!/usr/bin/env python3
"""v350gui — local web UI for v350scan (Epson V350 auto film loader).

    python3 v350gui.py [--port 8350] [--bin ../v350scan] [--no-browser]

Serves http://127.0.0.1:8350 (localhost only) and drives the v350scan CLI:
300 dpi preview -> place cut lines in the black gaps -> scan at 600..4800 dpi.
Standard library only.
"""
import argparse, atexit, json, os, re, shutil, signal, struct, subprocess, sys, tempfile, threading, time, webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

HERE = os.path.dirname(os.path.abspath(__file__))
STATE = {"busy": False, "kind": None, "log": [], "progress": None, "started": None,
         "result": None, "rc": None, "preview": None, "segments": {}, "scan_dir": None, "live_dir": None}
LOCK = threading.Lock()
PROC = {"p": None}
ALLOWED_PNG = set()
ARGS = None

def log(line):
    with LOCK:
        STATE["log"].append(line.rstrip("\n"))
        if len(STATE["log"]) > 400:
            STATE["log"] = STATE["log"][-400:]


# ---- private temp storage: previews and live-view files never go to ~/Pictures ----
TMP_BASE = os.path.join(tempfile.gettempdir(), "v350gui")
SESSION = {"dir": None}

def pid_alive(pid):
    try: os.kill(pid, 0); return True
    except ProcessLookupError: return False
    except PermissionError: return True

def clean_stale():
    """remove temp dirs left by GUI servers that are no longer running"""
    if not os.path.isdir(TMP_BASE): return
    for d in os.listdir(TMP_BASE):
        m = re.match(r"(\d+)_", d)
        if m and int(m.group(1)) != os.getpid() and not pid_alive(int(m.group(1))):
            shutil.rmtree(os.path.join(TMP_BASE, d), ignore_errors=True)

def session_dir():
    if not SESSION["dir"]:
        os.makedirs(TMP_BASE, mode=0o700, exist_ok=True)
        SESSION["dir"] = tempfile.mkdtemp(prefix="%d_" % os.getpid(), dir=TMP_BASE)
    return SESSION["dir"]

def wipe_session():
    p = PROC.get("p")
    if p and p.poll() is None:
        try: p.send_signal(signal.SIGINT); p.wait(timeout=30)
        except Exception: pass
    if SESSION["dir"]: shutil.rmtree(SESSION["dir"], ignore_errors=True); SESSION["dir"] = None

def drop_preview():
    """delete the current preview files (TIFF/PNG/JSON) and forget it"""
    with LOCK:
        pv = STATE.get("preview"); STATE["preview"] = None
    if pv:
        for k in ("tiff", "png", "json"):
            f = pv.get(k)
            if f and os.path.realpath(f).startswith(os.path.realpath(session_dir()) + os.sep):
                try: os.remove(f)
                except OSError: pass
                ALLOWED_PNG.discard(os.path.realpath(f))

def load_preview_json(path):
    with open(path) as f:
        j = json.load(f)
    j["json"] = path
    if j.get("png"):
        ALLOWED_PNG.add(os.path.realpath(j["png"]))
    return j

def run_job(kind, cmd, result_path):
    old = STATE.get("live_dir")
    if old and kind in ("scan", "preview"): shutil.rmtree(old, ignore_errors=True)
    with LOCK:
        STATE.update(busy=True, kind=kind, progress=None, started=time.time(), result=None, rc=None)
        if kind == "scan": STATE["segments"] = {}; STATE["scan_dir"] = result_path
        if kind in ("scan", "preview"):
            STATE["live_dir"] = live_dir = tempfile.mkdtemp(prefix="live_", dir=session_dir())
            cmd = cmd + ["--live", live_dir]
        STATE["log"] = []
    log("$ " + " ".join(cmd))
    try:
        p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)
    except OSError as e:
        log("无法启动：%s" % e)
        with LOCK: STATE.update(busy=False, rc=-1)
        return
    PROC["p"] = p
    for line in p.stdout:
        m = re.match(r"PROGRESS (\d+) (\d+) (\d+) (\d+)", line)
        if m:
            k, n, i, h = map(int, m.groups())
            with LOCK: STATE["progress"] = {"win": k, "nwin": n, "line": i, "lines": h}
            continue
        m = re.match(r"SEGMENT (\d+) (scanning|saved|failed|skipped) ?(.*)", line.strip())
        if m:
            with LOCK: STATE["segments"][m.group(1)] = {"status": m.group(2), "file": m.group(3) or None}
        log(line)
    rc = p.wait()
    PROC["p"] = None
    with LOCK:
        STATE.update(busy=False, rc=rc, result=result_path if rc == 0 else None)
    if rc == 0 and kind == "preview":
        try:
            pj = load_preview_json(os.path.splitext(result_path)[0] + ".json")
            with LOCK: STATE["preview"] = pj
        except Exception as e:
            log("无法读取预览 JSON：%s" % e)
    log("完成（退出码 0）" if rc == 0 else "失败（退出码 %d）" % rc)


ZH = [
    (r'DRIVER_MISSING.*', r'未找到 Epson 扫描仪驱动（Interpreter 68.bundle）：请先安装 Epson 官方 ICA Scanner Driver 5.8.23。'),
    (r'segment (\d+) \(([\d.]+)\.\.([\d.]+)\) is shorter than 0\.35"', r'第 \1 格太短（\2–\3 英寸，小于 0.35 英寸）：请删除或移动切分线，或取消勾选这一格。'),
    (r'segment (\d+) would be ([\d.]+)" long.*', r'第 \1 格的扫描窗口长 \2 英寸，超过 4.5 英寸（太长会扫描失败）：请在这一格里加切分线。'),
    (r'Add cuts so that every segment is about one frame\.', ''),
    (r'segment (\d+) exceeds the 9\.30" AFL area', r'第 \1 格超出了胶片仓 9.30 英寸的扫描范围：请把终点往上移。'),
    (r'segment (\d+): cut ([\d.]+) too close to the strip start', r'第 \1 格：切分线 \2 离胶片起点太近。'),
    (r'--only (\d+): there are (\d+) segments', r'所选的第 \1 格不存在（一共 \2 格）。'),
    (r'above 300 dpi give --cuts.*', r'300 dpi 以上需要切分线：请先预览。'),
    (r'or --cuts-from preview\.json; run .*', ''),
    (r'bad --cuts list', r'切分线格式错误。'),
    (r'need --preview or --dpi .*', r'分辨率无效。'),
]
def zh(text):
    out = []
    for line in text.splitlines():
        for pat, rep in ZH:
            if re.search(pat, line):
                line = re.sub(pat, rep, line); break
        else:
            continue
        if line.strip(): out.append(line)
    return "\n".join(out)


# ---- persistent settings (last chosen folder, dpi, bits) ----
CONF_DIR = os.path.expanduser("~/Library/Application Support/v350scan") if sys.platform == "darwin" \
    else os.path.expanduser("~/.config/v350scan")
CONF_FILE = os.path.join(CONF_DIR, "gui.json")
CONF_DEFAULT = {"outdir": "", "dpi": 300, "bits": 16, "film": "cneg", "interpreter": ""}  # outdir "" = none yet
FILMS = ("cneg", "bneg", "cpos", "bpos")

def load_conf():
    c = dict(CONF_DEFAULT)
    try:
        with open(CONF_FILE) as f: c.update({k: v for k, v in json.load(f).items() if k in CONF_DEFAULT})
    except Exception: pass
    return c

def save_conf(c):
    os.makedirs(CONF_DIR, exist_ok=True)
    tmp = CONF_FILE + ".tmp"
    with open(tmp, "w") as f: json.dump(c, f, ensure_ascii=False, indent=1)
    os.replace(tmp, CONF_FILE)

FRAME_RE = re.compile(r"^frame_(\d+)[._]", re.I)
def next_frame_number(d):
    """same rule as v350scan --continue-numbering: 1 + highest frame_NN.* in d"""
    try: nums = [int(m.group(1)) for f in os.listdir(d) for m in [FRAME_RE.match(f)] if m]
    except OSError: nums = []
    return (max(nums) if nums else 0) + 1

# ---- native folder picker (macOS) ----
# The folder is passed as argv (no quoting issues); `activate` brings the dialog to the front.
# Cancel -> osascript exits 1 ("User canceled. (-128)").
OSA_PICK = ["osascript",
            "-e", "on run argv",
            "-e", "activate",
            "-e", "set d to POSIX file (item 1 of argv)",
            "-e", 'return POSIX path of (choose folder with prompt "选择保存文件夹" default location d)',
            "-e", "end run"]
PICK_LOCK = threading.Lock()

def conf_folder():
    d = load_conf().get("outdir") or ""
    return os.path.expanduser(d) if d else ""

def pick_folder():
    """Open the picker at the last folder. Returns the chosen absolute path, or None if cancelled.
    Raises RuntimeError("picker open") if a picker is already showing."""
    if not PICK_LOCK.acquire(blocking=False): raise RuntimeError("picker open")
    try:
        last = conf_folder()
        d = last if last and os.path.isdir(last) else os.path.expanduser("~/Pictures")
        if not os.path.isdir(d): d = os.path.expanduser("~")
        cmd = (ARGS.picker.split() if ARGS.picker else OSA_PICK) + [d]
        try: r = subprocess.run(cmd, capture_output=True, text=True, timeout=900)
        except subprocess.TimeoutExpired: return None
        path = r.stdout.strip()
        if r.returncode != 0 or not path: return None
        path = os.path.realpath(os.path.expanduser(path))
        if not os.path.isdir(path): return None
        c = load_conf(); c["outdir"] = path
        try: save_conf(c)
        except OSError as e: log("无法保存设置：%s" % e)
        log("保存文件夹：%s" % path)
        return path
    finally:
        PICK_LOCK.release()

def interp_args():
    p = load_conf().get("interpreter") or ""
    return ["--interpreter", os.path.expanduser(p)] if p else []

DRIVER = {"t": 0, "v": None}
def driver_state():
    """cached `v350scan --check-driver` (no USB access)"""
    if DRIVER["v"] is None or time.time() - DRIVER["t"] > 10:
        try:
            r = subprocess.run([ARGS.bin, "--check-driver"] + interp_args(), capture_output=True, text=True, timeout=10)
            ok = r.returncode == 0
            path = next((l.split(" ", 1)[1] for l in r.stdout.splitlines() if l.startswith("DRIVER_OK ")), None)
            detail = "" if ok else ("找到了驱动但缺少固件 Esfw68.bin：%s" % path if r.returncode == 3 else "")
            DRIVER["v"] = {"ok": ok, "path": path, "detail": detail, "override": load_conf().get("interpreter") or ""}
        except Exception as e:
            DRIVER["v"] = {"ok": False, "path": None, "detail": str(e)}
        DRIVER["t"] = time.time()
    return DRIVER["v"]

def stamp():
    return time.strftime("%Y%m%d_%H%M%S")

def scan_cmd(q, dry=False):
    dpi = int(q.get("dpi", 600)); bits = int(q.get("bits", 16))
    if dpi not in (300, 600, 1200, 2400, 4800) or bits not in (8, 16):
        raise ValueError("bad dpi/bits")
    cuts = [float(c) for c in q.get("cuts", [])]
    only = [int(n) for n in q.get("only", [])]
    outdir = conf_folder()                                       # the folder chosen with the picker
    if not outdir: raise ValueError("no folder chosen")
    if not os.path.isdir(outdir): raise ValueError("folder missing")
    if dpi == 300:
        out = os.path.join(outdir, "strip_300_%d_%s.tif" % (bits, stamp()))
        cmd = [ARGS.bin, "--dpi", "300", "--bits", str(bits), "--progress", "--out", out] + interp_args()
    else:
        if not only: raise ValueError("no segments")
        out = outdir                                             # straight into the folder, numbers continue
        with LOCK: pv = STATE.get("preview") or {}
        film = pv.get("film") if pv.get("film") in FILMS else load_conf().get("film", "cpos")
        cmd = [ARGS.bin, "--dpi", str(dpi), "--bits", str(bits), "--progress", "--film", film,
               "--only", ",".join(str(n) for n in sorted(set(only))), "--outdir", outdir] + interp_args()
        if cuts: cmd += ["--cuts", ",".join("%.4f" % c for c in sorted(cuts))]
        if q.get("start") not in (None, ""): cmd += ["--start", "%.4f" % float(q["start"])]
        if q.get("end") not in (None, ""): cmd += ["--end", "%.4f" % float(q["end"])]
    if dry: cmd.append("--dry-run")
    return cmd, out

class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def send(self, code, body, ctype="application/json"):
        b = body if isinstance(body, bytes) else body.encode()
        self.send_response(code); self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(b))); self.send_header("Cache-Control", "no-store")
        self.end_headers(); self.wfile.write(b)
    def js(self, obj, code=200): self.send(code, json.dumps(obj))
    def body(self):
        n = int(self.headers.get("Content-Length") or 0)
        return json.loads(self.rfile.read(n) or b"{}")
    def do_GET(self):
        u = urlparse(self.path)
        if u.path == "/":
            with open(os.path.join(HERE, "index.html"), "rb") as f: return self.send(200, f.read(), "text/html; charset=utf-8")
        if u.path == "/api/state":
            with LOCK:
                s = dict(STATE); s["log"] = STATE["log"][-200:]
            s["elapsed"] = (time.time() - s["started"]) if s["started"] and s["busy"] else None
            s["bin"] = ARGS.bin
            d = conf_folder()
            s["folder"] = d or None; s["folder_ok"] = bool(d) and os.path.isdir(d)
            s["next"] = next_frame_number(d) if s["folder_ok"] else None
            s["picking"] = PICK_LOCK.locked()
            s["driver"] = driver_state()
            return self.js(s)
        if u.path == "/api/live":                     # rows of a live-view file from row `from`
            qs = parse_qs(u.query); name = qs.get("name", ["preview"])[0]
            if not re.fullmatch(r"preview|\d{2}", name): return self.send(400, "bad name", "text/plain")
            with LOCK: d = STATE.get("live_dir")
            p = os.path.join(d, "live_%s.raw" % name) if d else None
            if not p or not os.path.isfile(p): return self.send(404, "none", "text/plain")
            with open(p, "rb") as f:
                hd = f.read(16)
                if len(hd) < 16 or hd[:4] != b"V3LV": return self.send(404, "none", "text/plain")
                w, rows, dec = struct.unpack("<3I", hd[4:16])
                rb = w * 3; avail = (os.path.getsize(p) - 16) // rb
                start = max(0, min(int(qs.get("from", ["0"])[0]), avail))
                f.seek(16 + start * rb); body = f.read((avail - start) * rb)
            self.send_response(200); self.send_header("Content-Type", "application/octet-stream")
            for k, v in (("X-Width", w), ("X-Rows", rows), ("X-Avail", avail), ("X-From", start), ("X-Dec", dec)):
                self.send_header(k, str(v))
            self.send_header("Content-Length", str(len(body))); self.send_header("Cache-Control", "no-store")
            self.end_headers(); self.wfile.write(body); return
        if u.path == "/api/config":
            return self.js(load_conf())
        if u.path == "/api/nextnum":
            d = conf_folder()
            return self.js({"folder": d or None, "next": next_frame_number(d) if d and os.path.isdir(d) else None})
        if u.path == "/api/png":
            p = os.path.realpath(parse_qs(u.query).get("path", [""])[0])
            if p not in ALLOWED_PNG or not os.path.isfile(p): return self.send(404, "not found", "text/plain")
            with open(p, "rb") as f: return self.send(200, f.read(), "image/png")
        self.send(404, "not found", "text/plain")
    def do_POST(self):
        u = urlparse(self.path)
        try: q = self.body()
        except Exception: return self.js({"error": "bad json"}, 400)
        if u.path == "/api/cancel":
            p = PROC["p"]
            if p: p.send_signal(signal.SIGINT); log("已请求取消（将在下一行停止）")
            return self.js({"ok": True})
        if u.path == "/api/config":                 # dpi / bits only; the folder comes from the picker
            c = load_conf()
            for k in ("dpi", "bits"):
                if k in q: c[k] = int(q[k])
            if q.get("film") in FILMS: c["film"] = q["film"]
            if "interpreter" in q: c["interpreter"] = str(q["interpreter"] or "")
            try: save_conf(c)
            except OSError as e: return self.js({"error": str(e)}, 500)
            return self.js(c)
        if u.path == "/api/open":
            with LOCK: d = STATE.get("scan_dir")
            if not d or not os.path.isdir(d): d = conf_folder()
            if not d or not os.path.isdir(d): return self.js({"error": "no folder"}, 404)
            try:
                subprocess.Popen(["open", d] if sys.platform == "darwin" else ["xdg-open", d],
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            except OSError as e:
                return self.js({"error": str(e)}, 500)
            return self.js({"ok": True, "dir": d})
        if u.path == "/api/pickinterpreter":
            return self.js({"error": "unsupported"}, 400)
        if u.path == "/api/resetinterpreter":
            c = load_conf(); c["interpreter"] = ""; save_conf(c); DRIVER["v"] = None
            return self.js({"ok": True})
        if u.path == "/api/openurl":
            url = str(q.get("url", ""))
            if not url.startswith(("https://epson.com/", "https://ftp.epson.com/", "https://github.com/")):
                return self.js({"error": "bad url"}, 400)
            subprocess.Popen(["open" if sys.platform == "darwin" else "xdg-open", url],
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            return self.js({"ok": True})
        if u.path == "/api/plan":
            try: cmd, _ = scan_cmd(q, dry=True)
            except Exception as e: return self.js({"error": str(e)}, 400)
            r = subprocess.run(cmd, capture_output=True, text=True)
            text = (r.stderr + r.stdout).strip()
            return self.js({"rc": r.returncode, "text": text, "zh": zh(text) if r.returncode else ""})
        with LOCK:
            if STATE["busy"]: return self.js({"error": "busy"}, 409)
        if u.path == "/api/pickfolder":             # 更改文件夹
            try: d = pick_folder()
            except RuntimeError as e: return self.js({"error": str(e)}, 409)
            return self.js({"folder": d} if d else {"cancelled": True, "folder": conf_folder() or None})
        if u.path == "/api/preview":
            if q.get("pick", True):                  # 预览 = choose the folder first; cancel = no preview
                try: d = pick_folder()
                except RuntimeError as e: return self.js({"error": str(e)}, 409)
                if not d: return self.js({"cancelled": True})
                with LOCK:
                    if STATE["busy"]: return self.js({"error": "busy"}, 409)
            drop_preview()                               # previous preview is deleted when a new one starts
            out = os.path.join(session_dir(), "preview_%s.tif" % stamp())
            film = q.get("film") if q.get("film") in FILMS else load_conf().get("film", "cneg")
            c = load_conf(); c["film"] = film
            try: save_conf(c)
            except OSError: pass
            cmd = [ARGS.bin, "--preview", "--progress", "--film", film, "--out", out] + interp_args()
            threading.Thread(target=run_job, args=("preview", cmd, out), daemon=True).start()
            return self.js({"ok": True, "out": out})
        if u.path == "/api/scan":
            try: cmd, out = scan_cmd(q)
            except Exception as e: return self.js({"error": str(e)}, 400)
            threading.Thread(target=run_job, args=("scan", cmd, out), daemon=True).start()
            return self.js({"ok": True, "out": out})
        if u.path == "/api/status":
            threading.Thread(target=run_job, args=("status", [ARGS.bin, "--status"], None), daemon=True).start()
            return self.js({"ok": True})
        self.js({"error": "unknown"}, 404)

def main():
    global ARGS
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8350)
    ap.add_argument("--bin", default=os.path.join(os.path.dirname(HERE), "v350scan"))
    ap.add_argument("--no-browser", action="store_true")
    ap.add_argument("--picker", default=None,
                    help="test hook: command used instead of the macOS folder picker; gets the default folder "
                         "as last argument, prints the chosen path, non-zero exit = cancel")
    ARGS = ap.parse_args()
    ARGS.bin = os.path.abspath(os.path.expanduser(ARGS.bin))
    if not os.access(ARGS.bin, os.X_OK):
        print("v350scan binary not found/executable: %s (run ./build.sh)" % ARGS.bin); sys.exit(1)
    clean_stale()
    session_dir()
    atexit.register(wipe_session)
    def on_term(sig, frm): raise KeyboardInterrupt
    signal.signal(signal.SIGTERM, on_term); signal.signal(signal.SIGHUP, on_term)
    srv = ThreadingHTTPServer(("127.0.0.1", ARGS.port), H)
    url = "http://127.0.0.1:%d/" % ARGS.port
    print("v350gui: %s  (Ctrl-C to quit; quit Image Capture first)" % url)
    if not ARGS.no_browser: webbrowser.open(url)
    try: srv.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        wipe_session()                                   # stop a running job cleanly, delete previews
if __name__ == "__main__":
    main()
