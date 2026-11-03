import AVFoundation

/// Turns the byte stream from the Passport MIDI card's ACIA into MIDI
/// messages (handling running status and SysEx) and plays them on a synth.
final class MIDIPlayer {
    private let synth: AVAudioUnitMIDIInstrument
    private var status: UInt8 = 0
    private var data: [UInt8] = []
    private var sysex: [UInt8]?

    init(synth: AVAudioUnitMIDIInstrument) {
        self.synth = synth
    }

    func feed(_ bytes: ArraySlice<UInt8>) {
        for b in bytes { feed(b) }
    }

    private func feed(_ b: UInt8) {
        if b >= 0xF8 {
            return                       // real-time (clock, active sensing): nothing to play
        }
        if b == 0xF0 {
            sysex = [b]
            return
        }
        if var message = sysex {
            if b < 0x80 {
                message.append(b)
                sysex = message
                return
            }
            if b == 0xF7 {
                message.append(b)
                synth.sendMIDISysExEvent(Data(message))
                sysex = nil
                return
            }
            sysex = nil                  // a status byte ends an unterminated SysEx
        }
        if b >= 0x80 {
            status = b < 0xF0 ? b : 0    // other system common messages are ignored
            data.removeAll()
            return
        }
        guard status != 0 else { return }
        data.append(b)
        let kind = status & 0xF0
        let needed = (kind == 0xC0 || kind == 0xD0) ? 1 : 2
        if data.count == needed {
            if needed == 1 {
                synth.sendMIDIEvent(status, data1: data[0])
            } else {
                synth.sendMIDIEvent(status, data1: data[0], data2: data[1])
            }
            data.removeAll()             // running status: keep `status`
        }
    }

    /// All notes off and controllers reset on every channel (after a reset or removing the card).
    func panic() {
        for channel: UInt8 in 0..<16 {
            synth.sendMIDIEvent(0xB0 | channel, data1: 123, data2: 0)   // all notes off
            synth.sendMIDIEvent(0xB0 | channel, data1: 121, data2: 0)   // reset controllers
        }
        status = 0
        data.removeAll()
        sysex = nil
    }
}
