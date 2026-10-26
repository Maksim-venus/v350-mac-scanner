// V350 Scanner.app — native macOS host for the v350scan CLI (Epson Perfection V350 auto film loader).
// A WKWebView shows the HTML UI (Resources/index.html); its fetch() calls go to the custom URL
// scheme v350:// and are answered here (no local HTTP server, no Python).
// The scanning itself is done by the bundled v350scan CLI (Contents/MacOS/v350scan), which loads
// Epson's interpreter from the official ICA driver install. Nothing from Epson is bundled.
import AppKit
import WebKit

// MARK: - small helpers

func jsonData(_ o: Any) -> Data { (try? JSONSerialization.data(withJSONObject: o, options: [])) ?? Data("{}".utf8) }
func stamp() -> String { let f = DateFormatter(); f.dateFormat = "yyyyMMdd_HHmmss"; return f.string(from: Date()) }
func nn(_ x: Any?) -> Any { x ?? NSNull() }
func isDir(_ p: String) -> Bool { var d: ObjCBool = false; return FileManager.default.fileExists(atPath: p, isDirectory: &d) && d.boolValue }
func expand(_ p: String) -> String { (p as NSString).expandingTildeInPath }

let FILMS = ["cneg", "bneg", "cpos", "bpos"]

// MARK: - settings (shared format with gui/v350gui.py)

struct Conf {
    // V350_CONF_DIR overrides (used by --selftest so it never touches the real settings)
    static var dir: String { if let e = getenv("V350_CONF_DIR") { return expand(String(cString: e)) }; return expand("~/Library/Application Support/v350scan") }
    static var file: String { dir + "/gui.json" }
    static func load() -> [String: Any] {
        var c: [String: Any] = ["outdir": "", "dpi": 300, "bits": 16, "film": "cneg", "interpreter": "",
                                "finish": true, "esc_n1": false]
        if let d = FileManager.default.contents(atPath: file),
           let j = try? JSONSerialization.jsonObject(with: d) as? [String: Any] {
            for k in ["outdir", "dpi", "bits", "film", "interpreter", "finish", "esc_n1"] { if let v = j[k] { c[k] = v } }
        }
        return c
    }
    static func save(_ c: [String: Any]) {
        try? FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
        if let d = try? JSONSerialization.data(withJSONObject: c, options: [.prettyPrinted]) {
            let tmp = file + ".tmp"
            FileManager.default.createFile(atPath: tmp, contents: d)
            _ = try? FileManager.default.replaceItemAt(URL(fileURLWithPath: file), withItemAt: URL(fileURLWithPath: tmp))
            if !FileManager.default.fileExists(atPath: file) { try? FileManager.default.moveItem(atPath: tmp, toPath: file) }
        }
    }
    static func folder() -> String { let d = (load()["outdir"] as? String) ?? ""; return d.isEmpty ? "" : expand(d) }
    static func interpreter() -> String { (load()["interpreter"] as? String) ?? "" }
}

func nextFrameNumber(_ d: String) -> Int {
    guard let items = try? FileManager.default.contentsOfDirectory(atPath: d) else { return 1 }
    let re = try! NSRegularExpression(pattern: "^frame_(\\d+)[._]", options: [.caseInsensitive])
    var mx = 0
    for f in items {
        if let m = re.firstMatch(in: f, range: NSRange(f.startIndex..., in: f)),
           let r = Range(m.range(at: 1), in: f), let v = Int(f[r]) { mx = max(mx, v) }
    }
    return mx + 1
}

// CLI messages -> Chinese (same table as gui/v350gui.py)
let ZH: [(String, String)] = [
    ("DRIVER_MISSING.*", "未找到 Epson 扫描仪驱动（Interpreter 68.bundle）：请先安装 Epson 官方 ICA Scanner Driver 5.8.23。"),
    ("segment (\\d+) \\(([\\d.]+)\\.\\.([\\d.]+)\\) is shorter than 0\\.35\"", "第 $1 格太短（$2–$3 英寸，小于 0.35 英寸）：请删除或移动切分线，或取消勾选这一格。"),
    ("segment (\\d+) would be ([\\d.]+)\" long.*", "第 $1 格的扫描窗口长 $2 英寸，超过 4.5 英寸（太长会扫描失败）：请在这一格里加切分线。"),
    ("Add cuts so that every segment is about one frame\\.", ""),
    ("segment (\\d+) exceeds the 9\\.30\" AFL area", "第 $1 格超出了胶片仓 9.30 英寸的扫描范围：请把终点往上移。"),
    ("segment (\\d+): cut ([\\d.]+) too close to the strip start", "第 $1 格：切分线 $2 离胶片起点太近。"),
    ("--only (\\d+): there are (\\d+) segments", "所选的第 $1 格不存在（一共 $2 格）。"),
    ("above 300 dpi give --cuts.*", "300 dpi 以上需要切分线：请先预览。"),
    ("or --cuts-from preview\\.json; run .*", ""),
    ("bad --cuts list", "切分线格式错误。"),
    ("need --preview or --dpi .*", "分辨率无效。"),
]
func zh(_ text: String) -> String {
    var out: [String] = []
    for line in text.components(separatedBy: "\n") {
        for (pat, rep) in ZH {
            let re = try! NSRegularExpression(pattern: pat)
            let r = NSRange(line.startIndex..., in: line)
            if re.firstMatch(in: line, range: r) != nil {
                let l = re.stringByReplacingMatches(in: line, range: r, withTemplate: rep)
                if !l.trimmingCharacters(in: .whitespaces).isEmpty { out.append(l) }
                break
            }
        }
    }
    return out.joined(separator: "\n")
}

// MARK: - scanner job state

final class Engine {
    let lock = NSLock()
    var busy = false, kind: String? = nil, log: [String] = [], progress: [String: Int]? = nil
    var started: Date? = nil, result: String? = nil, rc: Int? = nil
    var preview: [String: Any]? = nil, segments: [String: [String: Any]] = [:]
    var scanDir: String? = nil, liveDir: String? = nil
    var proc: Process? = nil
    var allowedPNG = Set<String>()
    var picking = false
    var driver: [String: Any] = ["ok": true, "path": NSNull(), "detail": ""]
    var driverChecked = Date.distantPast

    let bin: String
    let simulate: Bool
    let session: String

    init(bin: String, simulate: Bool) {
        self.bin = bin; self.simulate = simulate
        let base = (NSTemporaryDirectory() as NSString).appendingPathComponent("v350app")
        try? FileManager.default.createDirectory(atPath: base, withIntermediateDirectories: true,
                                                 attributes: [.posixPermissions: 0o700])
        // remove temp dirs of app instances that are no longer running
        for d in (try? FileManager.default.contentsOfDirectory(atPath: base)) ?? [] {
            if let pid = Int32(d.split(separator: "_").first ?? ""), pid != getpid(), kill(pid, 0) != 0, errno == ESRCH {
                try? FileManager.default.removeItem(atPath: base + "/" + d)
            }
        }
        session = base + "/\(getpid())_\(UUID().uuidString.prefix(8))"
        try? FileManager.default.createDirectory(atPath: session, withIntermediateDirectories: true)
    }

    func addLog(_ l: String) {
        lock.lock(); log.append(l); if log.count > 400 { log.removeFirst(log.count - 400) }; lock.unlock()
    }

    func interpArgs() -> [String] { let p = Conf.interpreter(); return p.isEmpty ? [] : ["--interpreter", expand(p)] }

    func env() -> [String: String] {
        var e = ProcessInfo.processInfo.environment
        if simulate { if e["M_LINE_MS"] == nil { e["M_LINE_MS"] = "0.4" }; if e["M_FILM"] == nil { e["M_FILM"] = "cneg" } }
        return e
    }

    func refreshDriver() {
        if Date().timeIntervalSince(driverChecked) < 10 { return }
        driverChecked = Date()
        let p = Process(); p.executableURL = URL(fileURLWithPath: bin); p.arguments = ["--check-driver"] + interpArgs()
        p.environment = env()
        let pipe = Pipe(); p.standardOutput = pipe; p.standardError = FileHandle.nullDevice
        do { try p.run() } catch { lock.lock(); driver = ["ok": false, "path": NSNull(), "detail": "\(error)"]; lock.unlock(); return }
        let out = String(data: pipe.fileHandleForReading.readDataToEndOfFile(), encoding: .utf8) ?? ""
        p.waitUntilExit()
        var path: Any = NSNull()
        for l in out.components(separatedBy: "\n") where l.hasPrefix("DRIVER_OK ") { path = String(l.dropFirst(10)) }
        let rc = p.terminationStatus
        let detail = rc == 3 ? "找到了驱动但缺少固件 Esfw68.bin：\(path)" : ""
        lock.lock(); driver = ["ok": rc == 0, "path": path, "detail": detail, "override": Conf.interpreter()]; lock.unlock()
    }

    func dropPreview() {
        lock.lock(); let pv = preview; preview = nil; lock.unlock()
        guard let pv = pv else { return }
        for k in ["tiff", "png", "json"] {
            if let f = pv[k] as? String, f.hasPrefix(session + "/") { try? FileManager.default.removeItem(atPath: f); allowedPNG.remove(f) }
        }
    }

    func run(kind: String, cmd: [String], result: String?) {
        if let old = liveDir, kind == "scan" || kind == "preview" { try? FileManager.default.removeItem(atPath: old) }
        var args = Array(cmd.dropFirst())
        lock.lock()
        busy = true; self.kind = kind; progress = nil; started = Date(); self.result = nil; rc = nil; log = []
        if kind == "scan" { segments = [:]; scanDir = result }
        if kind == "scan" || kind == "preview" {
            let ld = session + "/live_" + UUID().uuidString.prefix(6)
            try? FileManager.default.createDirectory(atPath: ld, withIntermediateDirectories: true)
            liveDir = ld; args += ["--live", ld]
        }
        lock.unlock()
        addLog("$ " + ([cmd[0]] + args).joined(separator: " "))
        let p = Process(); p.executableURL = URL(fileURLWithPath: cmd[0]); p.arguments = args; p.environment = env()
        let pipe = Pipe(); p.standardOutput = pipe; p.standardError = pipe
        do { try p.run() } catch {
            addLog("无法启动：\(error)"); lock.lock(); busy = false; rc = -1; lock.unlock(); return
        }
        lock.lock(); proc = p; lock.unlock()
        DispatchQueue.global().async {
            let h = pipe.fileHandleForReading
            var buf = Data()
            let progRe = try! NSRegularExpression(pattern: "^PROGRESS (\\d+) (\\d+) (\\d+) (\\d+)")
            let segRe = try! NSRegularExpression(pattern: "^SEGMENT (\\d+) (scanning|saved|failed|skipped) ?(.*)$")
            func handle(_ line: String) {
                let r = NSRange(line.startIndex..., in: line)
                if let m = progRe.firstMatch(in: line, range: r) {
                    let v = (1...4).map { Int(line[Range(m.range(at: $0), in: line)!])! }
                    self.lock.lock(); self.progress = ["win": v[0], "nwin": v[1], "line": v[2], "lines": v[3]]; self.lock.unlock()
                    return
                }
                let t = line.trimmingCharacters(in: .whitespaces)
                if let m = segRe.firstMatch(in: t, range: NSRange(t.startIndex..., in: t)) {
                    let n = String(t[Range(m.range(at: 1), in: t)!]), st = String(t[Range(m.range(at: 2), in: t)!])
                    let f = m.range(at: 3).length > 0 ? String(t[Range(m.range(at: 3), in: t)!]) : nil
                    self.lock.lock(); self.segments[n] = ["status": st, "file": nn(f)]; self.lock.unlock()
                }
                self.addLog(line)
            }
            while true {
                let d = h.availableData
                if d.isEmpty { break }
                buf.append(d)
                while let nl = buf.firstIndex(of: 10) {
                    let line = String(decoding: buf[buf.startIndex..<nl], as: UTF8.self)
                    buf.removeSubrange(buf.startIndex...nl)
                    handle(line)
                }
            }
            if !buf.isEmpty { handle(String(decoding: buf, as: UTF8.self)) }
            p.waitUntilExit()
            let code = Int(p.terminationStatus)
            self.lock.lock(); self.proc = nil; self.busy = false; self.rc = code; self.result = code == 0 ? result : nil; self.lock.unlock()
            if code == 0, kind == "preview", let res = result {
                let jp = (res as NSString).deletingPathExtension + ".json"
                if let d = FileManager.default.contents(atPath: jp), var j = try? JSONSerialization.jsonObject(with: d) as? [String: Any] {
                    j["json"] = jp
                    if let png = j["png"] as? String { self.allowedPNG.insert(png) }
                    self.lock.lock(); self.preview = j; self.lock.unlock()
                } else { self.addLog("无法读取预览 JSON") }
            }
            self.addLog(code == 0 ? "完成（退出码 0）" : "失败（退出码 \(code)）")
        }
    }

    func cancel() { lock.lock(); let p = proc; lock.unlock(); if let p = p { kill(p.processIdentifier, SIGINT); addLog("已请求取消（将在下一行停止）") } }

    func stateJSON() -> [String: Any] {
        lock.lock(); defer { lock.unlock() }
        let folder = Conf.folder()
        let ok = !folder.isEmpty && isDir(folder)
        var s: [String: Any] = [
            "busy": busy, "kind": nn(kind), "log": Array(log.suffix(200)),
            "progress": nn(progress), "started": nn(started?.timeIntervalSince1970),
            "result": nn(result), "rc": nn(rc), "preview": nn(preview),
            "segments": segments, "scan_dir": nn(scanDir), "live_dir": nn(liveDir),
            "bin": bin, "folder": nn(folder.isEmpty ? nil : folder), "folder_ok": ok,
            "next": nn(ok ? nextFrameNumber(folder) : nil), "picking": picking, "driver": driver,
            "simulate": simulate,
        ]
        s["elapsed"] = nn((busy && started != nil) ? Date().timeIntervalSince(started!) : nil)
        return s
    }

    /// builds the CLI command for /api/scan and /api/plan
    func scanCmd(_ q: [String: Any], dry: Bool) throws -> ([String], String) {
        struct E: Error, CustomStringConvertible { let description: String }
        let dpi = (q["dpi"] as? NSNumber)?.intValue ?? 600, bits = (q["bits"] as? NSNumber)?.intValue ?? 16
        guard [300, 600, 1200, 2400, 4800].contains(dpi), [8, 16].contains(bits) else { throw E(description: "bad dpi/bits") }
        let cuts = ((q["cuts"] as? [NSNumber]) ?? []).map { $0.doubleValue }.sorted()
        let only = Array(Set(((q["only"] as? [NSNumber]) ?? []).map { $0.intValue })).sorted()
        let outdir = Conf.folder()
        if outdir.isEmpty { throw E(description: "no folder chosen") }
        if !isDir(outdir) { throw E(description: "folder missing") }
        var cmd: [String], out: String
        let conf = Conf.load()
        let film = (preview?["film"] as? String).flatMap { FILMS.contains($0) ? $0 : nil } ?? ((conf["film"] as? String) ?? "cpos")
        let neg = film == "cneg" || film == "bneg"
        // 「输出成片」: negatives are saved as finished positives with the strip parameters of the preview
        let wantFinish = neg && ((q["finish"] as? Bool) ?? ((conf["finish"] as? Bool) ?? true))
        var finishArgs: [String] = []
        if wantFinish {
            guard let jp = preview?["json"] as? String, let f = preview?["finish"] as? [String: Any],
                  ((f["valid"] as? NSNumber)?.intValue ?? 0) == 1 else { throw E(description: "finish needs preview") }
            finishArgs = ["--finish-from", jp]
        }
        if neg && ((conf["esc_n1"] as? Bool) ?? false) { finishArgs.append("--esc-n1") }   // hidden A/B switch (gui.json)
        if dpi == 300 {
            out = outdir + "/strip_300_\(bits)_\(stamp()).tif"
            cmd = [bin, "--dpi", "300", "--bits", "\(bits)", "--progress", "--film", film, "--out", out] + finishArgs
        } else {
            if only.isEmpty { throw E(description: "no segments") }
            out = outdir
            cmd = [bin, "--dpi", "\(dpi)", "--bits", "\(bits)", "--progress", "--film", film] + finishArgs + [
                   "--only", only.map(String.init).joined(separator: ","), "--outdir", outdir]
            if !cuts.isEmpty { cmd += ["--cuts", cuts.map { String(format: "%.4f", $0) }.joined(separator: ",")] }
            if let s = q["start"] as? NSNumber { cmd += ["--start", String(format: "%.4f", s.doubleValue)] }
            if let e = q["end"] as? NSNumber { cmd += ["--end", String(format: "%.4f", e.doubleValue)] }
        }
        cmd += interpArgs()
        if dry { cmd.append("--dry-run") }
        return (cmd, out)
    }

    func wipe() {
        cancel()
        lock.lock(); let p = proc; lock.unlock()
        if let p = p { let t0 = Date(); while p.isRunning && Date().timeIntervalSince(t0) < 30 { usleep(100_000) } }
        try? FileManager.default.removeItem(atPath: session)
    }
}

// MARK: - v350:// scheme handler

final class Handler: NSObject, WKURLSchemeHandler {
    let eng: Engine
    weak var window: NSWindow?
    var stopped = Set<ObjectIdentifier>()
    init(_ e: Engine) { eng = e }

    func webView(_ webView: WKWebView, stop task: WKURLSchemeTask) { stopped.insert(ObjectIdentifier(task)) }

    func reply(_ task: WKURLSchemeTask, _ code: Int, _ body: Data, _ type: String, _ extra: [String: String] = [:]) {
        let send = {
            if self.stopped.remove(ObjectIdentifier(task)) != nil { return }
            var h = ["Content-Type": type, "Content-Length": "\(body.count)", "Cache-Control": "no-store"]
            for (k, v) in extra { h[k] = v }
            let r = HTTPURLResponse(url: task.request.url!, statusCode: code, httpVersion: "HTTP/1.1", headerFields: h)!
            task.didReceive(r); task.didReceive(body); task.didFinish()
        }
        if Thread.isMainThread { send() } else { DispatchQueue.main.async(execute: send) }
    }
    func js(_ task: WKURLSchemeTask, _ o: Any, _ code: Int = 200) { reply(task, code, jsonData(o), "application/json") }

    var testFolder: String? = nil      // --selftest DIR: the "picker" answers DIR without a dialog

    func pickFolder(_ done: @escaping (String?) -> Void) {
        if let t = testFolder { var c = Conf.load(); c["outdir"] = t; Conf.save(c); done(t); return }
        if eng.picking { done(nil); return }
        eng.picking = true
        let p = NSOpenPanel()
        p.canChooseDirectories = true; p.canChooseFiles = false; p.canCreateDirectories = true
        p.allowsMultipleSelection = false
        p.message = "选择保存文件夹"; p.prompt = "选择"; p.title = "选择保存文件夹"
        let last = Conf.folder()
        p.directoryURL = URL(fileURLWithPath: !last.isEmpty && isDir(last) ? last : expand("~/Pictures"))
        NSApp.activate(ignoringOtherApps: true)
        let finish: (NSApplication.ModalResponse) -> Void = { r in
            self.eng.picking = false
            guard r == .OK, let u = p.url else { done(nil); return }
            var c = Conf.load(); c["outdir"] = u.path; Conf.save(c)
            self.eng.addLog("保存文件夹：\(u.path)")
            done(u.path)
        }
        if let w = window { p.beginSheetModal(for: w, completionHandler: finish) } else { finish(p.runModal()) }
    }

    func pickInterpreter(_ done: @escaping (String?) -> Void) {
        let p = NSOpenPanel()
        p.canChooseDirectories = true; p.canChooseFiles = true; p.treatsFilePackagesAsDirectories = true
        p.message = "选择 Epson 驱动：Interpreter 68.bundle（或 EPSON Scanner.app）"; p.prompt = "选择"
        p.directoryURL = URL(fileURLWithPath: "/Library/Image Capture/Devices")
        let finish: (NSApplication.ModalResponse) -> Void = { r in
            guard r == .OK, let u = p.url else { done(nil); return }
            var c = Conf.load(); c["interpreter"] = u.path; Conf.save(c); self.eng.driverChecked = .distantPast
            done(u.path)
        }
        if let w = window { p.beginSheetModal(for: w, completionHandler: finish) } else { finish(p.runModal()) }
    }

    func webView(_ webView: WKWebView, start task: WKURLSchemeTask) {
        guard let url = task.request.url, let comps = URLComponents(url: url, resolvingAgainstBaseURL: false) else { return }
        let path = comps.path.isEmpty ? "/" : comps.path
        let qi = Dictionary((comps.queryItems ?? []).map { ($0.name, $0.value ?? "") }, uniquingKeysWith: { a, _ in a })
        var q: [String: Any] = [:]
        if let b = task.request.httpBody, let j = try? JSONSerialization.jsonObject(with: b) as? [String: Any] { q = j }
        else if let s = qi["q"], let j = try? JSONSerialization.jsonObject(with: Data(s.utf8)) as? [String: Any] { q = j }
        let post = task.request.httpMethod == "POST"

        if !post {
            switch path {
            case "/", "/index.html":
                let f = Bundle.main.path(forResource: "index", ofType: "html")!
                reply(task, 200, FileManager.default.contents(atPath: f) ?? Data(), "text/html; charset=utf-8")
            case "/api/state":
                DispatchQueue.global().async { self.eng.refreshDriver(); self.js(task, self.eng.stateJSON()) }
            case "/api/config": js(task, Conf.load())
            case "/api/nextnum":
                let d = Conf.folder()
                var r: [String: Any] = ["folder": NSNull(), "next": NSNull()]
                if !d.isEmpty { r["folder"] = d; if isDir(d) { r["next"] = nextFrameNumber(d) } }
                js(task, r)
            case "/api/png":
                let p = qi["path"] ?? ""
                if eng.allowedPNG.contains(p), let d = FileManager.default.contents(atPath: p) { reply(task, 200, d, "image/png") }
                else { reply(task, 404, Data("not found".utf8), "text/plain") }
            case "/api/live":
                let name = qi["name"] ?? "preview"
                guard name == "preview" || name.range(of: "^\\d{2}$", options: .regularExpression) != nil else {
                    reply(task, 400, Data("bad name".utf8), "text/plain"); return }
                eng.lock.lock(); let ld = eng.liveDir; eng.lock.unlock()
                guard let d = ld, let fh = FileHandle(forReadingAtPath: d + "/live_\(name).raw") else {
                    reply(task, 404, Data("none".utf8), "text/plain"); return }
                defer { try? fh.close() }
                let hd = fh.readData(ofLength: 16)
                guard hd.count == 16, hd.prefix(4) == Data("V3LV".utf8) else { reply(task, 404, Data("none".utf8), "text/plain"); return }
                func u32(_ o: Int) -> Int { Int(hd[o]) | Int(hd[o + 1]) << 8 | Int(hd[o + 2]) << 16 | Int(hd[o + 3]) << 24 }
                let w = u32(4), rows = u32(8), dec = u32(12), rb = w * 3
                let size = Int((try? FileManager.default.attributesOfItem(atPath: d + "/live_\(name).raw")[.size] as? NSNumber)??.intValue ?? 16)
                let avail = rb > 0 ? (size - 16) / rb : 0
                let from = max(0, min(Int(qi["from"] ?? "0") ?? 0, avail))
                fh.seek(toFileOffset: UInt64(16 + from * rb))
                let body = fh.readData(ofLength: (avail - from) * rb)
                reply(task, 200, body, "application/octet-stream",
                      ["X-Width": "\(w)", "X-Rows": "\(rows)", "X-Avail": "\(avail)", "X-From": "\(from)", "X-Dec": "\(dec)"])
            default: reply(task, 404, Data("not found".utf8), "text/plain")
            }
            return
        }

        switch path {
        case "/api/cancel": eng.cancel(); js(task, ["ok": true]); return
        case "/api/config":
            var c = Conf.load()
            for k in ["dpi", "bits"] { if let v = q[k] as? NSNumber { c[k] = v.intValue } }
            if let f = q["film"] as? String, FILMS.contains(f) { c["film"] = f }
            if let f = q["finish"] as? Bool { c["finish"] = f }
            Conf.save(c); js(task, c); return
        case "/api/open":
            eng.lock.lock(); var d = eng.scanDir ?? ""; eng.lock.unlock()
            if d.isEmpty || !isDir(d) { d = Conf.folder() }
            if d.isEmpty || !isDir(d) { js(task, ["error": "no folder"], 404); return }
            NSWorkspace.shared.open(URL(fileURLWithPath: d)); js(task, ["ok": true, "dir": d]); return
        case "/api/openurl":
            let u = (q["url"] as? String) ?? ""
            guard u.hasPrefix("https://epson.com/") || u.hasPrefix("https://ftp.epson.com/") || u.hasPrefix("https://github.com/"),
                  let url = URL(string: u) else { js(task, ["error": "bad url"], 400); return }
            NSWorkspace.shared.open(url); js(task, ["ok": true]); return
        case "/api/pickinterpreter":
            pickInterpreter { p in
                var r: [String: Any] = [:]
                if let p = p { r["path"] = p } else { r["cancelled"] = true }
                self.js(task, r)
            }; return
        case "/api/resetinterpreter":
            var c = Conf.load(); c["interpreter"] = ""; Conf.save(c); eng.driverChecked = .distantPast; js(task, ["ok": true]); return
        case "/api/plan":
            do {
                let (cmd, _) = try eng.scanCmd(q, dry: true)
                DispatchQueue.global().async {
                    let p = Process(); p.executableURL = URL(fileURLWithPath: cmd[0]); p.arguments = Array(cmd.dropFirst())
                    p.environment = self.eng.env()
                    let pipe = Pipe(); p.standardOutput = pipe; p.standardError = pipe
                    do { try p.run() } catch { self.js(task, ["error": "\(error)"], 500); return }
                    let out = String(decoding: pipe.fileHandleForReading.readDataToEndOfFile(), as: UTF8.self)
                    p.waitUntilExit()
                    let rc = Int(p.terminationStatus), text = out.trimmingCharacters(in: .whitespacesAndNewlines)
                    self.js(task, ["rc": rc, "text": text, "zh": rc != 0 ? zh(text) : ""])
                }
            } catch { js(task, ["error": "\(error)"], 400) }
            return
        default: break
        }
        eng.lock.lock(); let busy = eng.busy; eng.lock.unlock()
        if busy { js(task, ["error": "busy"], 409); return }
        switch path {
        case "/api/pickfolder":
            if eng.picking { js(task, ["error": "picker open"], 409); return }
            pickFolder { d in
                let f = Conf.folder()
                var r: [String: Any] = [:]
                if let d = d { r["folder"] = d } else { r["cancelled"] = true; r["folder"] = f.isEmpty ? NSNull() : (f as Any) }
                self.js(task, r)
            }
        case "/api/preview":
            if eng.picking { js(task, ["error": "picker open"], 409); return }
            let film = (q["film"] as? String).flatMap { FILMS.contains($0) ? $0 : nil } ?? ((Conf.load()["film"] as? String) ?? "cneg")
            var c = Conf.load(); c["film"] = film; Conf.save(c)
            let start = {
                self.eng.dropPreview()
                let out = self.eng.session + "/preview_\(stamp()).tif"
                let n1 = (film == "cneg" || film == "bneg") && ((Conf.load()["esc_n1"] as? Bool) ?? false) ? ["--esc-n1"] : []
                self.eng.run(kind: "preview", cmd: [self.eng.bin, "--preview", "--progress", "--film", film, "--out", out] + n1 + self.eng.interpArgs(), result: out)
                self.js(task, ["ok": true, "out": out])
            }
            if (q["pick"] as? Bool) ?? true {
                pickFolder { d in
                    if d == nil { self.js(task, ["cancelled": true]); return }
                    self.eng.lock.lock(); let b = self.eng.busy; self.eng.lock.unlock()
                    if b { self.js(task, ["error": "busy"], 409); return }
                    start()
                }
            } else { start() }
        case "/api/scan":
            do { let (cmd, out) = try eng.scanCmd(q, dry: false); eng.run(kind: "scan", cmd: cmd, result: out); js(task, ["ok": true, "out": out]) }
            catch { js(task, ["error": "\(error)"], 400) }
        case "/api/status":
            eng.run(kind: "status", cmd: [eng.bin, "--status"] + eng.interpArgs(), result: nil); js(task, ["ok": true])
        default: js(task, ["error": "unknown"], 404)
        }
    }
}

// MARK: - app

final class AppDelegate: NSObject, NSApplicationDelegate, WKUIDelegate {
    var window: NSWindow!
    var web: WKWebView!
    var eng: Engine!
    var handler: Handler!
    var selftest: String? = nil

    func applicationDidFinishLaunching(_ n: Notification) {
        let args = CommandLine.arguments
        let simulate = args.contains("--simulate") || ProcessInfo.processInfo.environment["V350_SIMULATE"] == "1"
        let exeDir = (Bundle.main.executablePath! as NSString).deletingLastPathComponent
        let bin = exeDir + (simulate ? "/v350scan-sim" : "/v350scan")
        eng = Engine(bin: bin, simulate: simulate)
        handler = Handler(eng)
        if simulate, let i = args.firstIndex(of: "--selftest"), i + 1 < args.count {
            selftest = args[i + 1]; handler.testFolder = selftest
            setenv("V350_CONF_DIR", selftest! + "/conf", 1)
            try? FileManager.default.createDirectory(atPath: selftest!, withIntermediateDirectories: true)
        }

        let cfg = WKWebViewConfiguration()
        cfg.setURLSchemeHandler(handler, forURLScheme: "v350")
        web = WKWebView(frame: .zero, configuration: cfg)
        web.uiDelegate = self
        window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 1400, height: 900),
                          styleMask: [.titled, .closable, .miniaturizable, .resizable], backing: .buffered, defer: false)
        window.title = "V350 胶片条扫描" + (simulate ? "（模拟器，不连接扫描仪）" : "")
        window.contentView = web
        window.center(); window.setFrameAutosaveName("V350Main")
        window.appearance = NSAppearance(named: .aqua)
        handler.window = window
        web.load(URLRequest(url: URL(string: "v350://app/")!))
        window.makeKeyAndOrderFront(nil)
        if selftest != nil { DispatchQueue.main.asyncAfter(deadline: .now() + 2) { self.selfTest(step: 0) } }
        NSApp.activate(ignoringOtherApps: true)
        buildMenu()
    }

    func buildMenu() {
        let main = NSMenu()
        let appItem = NSMenuItem(); main.addItem(appItem)
        let am = NSMenu()
        am.addItem(withTitle: "关于 V350 Scanner", action: #selector(NSApplication.orderFrontStandardAboutPanel(_:)), keyEquivalent: "")
        am.addItem(.separator())
        am.addItem(withTitle: "退出 V350 Scanner", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        appItem.submenu = am
        let editItem = NSMenuItem(); main.addItem(editItem)
        let em = NSMenu(title: "编辑")
        em.addItem(withTitle: "拷贝", action: #selector(NSText.copy(_:)), keyEquivalent: "c")
        em.addItem(withTitle: "粘贴", action: #selector(NSText.paste(_:)), keyEquivalent: "v")
        em.addItem(withTitle: "全选", action: #selector(NSText.selectAll(_:)), keyEquivalent: "a")
        editItem.submenu = em
        let viewItem = NSMenuItem(); main.addItem(viewItem)
        let vm = NSMenu(title: "显示")
        vm.addItem(withTitle: "重新载入", action: #selector(reload), keyEquivalent: "r")
        viewItem.submenu = vm
        NSApp.mainMenu = main
    }
    @objc func reload() { web.reload() }

    // --simulate --selftest DIR: click 预览, wait, click 扫描所选格 at 600 dpi, wait, snapshot, quit.
    func say(_ s: String) { FileHandle.standardOutput.write(Data(("SELFTEST " + s + "\n").utf8)) }
    func snapshot(_ name: String, _ then: @escaping () -> Void) {
        web.takeSnapshot(with: nil) { img, _ in
            if let img = img, let t = img.tiffRepresentation, let r = NSBitmapImageRep(data: t), let png = r.representation(using: .png, properties: [:]) {
                let p = self.selftest! + "/" + name; try? png.write(to: URL(fileURLWithPath: p)); self.say("snapshot " + p)
            }
            then()
        }
    }
    func selfTest(step: Int, tries: Int = 0) {
        let st = eng.stateJSON()
        let busy = st["busy"] as? Bool ?? false
        func later(_ s: Int, _ t: Int = 0) { DispatchQueue.main.asyncAfter(deadline: .now() + 1) { self.selfTest(step: s, tries: t) } }
        if tries > 180 { say("TIMEOUT at step \(step)"); NSApp.terminate(nil); return }
        switch step {
        case 0:
            snapshot("ui_start.png") {
                self.web.evaluateJavaScript("document.getElementById('film').value='cneg'; document.getElementById('bprev').click(); 'ok'") { r, e in
                    self.say("clicked preview \(r ?? "") \(e.map { "\($0)" } ?? "")"); later(1) }
            }
        case 1:
            if busy || !(st["preview"] is [String: Any]) { later(1, tries + 1); return }
            let pv = st["preview"] as! [String: Any]
            say("preview film=\(pv["film"] ?? "") cuts=\(pv["cuts"] ?? "")")
            snapshot("ui_preview.png") {
                self.web.evaluateJavaScript("document.getElementById('dpi').value='600'; document.getElementById('dpi').dispatchEvent(new Event('change')); setTimeout(()=>document.getElementById('bscan').click(),1500); 'ok'") { _, e in
                    if let e = e { self.say("js error \(e)") }; later(2) }
            }
        case 2:
            if !busy && (st["kind"] as? String) != "scan" { later(2, tries + 1); return }
            later(3)
        case 3:
            if busy { later(3, tries + 1); return }
            say("scan rc=\(st["rc"] ?? "") segments=\(st["segments"] ?? "")")
            let files = (try? FileManager.default.contentsOfDirectory(atPath: selftest!))?.sorted() ?? []
            say("files " + files.joined(separator: " "))
            snapshot("ui_scanned.png") { self.say("done"); NSApp.terminate(nil) }
        default: break
        }
    }

    // JS alert / confirm -> native sheets
    func webView(_ w: WKWebView, runJavaScriptAlertPanelWithMessage m: String, initiatedByFrame f: WKFrameInfo, completionHandler: @escaping () -> Void) {
        if selftest != nil { say("alert: " + m); completionHandler(); return }
        let a = NSAlert(); a.messageText = m; a.addButton(withTitle: "好")
        a.beginSheetModal(for: window) { _ in completionHandler() }
    }
    func webView(_ w: WKWebView, runJavaScriptConfirmPanelWithMessage m: String, initiatedByFrame f: WKFrameInfo, completionHandler: @escaping (Bool) -> Void) {
        if selftest != nil { say("confirm: " + m.replacingOccurrences(of: "\n", with: " | ")); completionHandler(true); return }
        let a = NSAlert(); a.messageText = m; a.addButton(withTitle: "确定"); a.addButton(withTitle: "取消")
        a.beginSheetModal(for: window) { r in completionHandler(r == .alertFirstButtonReturn) }
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ s: NSApplication) -> Bool { true }
    func applicationShouldTerminate(_ s: NSApplication) -> NSApplication.TerminateReply {
        eng.lock.lock(); let busy = eng.busy; eng.lock.unlock()
        if busy {
            let a = NSAlert(); a.messageText = "正在扫描，确定要退出吗？"; a.informativeText = "退出会取消当前扫描（在下一行停止）。"
            a.addButton(withTitle: "取消扫描并退出"); a.addButton(withTitle: "继续扫描")
            if a.runModal() != .alertFirstButtonReturn { return .terminateCancel }
        }
        DispatchQueue.global().async { self.eng.wipe(); DispatchQueue.main.async { NSApp.reply(toApplicationShouldTerminate: true) } }
        return .terminateLater
    }
}

let app = NSApplication.shared
let delegate = AppDelegate()
app.delegate = delegate
app.setActivationPolicy(.regular)
signal(SIGPIPE, SIG_IGN)
app.run()
