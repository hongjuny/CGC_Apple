import Foundation
import XCTest
import A2Core
@testable import CGCApple

final class EmulatorMemoryTests: XCTestCase {
    @MainActor
    func testMemoryCardsFollowModelAndROMAvailability() async throws {
        let fm = FileManager.default
        let root = fm.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try fm.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? fm.removeItem(at: root) }
        for (name, size) in [("Apple2_Plus.rom", 0x3000), ("Apple2_Video.rom", 0x800),
                             ("Apple2e.rom", 0x4000), ("Apple2e_Enhanced.rom", 0x4000),
                             ("Apple2e_Enhanced_Video.rom", 0x1000)] {
            try Data(repeating: 0, count: size).write(to: root.appendingPathComponent(name))
        }
        let emulator = Emulator(model: A2_MODEL_IIE)
        try emulator.loadROMs(from: root)
        XCTAssertFalse(emulator.ramWorksActive)
        XCTAssertFalse(emulator.ramFactorEnabled)
        try emulator.setRamWorks(true)
        XCTAssertTrue(emulator.ramWorksActive)
        XCTAssertThrowsError(try emulator.setRamFactor(true))
        XCTAssertFalse(emulator.ramFactorEnabled)
        XCTAssertTrue(emulator.ramWorksActive)

        let invalidROM = root.appendingPathComponent("RamFactor.rom")
        try Data(repeating: 0, count: 8191).write(to: invalidROM)
        XCTAssertThrowsError(try emulator.setRamFactor(true))
        let rom = root.appendingPathComponent("ae ramfactor rom v1.4.bin")
        try Data(repeating: 0, count: 8192).write(to: rom)
        try emulator.setRamFactor(true) // Finds a valid MAME name even if the first file is invalid.
        XCTAssertTrue(emulator.ramFactorEnabled)
        XCTAssertTrue(emulator.hasRamFactorROM)

        try emulator.switchModel(to: A2_MODEL_IIPLUS)
        XCTAssertTrue(emulator.ramWorksEnabled)
        XCTAssertFalse(emulator.ramWorksActive)
        XCTAssertTrue(emulator.ramFactorEnabled)
        try emulator.switchModel(to: A2_MODEL_IIE_ENHANCED)
        XCTAssertTrue(emulator.ramWorksActive)
        XCTAssertTrue(emulator.ramFactorEnabled)

        try fm.removeItem(at: rom)
        XCTAssertThrowsError(try emulator.switchModel(to: A2_MODEL_IIPLUS))
        XCTAssertEqual(emulator.model, A2_MODEL_IIE_ENHANCED)
        XCTAssertTrue(emulator.hasRamFactorROM)
        XCTAssertTrue(emulator.ramFactorEnabled)
        XCTAssertTrue(emulator.ramWorksActive)
        try emulator.setRamFactor(false)
        try emulator.setRamWorks(false)
        try emulator.switchModel(to: A2_MODEL_IIPLUS)
        XCTAssertFalse(emulator.ramWorksEnabled)
        XCTAssertFalse(emulator.ramFactorEnabled)
    }
}
