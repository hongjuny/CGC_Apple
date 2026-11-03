import Foundation
import XCTest
import A2Core
@testable import CGCApple

final class EmulatorHardDiskTests: XCTestCase {
    @MainActor
    func testMountRoutingAndModelSwitch() throws {
        let fm = FileManager.default
        let root = fm.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try fm.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? fm.removeItem(at: root) }

        let hdv = root.appendingPathComponent("volume.hdv")
        try Data(repeating: 0, count: 64 * 512).write(to: hdv)
        let bigPO = root.appendingPathComponent("volume.po")
        try Data(repeating: 0, count: 1600 * 512).write(to: bigPO)
        let floppyPO = root.appendingPathComponent("floppy.po")
        try Data(repeating: 0, count: Int(A2_DSK_SIZE)).write(to: floppyPO)
        let bad = root.appendingPathComponent("bad.hdv")
        try Data(repeating: 0, count: 1000).write(to: bad)

        XCTAssertTrue(Emulator.isHardDiskImage(hdv))
        XCTAssertTrue(Emulator.isHardDiskImage(bigPO))
        XCTAssertFalse(Emulator.isHardDiskImage(floppyPO))
        XCTAssertFalse(Emulator.isHardDiskImage(root.appendingPathComponent("game.dsk")))

        try Data(repeating: 0xEA, count: 0x3000).write(to: root.appendingPathComponent("Apple2_Plus.rom"))
        try Data(repeating: 0, count: 0x800).write(to: root.appendingPathComponent("Apple2_Video.rom"))
        try Data(repeating: 0, count: 0x4000).write(to: root.appendingPathComponent("Apple2e.rom"))
        try Data(repeating: 0, count: 0x1000).write(to: root.appendingPathComponent("Apple2e_Enhanced_Video.rom"))

        let emulator = Emulator(model: A2_MODEL_IIPLUS)
        try emulator.loadROMs(from: root)
        XCTAssertThrowsError(try emulator.mountHardDisk(bad))
        XCTAssertNil(emulator.hardDiskURL)

        try emulator.mountHardDisk(hdv)
        XCTAssertEqual(emulator.hardDiskURL, hdv)
        try emulator.saveAllDisks()              // nothing written: a no-op

        // The hard disk survives a model change.
        try emulator.switchModel(to: A2_MODEL_IIE)
        XCTAssertEqual(emulator.hardDiskURL, hdv)

        try emulator.unmountHardDisk()
        XCTAssertNil(emulator.hardDiskURL)
    }
}
