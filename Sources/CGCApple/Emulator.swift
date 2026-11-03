import Foundation
import QuartzCore
import A2Core

/// Swift wrapper around the C core: owns the machine, ROM/disk loading,
/// the paste queue and the audio hand-off.
@MainActor
final class Emulator {
    static let frameRate = 1_020_484.0 / 17_030.0   // ~59.92 Hz

    private var a2: OpaquePointer
    let audio = AudioOutput()
    private(set) var model: a2_model
    private var romDir: URL?
    private(set) var diskURLs: [URL?] = [nil, nil]
    private(set) var hasDiskROM = false
    private var pasteQueue: [UInt8] = []
    private var audioScratch = [Int16](repeating: 0, count: 4096 * 2)   // stereo frames
    private var smoothedFill = Double(AudioOutput.targetFill)

    /// Mockingboards installed: 0, 1 (slot 4) or 2 (slots 4 and 5).
    var mockingboards = 1 {
        didSet {
            a2_set_mockingboards(a2, Int32(mockingboards))
            if midiSlot >= 4 && midiSlot - 4 < mockingboards { setMIDISlot(0) }
        }
    }

    private lazy var midi = MIDIPlayer(synth: audio.midiSynth)
    private var midiScratch = [UInt8](repeating: 0, count: 4096)
    /// Slot of the Passport MIDI card, 0 = none.
    private(set) var midiSlot = 0

    @discardableResult
    func setMIDISlot(_ slot: Int) -> Int {
        midiSlot = Int(a2_set_midi_slot(a2, Int32(slot)))
        midi.panic()
        return midiSlot
    }

    /// IIe Caps Lock: letters are sent upper-case. CALL3327 and most period
    /// software only understand upper-case input.
    var capsLock = true

    var monitor: a2_monitor = A2_MONITOR_COLOR {
        didSet { a2_set_monitor(a2, monitor) }
    }
    var scanlines = false {
        didSet { a2_set_scanlines(a2, scanlines) }
    }

    init(model: a2_model) {
        self.model = model
        a2 = a2_create(model)
    }

    static func name(of model: a2_model) -> String {
        switch model {
        case A2_MODEL_IIE: return "Apple //e"
        case A2_MODEL_IIE_ENHANCED: return "Apple //e Enhanced"
        default: return "Apple ][+"
        }
    }

    /// Replaces the machine with another model, keeping disks and display settings.
    func switchModel(to newModel: a2_model) throws {
        guard newModel != model, let dir = romDir else { return }
        saveAllDisks()
        let old = a2, oldModel = model
        a2 = a2_create(newModel)
        model = newModel
        do {
            try loadROMs(from: dir)
        } catch {
            a2_destroy(a2)
            a2 = old
            model = oldModel
            throw error
        }
        a2_destroy(old)
        a2_set_monitor(a2, monitor)
        a2_set_scanlines(a2, scanlines)
        a2_set_mockingboards(a2, Int32(mockingboards))
        a2_set_midi_slot(a2, Int32(midiSlot))
        for (drive, url) in diskURLs.enumerated() {
            if let url { try? insertDisk(url, drive: drive) }
        }
        powerCycle()
    }

    // MARK: ROMs

    enum ROMError: LocalizedError {
        case missing(String, URL)
        var errorDescription: String? {
            switch self {
            case let .missing(what, dir): return "\(what) ROM not found in \(dir.path)"
            }
        }
    }

    /// ROM file names per model: AppleWin names first, then the split MAME set.
    private struct ROMSet {
        let systemNames: [String]
        let systemParts: [String]
        let charNames: [String]
        let systemSize: String
    }

    private static func romSet(for model: a2_model) -> ROMSet {
        switch model {
        case A2_MODEL_IIE:
            return ROMSet(systemNames: ["Apple2e.rom"],
                          systemParts: ["342-0135-b.64", "342-0134-a.64"],
                          charNames: ["Apple2e_Video.rom", "342-0133-a.chr", "Apple2e_Enhanced_Video.rom", "342-0265-a.chr"],
                          systemSize: "Apple //e (16 KB)")
        case A2_MODEL_IIE_ENHANCED:
            return ROMSet(systemNames: ["Apple2e_Enhanced.rom"],
                          systemParts: ["342-0304-a.e10", "342-0303-a.e8"],
                          charNames: ["Apple2e_Enhanced_Video.rom", "342-0265-a.chr"],
                          systemSize: "Apple //e Enhanced (16 KB)")
        default:
            return ROMSet(systemNames: ["Apple2_Plus.rom", "apple2plus.rom", "APPLE2P.ROM"],
                          systemParts: ["341-0011.d0", "341-0012.d8", "341-0013.e0", "341-0014.e8", "341-0015.f0", "341-0020-00.f8"],
                          charNames: ["Apple2_Video.rom", "341-0036.chr"],
                          systemSize: "Apple ][+ (12 KB)")
        }
    }

    /// Loads the current model's ROMs from a folder (AppleWin or MAME file names).
    func loadROMs(from dir: URL) throws {
        func read(_ names: [String]) -> Data? {
            for name in names {
                if let d = try? Data(contentsOf: dir.appendingPathComponent(name)) { return d }
            }
            return nil
        }

        let set = Self.romSet(for: model)
        var system = read(set.systemNames)
        if system == nil {
            let chunks = set.systemParts.compactMap { read([$0]) }
            if chunks.count == set.systemParts.count { system = chunks.reduce(Data(), +) }
        }
        guard let system, system.withUnsafeBytes({ a2_load_system_rom(a2, $0.baseAddress, $0.count) }) else {
            throw ROMError.missing("System \(set.systemSize)", dir)
        }
        guard let chars = read(set.charNames),
              chars.withUnsafeBytes({ a2_load_char_rom(a2, $0.baseAddress, $0.count) }) else {
            throw ROMError.missing("Character", dir)
        }
        hasDiskROM = false
        if let disk = read(["DISK2.rom", "341-0027-a.p5", "341-0027.bin"]) {
            hasDiskROM = disk.withUnsafeBytes { a2_load_disk_rom(a2, $0.baseAddress, $0.count) }
        }
        romDir = dir
    }

    // MARK: Machine control

    func powerCycle() {
        pasteQueue.removeAll()
        a2_power_cycle(a2)
        midi.panic()
    }

    func reset() {
        a2_reset(a2)
        midi.panic()
    }

    func runFrame() {
        if !pasteQueue.isEmpty && !a2_key_pending(a2) {
            a2_key_press(a2, pasteQueue.removeFirst())
        }
        a2_run_frame(a2)
        let n = audioScratch.withUnsafeMutableBufferPointer {
            a2_take_audio(a2, $0.baseAddress, $0.count / 2)
        }
        audio.push(audioScratch, frames: n)

        if midiSlot > 0 {
            let m = midiScratch.withUnsafeMutableBufferPointer {
                a2_take_midi(a2, $0.baseAddress, $0.count)
            }
            if m > 0 { midi.feed(midiScratch[0..<m]) }
        }

        // Rate control: nudge the sample rate (at most ±1%, inaudible) so the
        // output queue stays near its target instead of draining or piling up.
        smoothedFill += (Double(audio.fill) - smoothedFill) * 0.05
        let target = Double(AudioOutput.targetFill)
        let ratio = 1 + max(-0.01, min(0.01, (target - smoothedFill) / target * 0.01))
        a2_set_audio_rate_adjust(a2, ratio)
    }

    /// Fast disk: while a drive is spinning, run the machine flat out. Emulated
    /// time is untouched (so loaders and copy protection behave as on real
    /// hardware); only the wall clock is skipped. Sound made meanwhile is dropped.
    var fastDisk = true

    var shouldWarp: Bool { fastDisk && motorOn }

    /// Runs frames without rendering until `deadline` or the drives stop; returns the count.
    func runWarp(until deadline: CFTimeInterval) -> Int {
        var frames = 0
        repeat {
            if !pasteQueue.isEmpty && !a2_key_pending(a2) {
                a2_key_press(a2, pasteQueue.removeFirst())
            }
            a2_run_frame_ex(a2, false)
            frames += 1
            audioScratch.withUnsafeMutableBufferPointer {
                _ = a2_take_audio(a2, $0.baseAddress, $0.count / 2)   // discard
            }
            if midiSlot > 0 {
                let m = midiScratch.withUnsafeMutableBufferPointer {
                    a2_take_midi(a2, $0.baseAddress, $0.count)
                }
                if m > 0 { midi.feed(midiScratch[0..<m]) }
            }
        } while motorOn && CACurrentMediaTime() < deadline
        a2_render(a2)
        return frames
    }

    func withFramebuffer<T>(_ body: (UnsafePointer<UInt32>) -> T) -> T {
        body(a2_framebuffer(a2))
    }

    // MARK: Input

    private func applyCapsLock(_ ascii: UInt8) -> UInt8 {
        capsLock && ascii >= 0x61 && ascii <= 0x7A ? ascii - 0x20 : ascii
    }

    func key(_ ascii: UInt8) {
        pasteQueue.removeAll()
        a2_key_press(a2, applyCapsLock(ascii))
    }

    func keyUp() {
        a2_key_release(a2)
    }

    func paste(_ text: String) {
        for scalar in text.unicodeScalars where scalar.isASCII {
            var v = UInt8(scalar.value)
            if v == 0x0A { v = 0x0D }
            if v >= 0x20 || v == 0x0D { pasteQueue.append(applyCapsLock(v)) }
        }
    }

    func setButton(_ n: Int32, down: Bool) {
        a2_set_button(a2, n, down)
    }

    func setPaddle(_ n: Int32, _ value: UInt8) {
        a2_set_paddle(a2, n, value)
    }

    // MARK: Disks

    var motorOn: Bool { a2_disk_motor_on(a2) }
    var activeDrive: Int { Int(a2_disk_active_drive(a2)) }

    func insertDisk(_ url: URL, drive: Int) throws {
        saveDisk(drive: drive)
        let data = try Data(contentsOf: url)
        let format: a2_disk_format
        switch url.pathExtension.lowercased() {
        case "po": format = A2_DISK_PRODOS_ORDER
        case "nib": format = A2_DISK_NIBBLE
        case "woz": format = A2_DISK_WOZ
        default: format = A2_DISK_DOS_ORDER
        }
        let writable = FileManager.default.isWritableFile(atPath: url.path)
        let ok = data.withUnsafeBytes {
            a2_disk_insert(a2, Int32(drive), $0.baseAddress, $0.count, format, !writable)
        }
        guard ok else {
            throw NSError(domain: "CGCApple", code: 1, userInfo: [
                NSLocalizedDescriptionKey: format == A2_DISK_WOZ
                    ? "\(url.lastPathComponent): not a valid 5.25\" WOZ 1/2 image (or its CRC does not match)"
                    : "\(url.lastPathComponent): unsupported size \(data.count) bytes " +
                      "(expected \(A2_DSK_SIZE) for .dsk/.do/.po or \(A2_NIB_SIZE) for .nib)",
            ])
        }
        diskURLs[drive] = url
    }

    func ejectDisk(drive: Int) {
        saveDisk(drive: drive)
        a2_disk_eject(a2, Int32(drive))
        diskURLs[drive] = nil
    }

    /// Writes a modified disk back to its file.
    func saveDisk(drive: Int) {
        guard let url = diskURLs[drive], a2_disk_dirty(a2, Int32(drive)) else { return }
        var buf = [UInt8](repeating: 0, count: a2_disk_export_size(a2, Int32(drive)))
        let n = buf.withUnsafeMutableBufferPointer {
            a2_disk_export(a2, Int32(drive), $0.baseAddress, $0.count)
        }
        if n > 0 {
            try? Data(buf[0..<n]).write(to: url, options: .atomic)
        }
    }

    func saveAllDisks() {
        saveDisk(drive: 0)
        saveDisk(drive: 1)
    }
}
