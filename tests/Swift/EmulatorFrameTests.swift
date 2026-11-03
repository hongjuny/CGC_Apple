import Foundation
import XCTest
import A2Core
@testable import CGCApple

final class EmulatorFrameTests: XCTestCase {
    @MainActor
    func testNormalAndWarpFramesConsumePastedKeys() async throws {
        let fm = FileManager.default
        let root = fm.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try fm.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? fm.removeItem(at: root) }

        // Echo keyboard input to successive text columns, clearing the strobe each time.
        var rom = Data(repeating: 0xEA, count: 0x3000)
        let program: [UInt8] = [0xA2, 0x00, 0xAD, 0x00, 0xC0, 0x10, 0xFB,
                               0x9D, 0x00, 0x04, 0xAD, 0x10, 0xC0, 0xE8,
                               0x4C, 0x02, 0xD0]
        rom.replaceSubrange(0..<program.count, with: program)
        rom[0x2FFC] = 0; rom[0x2FFD] = 0xD0
        try rom.write(to: root.appendingPathComponent("Apple2_Plus.rom"))
        let chars = Data((0..<0x800).map { UInt8(($0 / 8) & 0x7F) })
        try chars.write(to: root.appendingPathComponent("Apple2_Video.rom"))

        let normal = Emulator(model: A2_MODEL_IIPLUS)
        let fast = Emulator(model: A2_MODEL_IIPLUS)
        for emulator in [normal, fast] {
            try emulator.loadROMs(from: root)
            emulator.powerCycle()
        }
        func screen(_ emulator: Emulator) -> [UInt32] {
            emulator.withFramebuffer { Array(UnsafeBufferPointer(start: $0, count: Int(A2_SCREEN_W * A2_SCREEN_H))) }
        }
        normal.runFrame()
        fast.runWarp(until: 0) // Stopped motor: exactly one frame, then render.
        let initial = screen(normal)
        XCTAssertEqual(initial, screen(fast))
        normal.paste("hello")
        fast.paste("hello")
        for _ in 0..<5 {
            normal.runFrame()
            fast.runWarp(until: 0)
            XCTAssertEqual(screen(normal), screen(fast))
        }
        XCTAssertNotEqual(initial, screen(normal))
        XCTAssertEqual(fast.audio.fill, 0)
    }
}
