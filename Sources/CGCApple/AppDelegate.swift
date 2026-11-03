import AppKit
import A2Core
import UniformTypeIdentifiers

@MainActor
final class AppDelegate: NSObject, NSApplicationDelegate {
    private let emulator = Emulator(model: AppDelegate.savedModel())
    private lazy var joystick = Joystick(emulator: emulator)
    private var window: NSWindow!
    private var screen: ScreenView!
    private var timer: Timer?
    private var startTime = CACurrentMediaTime()
    private var framesRun = 0.0
    private var lastStatus = ""
    private var paused = false
    private var activity: NSObjectProtocol?
    private var loadedROMDir: URL?

    private static let romDirKey = "romDirectory"
    private static let modelKey = "model"
    private static let mockingboardCountKey = "mockingboardCount"
    private static let midiSlotKey = "midiSlot"
    private static let realDiskSpeedKey = "realDiskSpeed"
    private static let joystickKey = "joystickMode"
    private static let passportSlot = 2

    private static func savedModel() -> a2_model {
        let raw = UInt32(UserDefaults.standard.integer(forKey: modelKey))
        return raw <= A2_MODEL_IIE_ENHANCED.rawValue ? a2_model(rawValue: raw) : A2_MODEL_IIPLUS
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        buildMenus()

        screen = ScreenView(emulator: emulator)
        screen.joystick = joystick
        screen.onDiskDropped = { [weak self] url, drive in self?.insert(url, drive: drive) }

        window = NSWindow(contentRect: screen.frame, styleMask: [.titled, .closable, .miniaturizable, .resizable],
                          backing: .buffered, defer: false)
        window.title = "CGC " + Emulator.name(of: emulator.model)
        window.contentView = screen
        window.contentAspectRatio = screen.frame.size
        window.center()
        window.makeKeyAndOrderFront(nil)
        window.makeFirstResponder(screen)
        window.acceptsMouseMovedEvents = true

        guard loadROMs() else {
            NSApp.terminate(nil)
            return
        }
        emulator.powerCycle()

        // Disk images passed on the command line go into drives 1 and 2.
        let diskArgs = CommandLine.arguments.dropFirst()
            .filter { ["dsk", "do", "po", "nib", "woz"].contains(($0 as NSString).pathExtension.lowercased()) }
        for (i, path) in diskArgs.prefix(2).enumerated() {
            insert(URL(fileURLWithPath: path), drive: i)
        }
        if diskArgs.isEmpty, let disk = defaultDisk() {
            insert(disk, drive: 0)
        }

        startClock()
        NSApp.activate(ignoringOtherApps: true)
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }

    func applicationWillTerminate(_ notification: Notification) {
        emulator.saveAllDisks()
    }

    func application(_ application: NSApplication, open urls: [URL]) {
        for (i, url) in urls.prefix(2).enumerated() { insert(url, drive: i) }
    }

    // MARK: ROM discovery

    /// The CALL3327 Hangul disk, inserted at launch when no disk is given. Disk
    /// images are not shipped with the app; the first copy found is placed in
    /// Application Support and used from there, so the source file is never written.
    private static let defaultDiskName = "call3327.dsk"

    private func defaultDisk() -> URL? {
        let fm = FileManager.default
        let support = fm.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("CGCApple/disks")
        let working = support.appendingPathComponent(Self.defaultDiskName)
        if fm.fileExists(atPath: working.path) {
            return working
        }
        var dirs: [URL] = []
        if let env = ProcessInfo.processInfo.environment["CGCAPPLE_DISKS"] {
            dirs.append(URL(fileURLWithPath: env))
        }
        if let res = Bundle.main.resourceURL {
            dirs.append(res.appendingPathComponent("disks"))
        }
        if let romDir = loadedROMDir {
            dirs.append(romDir.deletingLastPathComponent().appendingPathComponent("disks"))
        }
        dirs.append(URL(fileURLWithPath: fm.currentDirectoryPath).appendingPathComponent("disks"))
        for dir in dirs {
            let source = dir.appendingPathComponent(Self.defaultDiskName)
            guard fm.fileExists(atPath: source.path) else { continue }
            try? fm.createDirectory(at: support, withIntermediateDirectories: true)
            return (try? fm.copyItem(at: source, to: working)) != nil ? working : nil
        }
        return nil
    }

    private func romCandidates() -> [URL] {
        var dirs: [URL] = []
        if let env = ProcessInfo.processInfo.environment["CGCAPPLE_ROMS"] {
            dirs.append(URL(fileURLWithPath: env))
        }
        if let saved = UserDefaults.standard.string(forKey: Self.romDirKey) {
            dirs.append(URL(fileURLWithPath: saved))
        }
        if let res = Bundle.main.resourceURL {
            dirs.append(res.appendingPathComponent("roms"))
        }
        let support = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        dirs.append(support.appendingPathComponent("CGCApple/roms"))
        dirs.append(URL(fileURLWithPath: FileManager.default.currentDirectoryPath).appendingPathComponent("roms"))
        return dirs
    }

    private func loadROMs() -> Bool {
        for dir in romCandidates() where (try? emulator.loadROMs(from: dir)) != nil {
            loadedROMDir = dir
            warnIfNoDiskROM(dir)
            return true
        }
        // Ask for the folder until valid ROMs are picked or the user cancels.
        while true {
            let panel = NSOpenPanel()
            panel.message = "Choose the folder with the Apple II ROMs (Apple2_Plus.rom, Apple2_Video.rom, DISK2.rom)"
            panel.canChooseDirectories = true
            panel.canChooseFiles = false
            guard panel.runModal() == .OK, let dir = panel.url else { return false }
            do {
                try emulator.loadROMs(from: dir)
                loadedROMDir = dir
                UserDefaults.standard.set(dir.path, forKey: Self.romDirKey)
                warnIfNoDiskROM(dir)
                return true
            } catch {
                NSAlert(error: error).runModal()
            }
        }
    }

    private func warnIfNoDiskROM(_ dir: URL) {
        guard !emulator.hasDiskROM else { return }
        let alert = NSAlert()
        alert.messageText = "Disk II ROM not found"
        alert.informativeText = "DISK2.rom (341-0027, 256 bytes) is missing from \(dir.path), so disks cannot boot. BASIC still works."
        alert.runModal()
    }

    // MARK: Frame clock

    private func startClock() {
        startTime = CACurrentMediaTime()
        framesRun = 0
        // Keep App Nap and timer coalescing from starving the emulation (and the audio queue)
        // when the window is not frontmost.
        activity = ProcessInfo.processInfo.beginActivity(options: [.userInitiated, .latencyCritical],
                                                         reason: "Apple II emulation")
        let t = Timer(timeInterval: 1.0 / 120.0, repeats: true) { [weak self] _ in
            MainActor.assumeIsolated { self?.tick() }
        }
        t.tolerance = 0
        RunLoop.main.add(t, forMode: .common)
        timer = t
    }

    private func tick() {
        guard !paused else { return }
        joystick.poll()
        if emulator.shouldWarp {
            // Spend most of this timer period emulating, then resync the frame clock.
            _ = emulator.runWarp(until: CACurrentMediaTime() + 0.007)
            startTime = CACurrentMediaTime()
            framesRun = 0
            screen.refresh()
            updateStatus()
            return
        }
        let due = (CACurrentMediaTime() - startTime) * Emulator.frameRate
        var ran = 0
        while framesRun < due && ran < 4 {
            emulator.runFrame()
            framesRun += 1
            ran += 1
        }
        if due - framesRun > 8 {
            framesRun = due   // fell far behind (e.g. app was suspended): resync
        }
        if ran > 0 {
            screen.refresh()
            updateStatus()
        }
    }

    private func updateStatus() {
        let names = emulator.diskURLs.map { $0?.lastPathComponent ?? "—" }
        var status = "D1: \(names[0])   D2: \(names[1])"
        if emulator.motorOn { status += "   ● D\(emulator.activeDrive + 1)" }
        if status != lastStatus {
            window.subtitle = status
            lastStatus = status
        }
    }

    // MARK: Actions

    private func insert(_ url: URL, drive: Int) {
        do {
            try emulator.insertDisk(url, drive: drive)
            NSDocumentController.shared.noteNewRecentDocumentURL(url)
        } catch {
            NSAlert(error: error).runModal()
        }
    }

    private func chooseDisk(drive: Int) {
        let panel = NSOpenPanel()
        panel.message = "Disk image for drive \(drive + 1)"
        panel.allowedContentTypes = ["dsk", "do", "po", "nib", "woz"].compactMap { UTType(filenameExtension: $0) }
        panel.allowsOtherFileTypes = true
        if panel.runModal() == .OK, let url = panel.url {
            insert(url, drive: drive)
        }
    }

    @objc private func insertDisk1() { chooseDisk(drive: 0) }
    @objc private func insertDisk2() { chooseDisk(drive: 1) }
    @objc private func ejectDisk1() { emulator.ejectDisk(drive: 0) }
    @objc private func ejectDisk2() { emulator.ejectDisk(drive: 1) }
    @objc private func reset() { emulator.reset() }
    @objc private func powerCycle() { emulator.saveAllDisks(); emulator.powerCycle() }

    @objc private func togglePause(_ sender: NSMenuItem) {
        paused.toggle()
        sender.state = paused ? .on : .off
        if !paused { startTime = CACurrentMediaTime(); framesRun = 0 }
    }

    @objc private func setMonitor(_ sender: NSMenuItem) {
        emulator.monitor = a2_monitor(rawValue: UInt32(sender.tag))
        sender.menu?.items.forEach { if $0.action == #selector(setMonitor(_:)) { $0.state = $0 == sender ? .on : .off } }
    }

    @objc private func selectModel(_ sender: NSMenuItem) {
        let model = a2_model(rawValue: UInt32(sender.tag))
        do {
            try emulator.switchModel(to: model)
        } catch {
            NSAlert(error: error).runModal()
            return
        }
        UserDefaults.standard.set(sender.tag, forKey: Self.modelKey)
        sender.menu?.items.forEach { if $0.action == #selector(selectModel(_:)) { $0.state = $0 == sender ? .on : .off } }
        window.title = "CGC " + Emulator.name(of: model)
        warnIfNoDiskROMForCurrentModel()
    }

    private func warnIfNoDiskROMForCurrentModel() {
        if let dir = UserDefaults.standard.string(forKey: Self.romDirKey) {
            warnIfNoDiskROM(URL(fileURLWithPath: dir))
        }
    }

    @objc private func toggleCapsLock(_ sender: NSMenuItem) {
        emulator.capsLock.toggle()
        sender.state = emulator.capsLock ? .on : .off
    }

    @objc private func togglePassportMIDI(_ sender: NSMenuItem) {
        let slot = emulator.setMIDISlot(emulator.midiSlot == 0 ? Self.passportSlot : 0)
        UserDefaults.standard.set(slot, forKey: Self.midiSlotKey)
        sender.state = slot != 0 ? .on : .off
    }

    @objc private func selectMockingboards(_ sender: NSMenuItem) {
        emulator.mockingboards = sender.tag
        UserDefaults.standard.set(sender.tag, forKey: Self.mockingboardCountKey)
        sender.menu?.items.forEach {
            if $0.action == #selector(selectMockingboards(_:)) { $0.state = $0 == sender ? .on : .off }
        }
    }

    @objc private func selectJoystick(_ sender: NSMenuItem) {
        joystick.mode = Joystick.Mode(rawValue: sender.tag) ?? .keyboard
        UserDefaults.standard.set(sender.tag, forKey: Self.joystickKey)
        sender.menu?.items.forEach { $0.state = $0 == sender ? .on : .off }
    }

    @objc private func toggleFastDisk(_ sender: NSMenuItem) {
        emulator.fastDisk.toggle()
        UserDefaults.standard.set(!emulator.fastDisk, forKey: Self.realDiskSpeedKey)
        sender.state = emulator.fastDisk ? .on : .off
    }

    @objc private func toggleScanlines(_ sender: NSMenuItem) {
        emulator.scanlines.toggle()
        sender.state = emulator.scanlines ? .on : .off
    }

    @objc private func toggleMute(_ sender: NSMenuItem) {
        emulator.audio.muted.toggle()
        sender.state = emulator.audio.muted ? .on : .off
    }

    @objc private func paste(_ sender: Any?) {
        if let text = NSPasteboard.general.string(forType: .string) {
            emulator.paste(text)
        }
    }

    // MARK: Menus

    private func fastDiskItem() -> NSMenuItem {
        emulator.fastDisk = !UserDefaults.standard.bool(forKey: Self.realDiskSpeedKey)
        let i = NSMenuItem(title: "Fast Disk (Full Speed While a Drive Spins)", action: #selector(toggleFastDisk(_:)),
                           keyEquivalent: "f")
        i.keyEquivalentModifierMask = [.command, .shift]
        i.state = emulator.fastDisk ? .on : .off
        return i
    }

    private func buildMenus() {
        let main = NSMenu()

        func submenu(_ title: String, _ items: [NSMenuItem]) {
            let item = NSMenuItem(title: title, action: nil, keyEquivalent: "")
            let menu = NSMenu(title: title)
            items.forEach(menu.addItem)
            item.submenu = menu
            main.addItem(item)
        }
        func item(_ title: String, _ action: Selector?, _ key: String = "",
                  _ mods: NSEvent.ModifierFlags = .command, tag: Int = 0) -> NSMenuItem {
            let i = NSMenuItem(title: title, action: action, keyEquivalent: key)
            i.keyEquivalentModifierMask = mods
            i.tag = tag
            return i
        }

        submenu("CGC Apple", [
            item("About CGC Apple", #selector(NSApplication.orderFrontStandardAboutPanel(_:))),
            .separator(),
            item("Hide CGC Apple", #selector(NSApplication.hide(_:)), "h"),
            item("Quit CGC Apple", #selector(NSApplication.terminate(_:)), "q"),
        ])
        submenu("Disk", [
            item("Insert in Drive 1…", #selector(insertDisk1), "o"),
            item("Insert in Drive 2…", #selector(insertDisk2), "o", [.command, .shift]),
            .separator(),
            item("Eject Drive 1", #selector(ejectDisk1), "e"),
            item("Eject Drive 2", #selector(ejectDisk2), "e", [.command, .shift]),
        ])
        submenu("Edit", [
            item("Paste (Type In)", #selector(paste(_:)), "v"),
        ])
        let color = item("Color Monitor", #selector(setMonitor(_:)), "1", tag: Int(A2_MONITOR_COLOR.rawValue))
        color.state = .on
        let models = [A2_MODEL_IIPLUS, A2_MODEL_IIE, A2_MODEL_IIE_ENHANCED].map { m -> NSMenuItem in
            let i = item(Emulator.name(of: m), #selector(selectModel(_:)), tag: Int(m.rawValue))
            i.state = m == emulator.model ? .on : .off
            return i
        }
        let caps = item("Caps Lock", #selector(toggleCapsLock(_:)), "k", [.command, .shift])
        caps.state = emulator.capsLock ? .on : .off
        let saved = UserDefaults.standard.object(forKey: Self.mockingboardCountKey) as? Int
        emulator.mockingboards = min(max(saved ?? 1, 0), 2)
        let boards = [("No Mockingboard", 0), ("1 Mockingboard (Slot 4)", 1), ("2 Mockingboards (Slots 4, 5)", 2)]
            .map { title, count -> NSMenuItem in
                let i = item(title, #selector(selectMockingboards(_:)), tag: count)
                i.state = count == emulator.mockingboards ? .on : .off
                return i
            }
        emulator.setMIDISlot(UserDefaults.standard.integer(forKey: Self.midiSlotKey))
        let passport = item("Passport MIDI (Slot 2, Built-in GM Synth)", #selector(togglePassportMIDI(_:)))
        passport.state = emulator.midiSlot != 0 ? .on : .off
        submenu("Model", models + [.separator(), caps, .separator()] + boards + [.separator(), passport])
        submenu("Machine", [
            item("Reset (Ctrl-RESET)", #selector(reset), "r"),
            item("Power Cycle", #selector(powerCycle), "r", [.command, .shift]),
            item("Pause", #selector(togglePause(_:)), "p"),
            fastDiskItem(),
            .separator(),
            color,
            item("Green Monitor", #selector(setMonitor(_:)), "2", tag: Int(A2_MONITOR_GREEN.rawValue)),
            item("Amber Monitor", #selector(setMonitor(_:)), "3", tag: Int(A2_MONITOR_AMBER.rawValue)),
            item("White Monitor", #selector(setMonitor(_:)), "4", tag: Int(A2_MONITOR_WHITE.rawValue)),
            item("Scanlines", #selector(toggleScanlines(_:)), "l"),
            .separator(),
            item("Mute", #selector(toggleMute(_:)), "m", [.command, .shift]),
        ])
        let savedMode = UserDefaults.standard.object(forKey: Self.joystickKey) as? Int
        joystick.mode = Joystick.Mode(rawValue: savedMode ?? Joystick.Mode.keyboard.rawValue) ?? .keyboard
        submenu("Joystick", Joystick.Mode.allCases.map { mode -> NSMenuItem in
            let i = item(mode.title, #selector(selectJoystick(_:)), tag: mode.rawValue)
            i.state = mode == joystick.mode ? .on : .off
            return i
        })
        submenu("Window", [
            item("Minimize", #selector(NSWindow.performMiniaturize(_:)), "m"),
        ])

        NSApp.mainMenu = main
    }
}
