#pragma once

#include <cstddef>
#include <cstdint>

// Deterministic decision source for the structural program generator
// (generator.hpp). One generator, two drivers:
//
//  - Seeded PRNG mode (splitmix64): benchmark-corpus generation (the roxy_gen
//    CLI) and the fixed-seed `Structured Gen` regression suite. Same seed ->
//    byte-identical program, forever.
//  - Byte-buffer mode: structure-aware fuzzing. libFuzzer's mutated input
//    drives every generator decision, so coverage feedback shapes which
//    programs get generated — mutating the bytes mutates the program.
//
// Dry-buffer contract (byte mode only): once the buffer is exhausted, range()
// returns 0 and chance() returns false. The generator keeps its
// smallest/terminal option at choice index 0 and phrases chance() so that
// `false` is the simpler path, so a short fuzz input degrades gracefully into
// a small program instead of failing or recursing forever.
namespace rx::gen {

class Entropy {
public:
    explicit Entropy(uint64_t seed)
        : m_mode(Mode::Prng), m_state(seed * 0x9e3779b97f4a7c15ull + 0x2545f4914f6cdd1dull) {}

    Entropy(const uint8_t* data, size_t size)
        : m_mode(Mode::Bytes), m_data(data), m_size(size) {}

    bool dry() const { return m_mode == Mode::Bytes && m_pos >= m_size; }

    // Uniform-ish value in [0, n). n == 0 or 1 returns 0 without consuming
    // entropy. Modulo bias is irrelevant for generation purposes.
    uint32_t range(uint32_t n) {
        if (n <= 1) return 0;
        uint32_t value;
        if (m_mode == Mode::Prng) {
            value = static_cast<uint32_t>(splitmix64() >> 32);
        } else {
            if (dry()) return 0;
            value = next_byte();
            if (n > 0x100) value |= static_cast<uint32_t>(next_byte()) << 8;
            if (n > 0x10000) value |= static_cast<uint32_t>(next_byte()) << 16;
        }
        return value % n;
    }

    // True with roughly `percent`% probability. Dry byte-buffer -> false, so
    // callers must phrase decisions with `true` as the *more* elaborate path.
    bool chance(uint32_t percent) {
        if (dry()) return false;
        return range(100) < percent;
    }

private:
    enum class Mode : uint8_t { Prng, Bytes };

    uint64_t splitmix64() {
        uint64_t z = (m_state += 0x9e3779b97f4a7c15ull);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        return z ^ (z >> 31);
    }

    uint8_t next_byte() { return m_pos < m_size ? m_data[m_pos++] : 0; }

    Mode m_mode;
    uint64_t m_state = 0;
    const uint8_t* m_data = nullptr;
    size_t m_size = 0;
    size_t m_pos = 0;
};

} // namespace rx::gen
