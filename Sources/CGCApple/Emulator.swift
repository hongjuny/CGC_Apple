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
    var model: a2_model { a2_get_model(a2) }
    private(set) var romDir: URL?
    private(set) var diskURLs: [URL?] = [nil, nil]
    /// For a floppy that came from a 2IMG file, the bytes before and after the
    /// disk data (header, comment, creator chunks), written back on save.
    private var diskWrappers: [(header: Data, trailer: Data)?] = [nil, nil]
    /// Image mounted on the slot 7 hard disk.
    private(set) var hardDiskURL: URL?
    /// Pseudo drive number for the hard disk in save and discard calls.
    nonisolated static let hardDisk = 2
    private(set) var hasDiskROM = false
    private(set) var hasRamFactorROM = false
    private(set) var ramWorksEnabled = false
    var ramFactorEnabled: Bool { a2_ramfactor(a2) }
    var ramWorksActive: Bool { a2_ramworks(a2) }
    private var pasteQueue: [UInt8] = []
    private var audioScratch = [Int16](repeating: 0, count: 4096 * 2)   // stereo frames
    private var smoothedFill = Double(AudioOutput.targetFill)

    /// Mockingboard in slot 4: 0 or 1.
    var mockingboards = 1 {
        didSet { a2_set_mockingboards(a2, Int32(mockingboards)) }
    }

    // MARK: FujiNet (SmartPort in slot 5)

    /// FujiNet-PC's default "SP over SLIP" port.
    nonisolated static let fujiNetPort = 1985
    private(set) var fujiNetEnabled = false

    /// Opens or closes the slot 5 SmartPort that FujiNet-PC connects to.
    func setFujiNet(_ enabled: Bool) throws {
        guard a2_set_smartport(a2, enabled, Int32(Self.fujiNetPort)) else {
            fujiNetEnabled = false
            throw NSError(domain: "CGCApple", code: 7, userInfo: [
                NSLocalizedDescriptionKey: "Could not listen on port \(Self.fujiNetPort) for FujiNet. " +
                    "Another program (or another copy of this emulator) may be using it.",
            ])
        }
        fujiNetEnabled = enabled
    }

    /// nil when off; otherwise the number of FujiNet devices, or 0 while waiting for FujiNet-PC.
    var fujiNetDevices: Int? {
        switch a2_smartport_state(a2) {
        case 0: return nil
        case 1: return 0
        default: return Int(a2_smartport_units(a2))
        }
    }

    var fujiNetActive: Bool { a2_smartport_active(a2) }

    // MARK: Mouse card (slot 2)

    var mouseCard = false {
        didSet { a2_set_mouse(a2, mouseCard) }
    }

    /// Pointer position over the Apple II screen, 0...1 from the top left.
    func mouseCardMoved(x: Double, y: Double) {
        a2_mouse_move(a2, Float(x), Float(y))
    }

    func mouseCardButton(down: Bool) {
        a2_mouse_button(a2, down)
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
        a2 = a2_create(model)
    }

    deinit { a2_destroy(a2) }

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
        try saveAllDisks()
        let old = a2, oldHasDiskROM = hasDiskROM
        let ramFactorWasEnabled = ramFactorEnabled
        let oldHasRamFactorROM = hasRamFactorROM
        a2 = a2_create(newModel)
        do {
            try loadROMs(from: dir)
            try setRamWorks(ramWorksEnabled)
            try setRamFactor(ramFactorWasEnabled)
            for (drive, url) in diskURLs.enumerated() {
                if let url { try insertDisk(url, drive: drive) }
            }
            if let url = hardDiskURL { try mountHardDisk(url) }
        } catch {
            a2_destroy(a2)
            a2 = old
            hasDiskROM = oldHasDiskROM
            hasRamFactorROM = oldHasRamFactorROM
            throw error
        }
        a2_destroy(old)
        a2_set_monitor(a2, monitor)
        a2_set_scanlines(a2, scanlines)
        a2_set_mockingboards(a2, Int32(mockingboards))
        // The listening port moves to the new machine once the old one has closed it.
        if fujiNetEnabled { try? setFujiNet(true) }
        a2_set_mouse(a2, mouseCard)
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
        hasRamFactorROM = loadRamFactorROM(from: dir)
        romDir = dir
    }

    private func loadRamFactorROM(from dir: URL) -> Bool {
        let names = ["RamFactor.rom", "ramfactor.rom", "ae ramfactor rom v1.4.bin",
                     "ae ramfactor rom v1.3.bin", "ae ramfactor rom v1.1.bin", "ae ramfactor rom v1.0.bin"]
        for name in names {
            if let data = try? Data(contentsOf: dir.appendingPathComponent(name)),
               data.withUnsafeBytes({ a2_load_ramfactor_rom(a2, $0.baseAddress, $0.count) }) {
                return true
            }
        }
        return false
    }

    // MARK: Memory cards

    func setRamWorks(_ enabled: Bool) throws {
        // Remember the IIe preference while using a II+.
        guard a2_set_ramworks(a2, enabled && model != A2_MODEL_IIPLUS) else {
            throw NSError(domain: "CGCApple", code: 3, userInfo: [
                NSLocalizedDescriptionKey: "Could not allocate RamWorks memory.",
            ])
        }
        ramWorksEnabled = enabled
    }

    func setRamFactor(_ enabled: Bool) throws {
        if enabled && !hasRamFactorROM {
            let dir = romDir ?? URL(fileURLWithPath: FileManager.default.currentDirectoryPath)
            hasRamFactorROM = loadRamFactorROM(from: dir)
            guard hasRamFactorROM else { throw ROMError.missing("RamFactor (8 KB, RamFactor.rom)", dir) }
        }
        guard a2_set_ramfactor(a2, enabled) else {
            throw NSError(domain: "CGCApple", code: 4, userInfo: [
                NSLocalizedDescriptionKey: "Could not enable RamFactor. Slot 1 must be free and 8 MB of memory available.",
            ])
        }
    }

    // MARK: Machine control

    func powerCycle() {
        pasteQueue.removeAll()
        a2_power_cycle(a2)
    }

    func reset() {
        a2_reset(a2)
    }

    func runFrame() { runFrame(warp: false) }

    private func runFrame(warp: Bool) {
        if !pasteQueue.isEmpty && !a2_key_pending(a2) {
            a2_key_press(a2, pasteQueue.removeFirst())
        }
        a2_run_frame_ex(a2, !warp)
        let n = audioScratch.withUnsafeMutableBufferPointer {
            a2_take_audio(a2, $0.baseAddress, $0.count / 2)
        }
        if !warp { audio.push(audioScratch, frames: n) }

        guard !warp else { return }

        // Rate control: nudge the sample rate (at most ±1%, inaudible) so the
        // output queue stays near its target instead of draining or piling up.
        smoothedFill += (Double(audio.fill) - smoothedFill) * 0.05
        let target = Double(AudioOutput.targetFill)
        let ratio = 1 + max(-0.01, min(0.01, (target - smoothedFill) / target * 0.01))
        a2_set_audio_rate_adjust(a2, ratio)
    }

    /// Fast disk: while a floppy spins or the hard disk is active, run flat out. Emulated
    /// time is untouched (so loaders and copy protection behave as on real
    /// hardware); only the wall clock is skipped. Sound made meanwhile is dropped.
    var fastDisk = true

    var shouldWarp: Bool { fastDisk && (motorOn || hardDiskActive) }

    /// Runs without rendering until `deadline` or disk activity ends, then renders once.
    func runWarp(until deadline: CFTimeInterval) {
        repeat {
            runFrame(warp: true)
        } while shouldWarp && CACurrentMediaTime() < deadline
        a2_render(a2)
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
        try saveDisk(drive: drive)
        var data = try Data(contentsOf: url)
        var format: a2_disk_format
        switch url.pathExtension.lowercased() {
        case "po": format = A2_DISK_PRODOS_ORDER
        case "nib": format = A2_DISK_NIBBLE
        case "woz": format = A2_DISK_WOZ
        default: format = A2_DISK_DOS_ORDER
        }
        var writable = FileManager.default.isWritableFile(atPath: url.path)
        var wrapper: (header: Data, trailer: Data)?
        if let img = TwoIMG(header: data, fileSize: data.count) {
            // A floppy inside a 2IMG wrapper: insert the disk data, keep the rest for saving.
            format = img.format == 2 ? A2_DISK_NIBBLE : img.format == 1 ? A2_DISK_PRODOS_ORDER : A2_DISK_DOS_ORDER
            wrapper = (Data(data.prefix(img.offset)), Data(data.suffix(from: img.offset + img.length)))
            data = data.subdata(in: img.offset..<img.offset + img.length)
            writable = writable && !img.locked
        }
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
        diskWrappers[drive] = wrapper
    }

    func ejectDisk(drive: Int) throws {
        try saveDisk(drive: drive)
        a2_disk_eject(a2, Int32(drive))
        diskURLs[drive] = nil
        diskWrappers[drive] = nil
    }

    // MARK: Hard disk (slot 7)

    /// File name extensions the app opens: floppy and hard disk images alike.
    nonisolated static let imageExtensions = ["dsk", "do", "po", "nib", "woz", "hdv", "2mg", "2img", "2meg"]

    /// A 2IMG image's header fields: format (0 DOS order, 1 ProDOS order,
    /// 2 nibbles), where the data is and how long, and the locked flag.
    struct TwoIMG {
        let format: UInt32, offset: Int, length: Int, locked: Bool

        /// `header` is at least the first 64 bytes of a file `fileSize` bytes long.
        init?(header: Data, fileSize: Int) {
            guard header.count >= 64, header.prefix(4) == Data("2IMG".utf8) else { return nil }
            let h = Data(header.prefix(64))
            func u32(_ at: Int) -> UInt32 {
                UInt32(h[at]) | UInt32(h[at + 1]) << 8 | UInt32(h[at + 2]) << 16 | UInt32(h[at + 3]) << 24
            }
            format = u32(0x0C)
            offset = Int(u32(0x18))
            length = Int(u32(0x1C))
            locked = u32(0x10) & 0x8000_0000 != 0
            guard offset >= 64, offset <= fileSize, length <= fileSize - offset else { return nil }
        }
    }

    /// True for images that belong on the hard disk rather than a floppy drive,
    /// judged by content: a 2IMG image bigger than a 140K floppy, or a headerless
    /// ProDOS block image (.hdv, .2meg, a large .po...) bigger than a floppy.
    nonisolated static func isHardDiskImage(_ url: URL) -> Bool {
        guard let handle = try? FileHandle(forReadingFrom: url) else { return false }
        defer { try? handle.close() }
        let head = (try? handle.read(upToCount: 64)) ?? Data()
        let size = Int((try? handle.seekToEnd()) ?? 0)
        if let img = TwoIMG(header: head, fileSize: size) {
            return img.format == 1 && img.length > Int(A2_DSK_SIZE)
        }
        if head.prefix(4) == Data("2IMG".utf8) { return false }
        switch url.pathExtension.lowercased() {
        case "dsk", "do", "nib", "woz": return false
        case "hdv", "2meg": return size > 0 && size % 512 == 0      // hard disk names, any size
        default: return size > Int(A2_DSK_SIZE) && size % 512 == 0
        }
    }

    func mountHardDisk(_ url: URL) throws {
        try saveDisk(drive: Self.hardDisk)
        let data = try Data(contentsOf: url)
        let writable = FileManager.default.isWritableFile(atPath: url.path)
        let ok = data.withUnsafeBytes { a2_hd_insert(a2, $0.baseAddress, $0.count, !writable) }
        guard ok else {
            throw NSError(domain: "CGCApple", code: 5, userInfo: [
                NSLocalizedDescriptionKey: "\(url.lastPathComponent): not a ProDOS-order hard disk image " +
                    "(.hdv/.po up to 32 MB, or a ProDOS-order .2mg)",
            ])
        }
        hardDiskURL = url
    }

    func unmountHardDisk() throws {
        try saveDisk(drive: Self.hardDisk)
        a2_hd_eject(a2)
        hardDiskURL = nil
    }

    var hardDiskActive: Bool { a2_hd_active(a2) }

    private func saveHardDisk() throws {
        guard let url = hardDiskURL, a2_hd_dirty(a2) else { return }
        var buf = [UInt8](repeating: 0, count: a2_hd_export_size(a2))
        let n = buf.withUnsafeMutableBufferPointer { a2_hd_export(a2, $0.baseAddress, $0.count) }
        guard n > 0 else {
            throw NSError(domain: "CGCApple", code: 6, userInfo: [
                NSLocalizedDescriptionKey: "Could not export \(url.lastPathComponent). The hard disk remains mounted.",
            ])
        }
        try Data(buf[0..<n]).write(to: url, options: .atomic)
        a2_hd_mark_clean(a2)
    }

    // MARK: Saving

    /// Writes a modified disk (drive 0, 1 or `Emulator.hardDisk`) back to its file.
    func saveDisk(drive: Int) throws {
        if drive == Self.hardDisk { return try saveHardDisk() }
        guard let url = diskURLs[drive], a2_disk_dirty(a2, Int32(drive)) else { return }
        var buf = [UInt8](repeating: 0, count: a2_disk_export_size(a2, Int32(drive)))
        let n = buf.withUnsafeMutableBufferPointer {
            a2_disk_export(a2, Int32(drive), $0.baseAddress, $0.count)
        }
        guard n > 0 else {
            throw NSError(domain: "CGCApple", code: 2, userInfo: [
                NSLocalizedDescriptionKey: "Could not export \(url.lastPathComponent). The disk remains inserted.",
            ])
        }
        let wrapper = diskWrappers[drive]
        try ((wrapper?.header ?? Data()) + Data(buf[0..<n]) + (wrapper?.trailer ?? Data())).write(to: url, options: .atomic)
        a2_disk_mark_clean(a2, Int32(drive))
    }

    /// Forgets unsaved writes so the disk can be ejected or the app closed without saving.
    func discardDiskChanges(drive: Int) {
        if drive == Self.hardDisk {
            a2_hd_mark_clean(a2)
        } else {
            a2_disk_mark_clean(a2, Int32(drive))
        }
    }

    func saveAllDisks() throws {
        try saveDisk(drive: 0)
        try saveDisk(drive: 1)
        try saveDisk(drive: Self.hardDisk)
    }
}
