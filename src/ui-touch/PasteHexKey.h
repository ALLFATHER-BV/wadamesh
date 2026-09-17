// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <ctype.h>
#include <stddef.h>

// Pasting a key that was shared inside a message (#526, reported by Steve8677):
// "join my channel, the secret is <32 hex digits>" is what people copy, and a
// key field's length cap then keeps the prose and cuts the key off. So a key
// field takes the key OUT of whatever was pasted instead of taking the text.
namespace PasteHexKey {

// The first `want` hex digits in `clip` that stand on their own: a run of
// exactly that length, not a longer one. With `spaced`, blanks inside the run
// are skipped, so a key copied in groups ("a1b2 c3d4 ...") is still found.
//
// The length has to match EXACTLY. A 64-digit public key must not satisfy a
// request for a 32-digit channel secret by handing over its first half, and a
// run with one stray digit too many is not a key at all.
//
// `out` must hold want + 1 bytes. Returns false and leaves `out` unspecified
// when there is no such run; the caller then pastes the text unchanged.
inline bool find(const char* clip, size_t want, bool spaced, char* out) {
    if (!clip || !out || !want) return false;
    size_t n = 0;
    for (const char* p = clip;; ++p) {
        const unsigned char c = (unsigned char)*p;
        if (c && isxdigit(c)) {
            if (n < want) out[n] = (char)c;
            ++n;
            continue;
        }
        if (spaced && (c == ' ' || c == '\t')) continue;
        if (n == want) {
            out[want] = '\0';
            return true;
        }
        n = 0;
        if (!c) return false;
    }
}

// Exact run first, then the spaced reading: a message that carries the key both
// ways should yield the plain one, and the spaced pass can otherwise glue
// unrelated hex words together.
inline const char* extract(const char* clip, size_t want, char* buf, size_t cap) {
    if (!want || !clip || cap <= want) return clip;
    if (find(clip, want, false, buf) || find(clip, want, true, buf)) return buf;
    return clip;
}

}  // namespace PasteHexKey
