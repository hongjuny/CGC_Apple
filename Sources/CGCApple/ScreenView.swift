import AppKit
import A2Core

/// Displays the emulator framebuffer and turns key events into Apple II keys.
@MainActor
final class ScreenView: NSView {
    let emulator: Emulator
    var joystick: Joystick?
    var onDiskDropped: ((URL, Int) -> Void)?

    private let colorSpace = CGColorSpace(name: CGColorSpace.sRGB)!

    init(emulator: Emulator) {
        self.emulator = emulator
        super.init(frame: NSRect(x: 0, y: 0, width: Int(A2_SCREEN_W) * 2, height: Int(A2_SCREEN_H) * 2))
        wantsLayer = true
        layer?.backgroundColor = NSColor.black.cgColor
        layer?.contentsGravity = .resizeAspect
        layer?.magnificationFilter = .nearest
        registerForDraggedTypes([.fileURL])
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) is not used") }

    override var acceptsFirstResponder: Bool { true }
    override var isFlipped: Bool { true }

    // MARK: Mouse (joystick in mouse mode)

    override func updateTrackingAreas() {
        super.updateTrackingAreas()
        trackingAreas.forEach(removeTrackingArea)
        addTrackingArea(NSTrackingArea(rect: bounds, options: [.mouseMoved, .activeInKeyWindow, .inVisibleRect],
                                       owner: self, userInfo: nil))
    }

    /// Pointer position over the displayed (aspect-fitted) Apple II screen, 0...1.
    private func screenPosition(_ event: NSEvent) -> (CGFloat, CGFloat) {
        let p = convert(event.locationInWindow, from: nil)
        let aspect = CGFloat(A2_SCREEN_W) / CGFloat(A2_SCREEN_H)
        var w = bounds.width, h = bounds.height
        if w / h > aspect { w = h * aspect } else { h = w / aspect }
        let x0 = (bounds.width - w) / 2, y0 = (bounds.height - h) / 2
        return ((p.x - x0) / w, (p.y - y0) / h)
    }

    override func mouseMoved(with event: NSEvent) {
        let (x, y) = screenPosition(event)
        joystick?.mouseMoved(x: x, y: y)
    }
    override func mouseDragged(with event: NSEvent) { mouseMoved(with: event) }
    override func rightMouseDragged(with event: NSEvent) { mouseMoved(with: event) }
    override func mouseDown(with event: NSEvent) { joystick?.mouseButton(0, down: true) }
    override func mouseUp(with event: NSEvent) { joystick?.mouseButton(0, down: false) }
    override func rightMouseDown(with event: NSEvent) { joystick?.mouseButton(1, down: true) }
    override func rightMouseUp(with event: NSEvent) { joystick?.mouseButton(1, down: false) }

    func refresh() {
        let w = Int(A2_SCREEN_W), h = Int(A2_SCREEN_H)
        let image: CGImage? = emulator.withFramebuffer { fb in
            let data = Data(bytes: fb, count: w * h * 4) as CFData
            guard let provider = CGDataProvider(data: data) else { return nil }
            return CGImage(width: w, height: h, bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: w * 4,
                           space: colorSpace,
                           bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.noneSkipLast.rawValue),
                           provider: provider, decode: nil, shouldInterpolate: false, intent: .defaultIntent)
        }
        layer?.contents = image
    }

    // MARK: Keyboard

    // US layout fallback for when an input method (e.g. Korean) supplies non-ASCII characters.
    private static let usLayout: [UInt16: (Character, Character)] = [
        0: ("a", "A"), 11: ("b", "B"), 8: ("c", "C"), 2: ("d", "D"), 14: ("e", "E"), 3: ("f", "F"),
        5: ("g", "G"), 4: ("h", "H"), 34: ("i", "I"), 38: ("j", "J"), 40: ("k", "K"), 37: ("l", "L"),
        46: ("m", "M"), 45: ("n", "N"), 31: ("o", "O"), 35: ("p", "P"), 12: ("q", "Q"), 15: ("r", "R"),
        1: ("s", "S"), 17: ("t", "T"), 32: ("u", "U"), 9: ("v", "V"), 13: ("w", "W"), 7: ("x", "X"),
        16: ("y", "Y"), 6: ("z", "Z"),
        18: ("1", "!"), 19: ("2", "@"), 20: ("3", "#"), 21: ("4", "$"), 23: ("5", "%"), 22: ("6", "^"),
        26: ("7", "&"), 28: ("8", "*"), 25: ("9", "("), 29: ("0", ")"),
        27: ("-", "_"), 24: ("=", "+"), 33: ("[", "{"), 30: ("]", "}"), 42: ("\\", "|"),
        41: (";", ":"), 39: ("'", "\""), 43: (",", "<"), 47: (".", ">"), 44: ("/", "?"), 50: ("`", "~"),
        49: (" ", " "),
    ]

    private static let specialKeys: [UInt16: UInt8] = [
        36: 0x0D, 76: 0x0D,     // Return, keypad Enter
        51: 0x08, 123: 0x08,    // Delete, left arrow
        124: 0x15,              // right arrow
        125: 0x0A, 126: 0x0B,   // down / up arrows
        53: 0x1B,               // Escape
        48: 0x09,               // Tab
    ]

    override func keyDown(with event: NSEvent) {
        if event.modifierFlags.contains(.command) {
            super.keyDown(with: event)
            return
        }
        if joystick?.handleButtonKey(event.keyCode, down: true) == true {
            return                       // ` ~ key: joystick button 1
        }
        if joystick?.handleArrow(event.keyCode, down: true) == true {
            return                       // arrows are the joystick in keyboard mode
        }
        if let code = Self.specialKeys[event.keyCode] {
            emulator.key(code)
            return
        }
        var ch = event.characters?.unicodeScalars.first
        if ch?.isASCII != true || event.modifierFlags.contains(.control) {
            if let pair = Self.usLayout[event.keyCode] {
                let c = event.modifierFlags.contains(.shift) ? pair.1 : pair.0
                ch = c.unicodeScalars.first
            }
        }
        guard let scalar = ch, scalar.isASCII else { return }
        var ascii = UInt8(scalar.value)
        if event.modifierFlags.contains(.control) {
            ascii &= 0x1F
        }
        emulator.key(ascii)
    }

    override func keyUp(with event: NSEvent) {
        if joystick?.handleButtonKey(event.keyCode, down: false) == true {
            return
        }
        if joystick?.handleArrow(event.keyCode, down: false) == true {
            return
        }
        emulator.keyUp()
    }

    override func flagsChanged(with event: NSEvent) {
        // Left Option = button 0 (IIe Open Apple), right Option = button 1 (Closed Apple).
        let down = event.modifierFlags.contains(.option)
        switch event.keyCode {
        case 58: joystick?.setOptionButton(0, down: down)
        case 61: joystick?.setOptionButton(1, down: down)
        default: break
        }
    }

    // MARK: Drag and drop (drop on the right half for drive 2)

    private static let diskExtensions: Set<String> = ["dsk", "do", "po", "nib", "woz"]

    private func droppedDisk(_ sender: NSDraggingInfo) -> URL? {
        let urls = sender.draggingPasteboard.readObjects(forClasses: [NSURL.self]) as? [URL]
        return urls?.first { Self.diskExtensions.contains($0.pathExtension.lowercased()) }
    }

    override func draggingEntered(_ sender: NSDraggingInfo) -> NSDragOperation {
        droppedDisk(sender) == nil ? [] : .copy
    }

    override func performDragOperation(_ sender: NSDraggingInfo) -> Bool {
        guard let url = droppedDisk(sender) else { return false }
        let point = convert(sender.draggingLocation, from: nil)
        onDiskDropped?(url, point.x > bounds.midX ? 1 : 0)
        return true
    }
}
