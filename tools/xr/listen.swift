// listen.swift - what is said in the headset, as text, with the Mac's own speech recognition (on the device when the
// language supports it; nothing is sent to a cloud service then). Used by tools/xr/xr_bridge.py (docs/xr.md).
//     build/openphysics-listen clip.wav [en-US|de-DE|...]       prints the words, or fails with a reason on stderr
//     "build/OpenPhysicsAI Listen.app" (open -g -a ... --args --serve DIR --parent PID)
//         the same as a small background app with its own name, so that macOS asks once whether "OpenPhysicsAI
//         Listen" may use speech recognition, whoever started the bridge (a program from Terminal is refused otherwise,
//         since Terminal declares no use of speech). It serves the folder DIR: NAME.req (JSON {"wav": path, "lang":
//         "en-US"}) is answered by NAME.json ({"text": ..., "ms": ...} or {"error": ...}); DIR/ready.json says its pid
//         and whether recognition is allowed. The recognizer stays loaded between sentences. It ends with the bridge
//         (PID), or after 30 minutes without a request. --standin (or OPENPHYSICS_LISTEN_STANDIN=1) answers "stand-in: N bytes"
//         instead of recognising: the plumbing's test, which must not ask for a permission.
import Foundation
import Speech

func fail(_ msg: String) -> Never {
    FileHandle.standardError.write((msg + "\n").data(using: .utf8)!)
    exit(1)
}

let args = CommandLine.arguments
let env = ProcessInfo.processInfo.environment  // NAVIER_: the name before the rename of 2026-10-21
let standin = (env["OPENPHYSICS_LISTEN_STANDIN"] ?? env["NAVIER_LISTEN_STANDIN"]) == "1" || args.contains("--standin")

func authorization() -> SFSpeechRecognizerAuthorizationStatus {
    var status = SFSpeechRecognizer.authorizationStatus()
    if status == .notDetermined {
        let done = DispatchSemaphore(value: 0)
        SFSpeechRecognizer.requestAuthorization { s in status = s; done.signal() }
        while done.wait(timeout: .now() + 0.05) == .timedOut { RunLoop.current.run(until: Date(timeIntervalSinceNow: 0.05)) }
    }
    return status
}

func statusName(_ s: SFSpeechRecognizerAuthorizationStatus) -> String {
    switch s {
    case .authorized: return "authorized"
    case .denied: return "denied"
    case .restricted: return "restricted"
    default: return "not determined"
    }
}

var recognizers: [String: SFSpeechRecognizer] = [:]
func recognizer(_ lang: String) -> SFSpeechRecognizer? {
    if let r = recognizers[lang] { return r }
    let r = SFSpeechRecognizer(locale: Locale(identifier: lang))
    recognizers[lang] = r
    return r
}

// one clip into words; calls back on the main run loop
func recognise(_ wav: URL, _ lang: String, _ done: @escaping (String?, String?) -> Void) {
    guard let rec = recognizer(lang), rec.isAvailable else { return done(nil, "no speech recognition for \(lang)") }
    let req = SFSpeechURLRecognitionRequest(url: wav)
    req.shouldReportPartialResults = false
    if rec.supportsOnDeviceRecognition { req.requiresOnDeviceRecognition = true }
    var answered = false
    rec.recognitionTask(with: req) { result, error in
        if answered { return }
        if let r = result, r.isFinal { answered = true; done(r.bestTranscription.formattedString, nil) }
        else if let e = error { answered = true; done(nil, "recognition failed: \(e.localizedDescription)") }
    }
}

func writeJSON(_ obj: [String: Any], _ path: String) {
    guard let d = try? JSONSerialization.data(withJSONObject: obj) else { return }
    let tmp = path + ".tmp"
    FileManager.default.createFile(atPath: tmp, contents: d)
    try? FileManager.default.removeItem(atPath: path)
    try? FileManager.default.moveItem(atPath: tmp, toPath: path)
}

if let i = args.firstIndex(of: "--serve"), i + 1 < args.count {
    let dir = args[i + 1]
    let parent = args.firstIndex(of: "--parent").flatMap { $0 + 1 < args.count ? Int32(args[$0 + 1]) : nil }
    try? FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
    let status = standin ? "authorized" : statusName(authorization())
    writeJSON(["pid": Int(getpid()), "status": status, "standin": standin], dir + "/ready.json")
    var last = Date(), busy = false
    Timer.scheduledTimer(withTimeInterval: 0.03, repeats: true) { _ in
        if let p = parent, kill(p, 0) != 0 { exit(0) } // the bridge has gone
        if Date().timeIntervalSince(last) > 1800 { exit(0) }
        if busy { return }
        let names = (try? FileManager.default.contentsOfDirectory(atPath: dir)) ?? []
        guard let req = names.filter({ $0.hasSuffix(".req") }).sorted().first else { return }
        let base = String(req.dropLast(4)), reqPath = dir + "/" + req
        let j = (try? JSONSerialization.jsonObject(with: Data(contentsOf: URL(fileURLWithPath: reqPath)))) as? [String: Any] ?? [:]
        try? FileManager.default.removeItem(atPath: reqPath)
        last = Date()
        let wav = j["wav"] as? String ?? "", lang = j["lang"] as? String ?? "en-US", t0 = Date()
        let answer = { (text: String?, error: String?) in
            var o: [String: Any] = ["ms": Int(Date().timeIntervalSince(t0) * 1000)]
            if let t = text { o["text"] = t } else { o["error"] = error ?? "no words were recognised" }
            writeJSON(o, dir + "/" + base + ".json")
            busy = false
        }
        busy = true
        if standin {
            let n = (try? FileManager.default.attributesOfItem(atPath: wav)[.size] as? Int) ?? -1
            return answer("stand-in: \(n) bytes", nil)
        }
        if status != "authorized" {
            return answer(nil, "speech recognition is not allowed for OpenPhysicsAI Listen: System Settings > Privacy & Security > Speech Recognition")
        }
        recognise(URL(fileURLWithPath: wav), lang, answer)
    }
    RunLoop.main.run()
}

if args.count < 2 { fail("usage: openphysics-listen clip.wav [locale] | --serve DIR [--parent PID]") }
if !standin && authorization() != .authorized {
    fail("speech recognition is not allowed for this program: System Settings > Privacy & Security > Speech Recognition")
}
if standin { print(URL(fileURLWithPath: args[1]).deletingPathExtension().lastPathComponent); exit(0) }
recognise(URL(fileURLWithPath: args[1]), args.count > 2 ? args[2] : "en-US") { text, error in
    if let t = text { print(t); exit(0) }
    fail(error ?? "no words were recognised")
}
RunLoop.main.run(until: Date(timeIntervalSinceNow: 45))
fail("recognition took longer than 45 s")
