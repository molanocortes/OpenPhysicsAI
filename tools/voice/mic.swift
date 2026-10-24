// mic.swift - a question spoken at the Mac (GOALS.md G18): the microphone until the window stops it (SIGINT or SIGTERM),
// turned into text with the Mac's own speech recognition, on the device when the language allows (nothing is sent to a
// cloud service then). Used by the agentic bar through src/voice.c.
//     build/openphysics-mic [en-US|de-DE|...]      prints the words when stopped, or fails with a reason on stderr
import AVFoundation
import Foundation
import Speech

func fail(_ msg: String) -> Never {
    FileHandle.standardError.write((msg + "\n").data(using: .utf8)!)
    exit(1)
}

let args = CommandLine.arguments
let locale = Locale(identifier: args.count > 1 ? args[1] : (Locale.preferredLanguages.first ?? "en-US"))

var speech = SFSpeechRecognizer.authorizationStatus()
if speech == .notDetermined {
    let done = DispatchSemaphore(value: 0)
    SFSpeechRecognizer.requestAuthorization { s in speech = s; done.signal() }
    _ = done.wait(timeout: .now() + 60)
}
if speech != .authorized { fail("speech recognition is not allowed: System Settings > Privacy & Security > Speech Recognition") }
switch AVCaptureDevice.authorizationStatus(for: .audio) {
case .authorized: break
case .notDetermined:
    let done = DispatchSemaphore(value: 0)
    var granted = false
    AVCaptureDevice.requestAccess(for: .audio) { g in granted = g; done.signal() }
    _ = done.wait(timeout: .now() + 60)
    if !granted { fail("the microphone is not allowed: System Settings > Privacy & Security > Microphone") }
default: fail("the microphone is not allowed: System Settings > Privacy & Security > Microphone")
}
guard let rec = SFSpeechRecognizer(locale: locale), rec.isAvailable else { fail("no speech recognition for \(locale.identifier)") }

let req = SFSpeechAudioBufferRecognitionRequest()
req.shouldReportPartialResults = true
if rec.supportsOnDeviceRecognition { req.requiresOnDeviceRecognition = true }
let engine = AVAudioEngine()
let input = engine.inputNode
input.installTap(onBus: 0, bufferSize: 1024, format: input.outputFormat(forBus: 0)) { buffer, _ in req.append(buffer) }
engine.prepare()
do { try engine.start() } catch { fail("the microphone could not start: \(error.localizedDescription)") }

var heard = ""
let task = rec.recognitionTask(with: req) { result, error in
    if let r = result {
        heard = r.bestTranscription.formattedString
        if r.isFinal { print(heard); exit(0) }
    }
    if error != nil {
        if !heard.isEmpty { print(heard); exit(0) }
        fail("no words were heard")
    }
}
var stopped = false
func stop() {
    if stopped { return }
    stopped = true
    engine.stop()
    input.removeTap(onBus: 0)
    req.endAudio()
}
signal(SIGINT, SIG_IGN)
signal(SIGTERM, SIG_IGN)
let onInt = DispatchSource.makeSignalSource(signal: SIGINT, queue: .main)
onInt.setEventHandler { stop() }
onInt.resume()
let onTerm = DispatchSource.makeSignalSource(signal: SIGTERM, queue: .main)
onTerm.setEventHandler { stop() }
onTerm.resume()
DispatchQueue.main.asyncAfter(deadline: .now() + 60) { stop() } /* a minute of listening at most */
DispatchQueue.main.asyncAfter(deadline: .now() + 75) {
    if !heard.isEmpty { print(heard); exit(0) }
    fail("no words were heard")
}
_ = task
RunLoop.main.run()
