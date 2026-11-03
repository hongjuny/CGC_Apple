import AVFoundation
import A2Core

/// Plays the stereo frames produced each frame (speaker in both channels,
/// Mockingboard PSGs left/right) through a small ring buffer drained by an
/// AVAudioSourceNode on the audio thread.
final class AudioOutput: @unchecked Sendable {
    private let engine = AVAudioEngine()
    private let lock = NSLock()
    private var ring = [Float](repeating: 0, count: 16384 * 2)   // interleaved L/R
    private var readIndex = 0        // in frames
    private var count = 0            // in frames
    private var last: (Float, Float) = (0, 0)
    private let capacity = 16384
    private let maxLatency = 8192    // frames kept before old ones are dropped
    /// Frames to hold before (re)starting playback. Frames arrive in bursts from
    /// the 60 Hz emulation while the device pulls steadily, so this absorbs the jitter.
    static let targetFill = 2048
    private var primed = false

    var muted = false

    /// Frames currently queued, for the emulator's rate control.
    var fill: Int {
        lock.lock()
        defer { lock.unlock() }
        return count
    }

    init() {
        let format = AVAudioFormat(standardFormatWithSampleRate: Double(A2_AUDIO_RATE), channels: 2)!
        let source = AVAudioSourceNode(format: format) { [unowned self] _, _, frameCount, bufferList in
            let buffers = UnsafeMutableAudioBufferListPointer(bufferList)
            guard buffers.count >= 2,
                  let left = buffers[0].mData?.assumingMemoryBound(to: Float.self),
                  let right = buffers[1].mData?.assumingMemoryBound(to: Float.self) else { return noErr }
            self.pull(left: left, right: right, frames: Int(frameCount))
            return noErr
        }
        engine.attach(source)
        engine.connect(source, to: engine.mainMixerNode, format: format)
        try? engine.start()
    }

    /// Takes `frames` interleaved stereo frames (2 * frames samples).
    func push(_ samples: [Int16], frames n: Int) {
        lock.lock()
        defer { lock.unlock() }
        for i in 0..<n {
            if count >= maxLatency {
                readIndex = (readIndex + 1) % capacity
                count -= 1
            }
            let slot = (readIndex + count) % capacity
            ring[slot * 2] = muted ? 0 : Float(samples[i * 2]) / 32768
            ring[slot * 2 + 1] = muted ? 0 : Float(samples[i * 2 + 1]) / 32768
            count += 1
        }
    }

    private func pull(left: UnsafeMutablePointer<Float>, right: UnsafeMutablePointer<Float>, frames: Int) {
        lock.lock()
        defer { lock.unlock() }
        if !primed && count >= Self.targetFill {
            primed = true
        }
        for i in 0..<frames {
            if primed && count > 0 {
                last = (ring[readIndex * 2], ring[readIndex * 2 + 1])
                readIndex = (readIndex + 1) % capacity
                count -= 1
            } else {
                primed = false                          // underrun: refill before resuming
                last = (last.0 * 0.99, last.1 * 0.99)   // fade instead of clicking
            }
            left[i] = last.0
            right[i] = last.1
        }
    }
}
