import Foundation
import XCTest
import A2Core
@testable import CGCApple

final class EmulatorDiskTests: XCTestCase {
    @MainActor
    func testFailedSaveKeepsDiskAndMachine() async throws {
        let fm = FileManager.default
        let root = fm.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try fm.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? fm.removeItem(at: root) }
        let disks = root.appendingPathComponent("disks")
        let moved = root.appendingPathComponent("moved")
        try fm.createDirectory(at: disks, withIntermediateDirectories: true)
        let disk = disks.appendingPathComponent("test.dsk")
        try Data(repeating: 0, count: Int(A2_DSK_SIZE)).write(to: disk)
        let replacement = root.appendingPathComponent("replacement.dsk")
        try Data(repeating: 1, count: Int(A2_DSK_SIZE)).write(to: replacement)

        // A tiny ROM starts the motor and writes bytes through the Disk II switches.
        var rom = Data(repeating: 0xEA, count: 0x3000)
        let program: [UInt8] = [0xAD, 0xE9, 0xC0, 0xAD, 0xEF, 0xC0,
                               0xA9, 0xFF, 0x8D, 0xED, 0xC0, 0xAD, 0xEC, 0xC0,
                               0x4C, 0x0B, 0xD0]
        rom.replaceSubrange(0..<program.count, with: program)
        rom[0x2FFC] = 0; rom[0x2FFD] = 0xD0
        try rom.write(to: root.appendingPathComponent("Apple2_Plus.rom"))
        try Data(repeating: 0, count: 0x800).write(to: root.appendingPathComponent("Apple2_Video.rom"))

        let emulator = Emulator(model: A2_MODEL_IIPLUS)
        try emulator.loadROMs(from: root)
        try emulator.insertDisk(disk, drive: 0)
        emulator.powerCycle()
        emulator.runFrame()
        try fm.moveItem(at: disks, to: moved)

        XCTAssertThrowsError(try emulator.saveAllDisks())
        XCTAssertThrowsError(try emulator.ejectDisk(drive: 0))
        XCTAssertThrowsError(try emulator.insertDisk(replacement, drive: 0))
        XCTAssertThrowsError(try emulator.switchModel(to: A2_MODEL_IIE))
        XCTAssertEqual(emulator.diskURLs[0], disk)
        XCTAssertEqual(emulator.model, A2_MODEL_IIPLUS)

        try fm.moveItem(at: moved, to: disks)
        try emulator.saveAllDisks()
        try emulator.ejectDisk(drive: 0)
        XCTAssertNil(emulator.diskURLs[0])

        // Disk reload failure during a model change must roll back the machine.
        try Data(repeating: 0, count: 0x4000).write(to: root.appendingPathComponent("Apple2e.rom"))
        try Data(repeating: 0, count: 0x1000).write(to: root.appendingPathComponent("Apple2e_Enhanced_Video.rom"))
        try emulator.insertDisk(replacement, drive: 0)
        let missing = root.appendingPathComponent("missing.dsk")
        try fm.moveItem(at: replacement, to: missing)
        XCTAssertThrowsError(try emulator.switchModel(to: A2_MODEL_IIE))
        XCTAssertEqual(emulator.model, A2_MODEL_IIPLUS)
        XCTAssertEqual(emulator.diskURLs[0], replacement)
        try fm.moveItem(at: missing, to: replacement)
        try emulator.switchModel(to: A2_MODEL_IIE)
        XCTAssertEqual(emulator.model, A2_MODEL_IIE)
        XCTAssertEqual(emulator.diskURLs[0], replacement)
    }
}
