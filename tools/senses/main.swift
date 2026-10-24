// CubeSenses: the Claude Cube's ears on the Mac. No windows (LSUIElement).
// While a Claude app is recording his voice message, runs tools/cube_viz.py on the same mic
// (as a child, so the mic access is this app's): the cube shows LISTENING with its bars.
// Otherwise the microphone is left alone. Being a real app is what lets macOS ask for it.
import AVFoundation
import CoreAudio
import Foundation

// CubeSenses.app lives in tools/, next to cube_viz.py
let tools = URL(fileURLWithPath: Bundle.main.bundlePath).deletingLastPathComponent().path

func cubeHost() -> String {
    let path = NSHomeDirectory() + "/.claude/cube_host"
    let h = (try? String(contentsOfFile: path, encoding: .utf8))?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
    return h.isEmpty ? "claudecube.local" : h
}

func log(_ s: String) {
    let line = "\(Date()) \(s)\n"
    if let h = FileHandle(forWritingAtPath: "/tmp/claudecube-eyes.log") {
        h.seekToEndOfFile(); h.write(line.data(using: .utf8)!); h.closeFile()
    } else {
        FileManager.default.createFile(atPath: "/tmp/claudecube-eyes.log", contents: line.data(using: .utf8))
    }
}

// --- microphone visualizer, only while he dictates a voice message to Claude ---
// CoreAudio lists which processes are capturing audio right now. While a Claude app is,
// run tools/cube_viz.py on the same default input; stop it the moment Claude stops.
func prop<T>(_ obj: AudioObjectID, _ sel: AudioObjectPropertySelector, _ value: inout T) -> Bool {
    var addr = AudioObjectPropertyAddress(mSelector: sel, mScope: kAudioObjectPropertyScopeGlobal, mElement: kAudioObjectPropertyElementMain)
    var size = UInt32(MemoryLayout<T>.size)
    return AudioObjectGetPropertyData(obj, &addr, 0, nil, &size, &value) == noErr
}

// bundle ids of every process capturing audio right now (logged when it changes, to see who records)
func recorders() -> [String] {
    var addr = AudioObjectPropertyAddress(mSelector: kAudioHardwarePropertyProcessObjectList,
                                          mScope: kAudioObjectPropertyScopeGlobal, mElement: kAudioObjectPropertyElementMain)
    var size: UInt32 = 0
    guard AudioObjectGetPropertyDataSize(AudioObjectID(kAudioObjectSystemObject), &addr, 0, nil, &size) == noErr else { return [] }
    var ids = [AudioObjectID](repeating: 0, count: Int(size) / MemoryLayout<AudioObjectID>.size)
    guard AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &addr, 0, nil, &size, &ids) == noErr else { return [] }
    var out: [String] = []
    for id in ids {
        var running: UInt32 = 0
        guard prop(id, kAudioProcessPropertyIsRunningInput, &running), running != 0 else { continue }
        var bundle: CFString = "" as CFString
        var pid: pid_t = 0
        _ = prop(id, kAudioProcessPropertyPID, &pid)
        out.append(prop(id, kAudioProcessPropertyBundleID, &bundle) && !(bundle as String).isEmpty ? bundle as String : "pid \(pid)")
    }
    return out.sorted()
}

var lastRecorders: [String] = []
func claudeIsRecording() -> Bool {
    let r = recorders().filter { !$0.lowercased().contains("cubesenses") && !$0.lowercased().contains("python") }
    if r != lastRecorders { log("recording now: \(r)"); lastRecorders = r }
    return r.contains { $0.lowercased().contains("anthropic") || $0.lowercased().contains("claude") }
}

func defaultInputName() -> String {
    var dev = AudioObjectID(0)
    guard prop(AudioObjectID(kAudioObjectSystemObject), kAudioHardwarePropertyDefaultInputDevice, &dev) else { return "" }
    var name: CFString = "" as CFString
    return prop(dev, kAudioObjectPropertyName, &name) ? name as String : ""
}

var viz: Process?
func watchDictation() {
    Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { _ in
        let rec = claudeIsRecording()
        if rec && viz == nil {
            let p = Process()
            p.executableURL = URL(fileURLWithPath: "/usr/bin/python3")
            p.arguments = [tools + "/cube_viz.py"]
            var env = ProcessInfo.processInfo.environment
            env["CUBE_MIC"] = defaultInputName()  // the mic Claude is listening to
            env["CUBE_VIZ_ALWAYS"] = "1"          // he is talking: no loudness gate
            p.environment = env
            p.standardError = FileHandle(forWritingAtPath: "/tmp/claudecube-viz.log") ?? FileHandle.nullDevice
            try? p.run()
            viz = p
            log("dictation started, visualizer on \(env["CUBE_MIC"] ?? "")")
        } else if !rec, let p = viz {
            p.terminate()
            viz = nil
            log("dictation stopped, visualizer off")
        }
    }
}

// microphone only: asked once, and only touched while Claude records his voice
AVCaptureDevice.requestAccess(for: .audio) { ok in
    log("microphone access: \(ok)")
    if ok { DispatchQueue.main.async { watchDictation() } }
}
RunLoop.main.run()
