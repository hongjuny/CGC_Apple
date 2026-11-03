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

        // Judged by content, whatever the extension: a headerless block image,
        // and 2IMG images holding a hard disk or a 140K floppy.
        func twoIMG(format: UInt32, bytes: Int) -> Data {
            var h = Data(count: 64)
            h.replaceSubrange(0..<4, with: Data("2IMG".utf8))
            func put(_ v: UInt32, _ at: Int) { for i in 0..<4 { h[at + i] = UInt8(truncatingIfNeeded: v >> (8 * i)) } }
            put(64, 0x08); put(format, 0x0C); put(UInt32(bytes / 512), 0x14); put(64, 0x18); put(UInt32(bytes), 0x1C)
            return h + Data(repeating: 0, count: bytes) + Data("trailer".utf8)
        }
        let twoMeg = root.appendingPathComponent("volume.2meg")
        try Data(repeating: 0, count: 1600 * 512).write(to: twoMeg)
        let hd2mg = root.appendingPathComponent("volume.2mg")
        try twoIMG(format: 1, bytes: 1600 * 512).write(to: hd2mg)
        let floppy2mg = root.appendingPathComponent("floppy.2mg")
        try twoIMG(format: 0, bytes: Int(A2_DSK_SIZE)).write(to: floppy2mg)
        XCTAssertTrue(Emulator.isHardDiskImage(twoMeg))
        XCTAssertTrue(Emulator.isHardDiskImage(hd2mg))
        XCTAssertFalse(Emulator.isHardDiskImage(floppy2mg))

        try Data(repeating: 0xEA, count: 0x3000).write(to: root.appendingPathComponent("Apple2_Plus.rom"))
        try Data(repeating: 0, count: 0x800).write(to: root.appendingPathComponent("Apple2_Video.rom"))
        try Data(repeating: 0, count: 0x4000).write(to: root.appendingPathComponent("Apple2e.rom"))
        try Data(repeating: 0, count: 0x1000).write(to: root.appendingPathComponent("Apple2e_Enhanced_Video.rom"))

        let emulator = Emulator(model: A2_MODEL_IIPLUS)
        try emulator.loadROMs(from: root)
        XCTAssertThrowsError(try emulator.mountHardDisk(bad))
        XCTAssertNil(emulator.hardDiskURL)

        try emulator.mountHardDisk(hd2mg)
        try emulator.mountHardDisk(twoMeg)
        try emulator.insertDisk(floppy2mg, drive: 1)   // the 2IMG wrapper is stripped
        XCTAssertEqual(emulator.diskURLs[1], floppy2mg)

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
