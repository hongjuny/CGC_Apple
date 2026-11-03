import AppKit
import GameController

/// Apple II joystick emulation: paddle 0 is X, paddle 1 is Y (0..255, 127 =
/// centre), buttons 0 and 1. The axes come from the arrow keys, the mouse or a
/// game controller depending on the mode; the Option keys always work as the
/// buttons as well.
@MainActor
final class Joystick {
    enum Mode: Int, CaseIterable {
        case off = 0, keyboard, mouse, controller

        var title: String {
            switch self {
            case .off: return "Off"
            case .keyboard: return "Keyboard (Arrow Keys, Option = Buttons)"
            case .mouse: return "Mouse (Click = Button 0, Right-Click = Button 1)"
            case .controller: return "Game Controller (A, B = Buttons)"
            }
        }
    }

    private let emulator: Emulator
    var mode: Mode = .keyboard {
        didSet { recentre() }
    }

    private static let left: UInt16 = 123, right: UInt16 = 124, down: UInt16 = 125, up: UInt16 = 126
    private var arrows = Set<UInt16>()
    private var optionButtons = [false, false]
    private var mouseButtons = [false, false]
    private var padButtons = [false, false]

    init(emulator: Emulator) {
        self.emulator = emulator
    }

    // MARK: Keyboard

    /// Returns true if the key was taken as a joystick direction (and should not reach the Apple II keyboard).
    func handleArrow(_ keyCode: UInt16, down isDown: Bool) -> Bool {
        guard mode == .keyboard, [Self.left, Self.right, Self.down, Self.up].contains(keyCode) else { return false }
        if isDown { arrows.insert(keyCode) } else { arrows.remove(keyCode) }
        func axis(_ low: UInt16, _ high: UInt16) -> UInt8 {
            switch (arrows.contains(low), arrows.contains(high)) {
            case (true, false): return 0
            case (false, true): return 255
            default: return 127   // self-centring, like a sprung stick
            }
        }
        emulator.setPaddle(0, axis(Self.left, Self.right))
        emulator.setPaddle(1, axis(Self.up, Self.down))
        return true
    }

    func setOptionButton(_ n: Int, down: Bool) {
        optionButtons[n] = down
        applyButtons()
    }

    // MARK: Mouse

    /// x, y are the pointer position over the Apple II screen, 0...1 from the top left.
    func mouseMoved(x: CGFloat, y: CGFloat) {
        guard mode == .mouse else { return }
        emulator.setPaddle(0, Self.paddle(x))
        emulator.setPaddle(1, Self.paddle(y))
    }

    func mouseButton(_ n: Int, down: Bool) {
        guard mode == .mouse else { return }
        mouseButtons[n] = down
        applyButtons()
    }

    // MARK: Game controller

    /// Polled every timer tick.
    func poll() {
        guard mode == .controller,
              let pad = (GCController.current ?? GCController.controllers().first)?.extendedGamepad else { return }
        var x = pad.leftThumbstick.xAxis.value, y = pad.leftThumbstick.yAxis.value
        if pad.dpad.xAxis.value != 0 { x = pad.dpad.xAxis.value }
        if pad.dpad.yAxis.value != 0 { y = pad.dpad.yAxis.value }
        emulator.setPaddle(0, Self.paddle(CGFloat(x + 1) / 2))
        emulator.setPaddle(1, Self.paddle(CGFloat(1 - y) / 2))   // stick up = 0
        padButtons = [pad.buttonA.isPressed, pad.buttonB.isPressed]
        applyButtons()
    }

    // MARK: Helpers

    private static func paddle(_ v: CGFloat) -> UInt8 {
        UInt8((min(max(v, 0), 1) * 255).rounded())
    }

    private func applyButtons() {
        for n in 0..<2 {
            emulator.setButton(Int32(n), down: optionButtons[n] || mouseButtons[n] || padButtons[n])
        }
    }

    private func recentre() {
        arrows.removeAll()
        mouseButtons = [false, false]
        padButtons = [false, false]
        emulator.setPaddle(0, 127)
        emulator.setPaddle(1, 127)
        applyButtons()
    }
}
