// voice.swift - the AI's answers spoken, fast: one synthesizer kept for the whole session, so a sentence becomes sound
// in tens of milliseconds instead of the second `say` needs to start each time (tools/xr/xr_bridge.py, docs/xr.md).
//     build/openphysics-voice [--voice NAME|IDENTIFIER] [--lang en-US]
// One JSON object a line on stdin: {"id": 7, "text": "...", "out": "/path/a.wav"}. One a line on stdout: first
// {"ready": true, "voice": "...", "identifier": "...", "quality": 1}, then {"id": 7, "out": "...", "seconds": 2.04,
// "ms": 46} for each sentence written (mono 16-bit WAVE at the voice's own rate), or {"id": 7, "error": "..."}.
// The voice: the one named, else the best installed for the language (Premium, then Enhanced, then Apple's Siri
// voices, then its default), never the novelty voices. It needs no permission: nothing is listened to here.
import AVFoundation
import Foundation

func emit(_ o: [String: Any]) {
    if let d = try? JSONSerialization.data(withJSONObject: o), let s = String(data: d, encoding: .utf8) {
        print(s)
        fflush(stdout)
    }
}

var want = "", lang = "en-US"
var args = CommandLine.arguments.dropFirst().makeIterator()
while let a = args.next() {
    if a == "--voice", let v = args.next() { want = v } else if a == "--lang", let l = args.next() { lang = l }
}

let novelty: Set<String> = ["Albert", "Bad News", "Bahh", "Bells", "Boing", "Bubbles", "Cellos", "Good News", "Jester",
                            "Organ", "Superstar", "Trinoids", "Whisper", "Wobble", "Zarvox", "Fred", "Junior", "Kathy", "Ralph"]
func pickVoice() -> AVSpeechSynthesisVoice? {
    let all = AVSpeechSynthesisVoice.speechVoices()
    if !want.isEmpty, let v = all.first(where: { $0.identifier == want })
        ?? all.first(where: { $0.name.caseInsensitiveCompare(want) == .orderedSame && $0.language == lang })
        ?? all.first(where: { $0.name.caseInsensitiveCompare(want) == .orderedSame }) {
        return v
    }
    let ranked = all.filter { $0.language == lang && !novelty.contains($0.name) && !$0.identifier.contains("eloquence") }
        .sorted { a, b in
            if a.quality != b.quality { return a.quality.rawValue > b.quality.rawValue }
            let sa = a.identifier.contains("siri"), sb = b.identifier.contains("siri")
            if sa != sb { return sa }
            return a.name < b.name
        }
    return ranked.first ?? AVSpeechSynthesisVoice(language: lang)
}

let synth = AVSpeechSynthesizer()
let voice = pickVoice()

// one sentence into samples: the synthesizer calls back with buffers, the last one empty
func samples(_ text: String) -> (data: [Int16], rate: Double) {
    let u = AVSpeechUtterance(string: text)
    u.voice = voice
    var out: [Int16] = []
    var rate = 22050.0
    var done = false
    synth.write(u) { buf in
        guard let pcm = buf as? AVAudioPCMBuffer else { return }
        if pcm.frameLength == 0 { done = true; return }
        rate = pcm.format.sampleRate
        let n = Int(pcm.frameLength)
        if let f = pcm.floatChannelData?[0] {
            for i in 0..<n { out.append(Int16(max(-1, min(1, f[i])) * 32767)) }
        } else if let s = pcm.int16ChannelData?[0] {
            out.append(contentsOf: UnsafeBufferPointer(start: s, count: n))
        } else if let s = pcm.int32ChannelData?[0] {
            for i in 0..<n { out.append(Int16(truncatingIfNeeded: s[i] >> 16)) }
        }
    }
    let end = Date(timeIntervalSinceNow: 30)
    while !done && Date() < end { RunLoop.current.run(until: Date(timeIntervalSinceNow: 0.002)) }
    return (out, rate)
}

func wave(_ s: [Int16], rate: Double) -> Data {
    var d = Data()
    func u32(_ v: UInt32) { var x = v.littleEndian; d.append(Data(bytes: &x, count: 4)) }
    func u16(_ v: UInt16) { var x = v.littleEndian; d.append(Data(bytes: &x, count: 2)) }
    let bytes = UInt32(s.count * 2), r = UInt32(rate)
    d.append("RIFF".data(using: .ascii)!); u32(36 + bytes); d.append("WAVE".data(using: .ascii)!)
    d.append("fmt ".data(using: .ascii)!); u32(16); u16(1); u16(1); u32(r); u32(r * 2); u16(2); u16(16)
    d.append("data".data(using: .ascii)!); u32(bytes)
    s.withUnsafeBufferPointer { d.append(Data(buffer: $0)) }
    return d
}

// the voice loaded now, not at the first answer: a real sentence, as "." alone left the first answer paying for the rest
// of the voice (3.5 s once under load, 2026-10-08)
_ = samples("The voice is ready, and so is the lab.")
emit(["ready": true, "voice": voice?.name ?? "", "identifier": voice?.identifier ?? "", "quality": voice?.quality.rawValue ?? 0])
while let line = readLine() {
    guard let d = line.data(using: .utf8), let j = (try? JSONSerialization.jsonObject(with: d)) as? [String: Any] else { continue }
    let id = j["id"] ?? 0
    guard let text = j["text"] as? String, let out = j["out"] as? String, !text.isEmpty else {
        emit(["id": id, "error": "text and out are needed"])
        continue
    }
    let t0 = Date()
    let (s, rate) = samples(text)
    if s.isEmpty {
        emit(["id": id, "error": "no sound was made"])
        continue
    }
    let tmp = out + ".tmp"
    do {
        try wave(s, rate: rate).write(to: URL(fileURLWithPath: tmp))
        try? FileManager.default.removeItem(atPath: out)
        try FileManager.default.moveItem(atPath: tmp, toPath: out)
        emit(["id": id, "out": out, "seconds": Double(s.count) / rate, "ms": Int(Date().timeIntervalSince(t0) * 1000)])
    } catch {
        emit(["id": id, "error": "cannot write \(out): \(error.localizedDescription)"])
    }
}
