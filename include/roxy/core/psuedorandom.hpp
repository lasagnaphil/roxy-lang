#pragma once

#include "roxy/core/types.hpp"

namespace rx {

inline u64 rol64(u64 x, int k)
{
    return (x << k) | (x >> (64 - k));
}

struct xoshiro256ss_state {
    u64 s[4];
};

inline u64 xoshiro256ss(struct xoshiro256ss_state *state)
{
    u64 *s = state->s;
    u64 const result = rol64(s[1] * 5, 7) * 9;
    u64 const t = s[1] << 17;

    s[2] ^= s[0];
    s[3] ^= s[1];
    s[1] ^= s[2];
    s[0] ^= s[3];

    s[2] ^= t;
    s[3] = rol64(s[3], 45);

    return result;
}

struct splitmix64_state {
    u64 s;
};

inline u64 splitmix64(struct splitmix64_state *state) {
    u64 result = (state->s += 0x9E3779B97f4A7C15);
    result = (result ^ (result >> 30)) * 0xBF58476D1CE4E5B9;
    result = (result ^ (result >> 27)) * 0x94D049BB133111EB;
    return result ^ (result >> 31);
}

// one could do the same for any of the other generators
inline void xoshiro256ss_init(struct xoshiro256ss_state *state, u64 seed) {
    struct splitmix64_state smstate = {seed};

    u64 tmp = splitmix64(&smstate);
    state->s[0] = tmp;
    tmp = splitmix64(&smstate);
    state->s[1] = tmp;
    tmp = splitmix64(&smstate);
    state->s[2] = tmp;
    tmp = splitmix64(&smstate);
    state->s[3] = tmp;
}

}