// Spell suggestions over a front-coded word list. See spell.h for the format.
#include "spell.h"

#include <string.h>

namespace spell {

namespace {

inline bool isUpper(char c) { return c >= 'A' && c <= 'Z'; }
inline bool isLower(char c) { return c >= 'a' && c <= 'z'; }
inline char toLower(char c) { return isUpper(c) ? (char)(c - 'A' + 'a') : c; }
inline char toUpper(char c) { return isLower(c) ? (char)(c - 'a' + 'A') : c; }

// Longest word the list may hold. gen_spell_dict.py refuses anything longer.
constexpr int kMaxDictWord = 32;

// Decode the entry at pos into word, whose first `prefix` bytes are still the
// previous word's. Advances pos. Returns the word's length.
int decode(const Dict &d, uint32_t &pos, char *word, uint8_t &freq, int &prefix) {
    const uint8_t h = d.data[pos++];
    prefix = h >> 4;
    int suffix = h & 15;
    if (suffix == 15) suffix = d.data[pos++];
    int len = prefix + suffix;
    if (len > kMaxDictWord) len = kMaxDictWord;   // a corrupt list stays in bounds
    memcpy(word + prefix, d.data + pos, (size_t)(len - prefix));
    pos += (uint32_t)suffix;
    freq = d.data[pos++];
    word[len] = '\0';
    return len;
}

// Lowercase w into q. False when it does not fit.
bool lowerInto(const char *w, size_t n, char *q) {
    if (n == 0 || n > (size_t)kMaxWord) return false;
    for (size_t i = 0; i < n; i++) q[i] = toLower(w[i]);
    q[n] = '\0';
    return true;
}

// Equal once apostrophes are taken out of both: "dont" and "don't".
bool sameLetters(const char *a, const char *b) {
    for (;;) {
        while (*a == '\'') a++;
        while (*b == '\'') b++;
        if (*a != *b) return false;
        if (!*a) return true;
        a++;
        b++;
    }
}

bool lookup(const Dict &d, const char *q) {
    if (!d.data || d.blockCount == 0) return false;
    char word[kMaxDictWord + 1];
    uint8_t freq;
    int prefix;

    // Last block whose first word is <= q.
    uint32_t lo = 0, hi = d.blockCount;
    while (hi - lo > 1) {
        const uint32_t mid = lo + (hi - lo) / 2;
        uint32_t pos = d.blocks[mid];
        decode(d, pos, word, freq, prefix);
        if (strcmp(word, q) <= 0) lo = mid;
        else hi = mid;
    }

    uint32_t pos = d.blocks[lo];
    uint32_t left = d.wordCount - lo * (uint32_t)kBlockWords;
    if (left > (uint32_t)kBlockWords) left = kBlockWords;
    while (left-- > 0) {
        decode(d, pos, word, freq, prefix);
        const int c = strcmp(word, q);
        if (c == 0) return true;
        if (c > 0) return false;
    }
    return false;
}

}  // namespace

bool shouldCheck(const char *w, size_t n) {
    if (!w || n < 2 || n > (size_t)kMaxWord - 2) return false;
    if (w[0] == '\'' || w[n - 1] == '\'') return false;
    for (size_t i = 0; i < n; i++) {
        const char c = w[i];
        if (c == '\'' || isLower(c)) continue;
        if (isUpper(c) && i == 0) continue;
        return false;   // digit, symbol, non-ASCII, or a capital past the first letter
    }
    return true;
}

bool known(const Dict &d, const char *w, size_t n) {
    char q[kMaxWord + 1];
    if (!lowerInto(w, n, q)) return false;
    if (lookup(d, q)) return true;
    // Possessives: the list has "mike", not "mike's".
    if (n > 3 && q[n - 2] == '\'' && q[n - 1] == 's') {
        q[n - 2] = '\0';
        return lookup(d, q);
    }
    return false;
}

void applyCase(const char *typed, size_t n, char *sugg) {
    if (!sugg || !sugg[0]) return;
    size_t letters = 0, upper = 0;
    for (size_t i = 0; i < n; i++) {
        if (isUpper(typed[i])) { upper++; letters++; }
        else if (isLower(typed[i])) letters++;
    }
    if (letters >= 2 && upper == letters) {
        for (char *p = sugg; *p; p++) *p = toUpper(*p);
        return;
    }
    if (n > 0 && isUpper(typed[0])) sugg[0] = toUpper(sugg[0]);
    // The pronoun is a capital however it was typed: "im" -> "I'm".
    if (sugg[0] == 'i' && sugg[1] == '\'') sugg[0] = 'I';
}

int suggest(const Dict &d, const char *w, size_t n,
            char out[][kMaxWord + 1], int maxOut) {
    if (maxOut <= 0 || !d.data) return 0;
    if (maxOut > kMaxSuggest) maxOut = kMaxSuggest;
    // Two short of kMaxWord, so a match two letters longer still fits out[].
    if (n > (size_t)kMaxWord - 2) return 0;
    char q[kMaxWord + 1];
    if (!lowerInto(w, n, q)) return 0;
    const int m = (int)n;
    const int maxDist = m >= 4 ? 2 : 1;

    // One edit-distance matrix, row i for the dictionary word's first i
    // letters. Consecutive words share a prefix, so the rows for it are still
    // right from the word before and only the rest is computed -- a walk down
    // a trie without building one. Optimal string alignment distance: the
    // usual three edits plus swapping two neighbours ("teh" -> "the").
    constexpr int kRows = kMaxWord + 3;
    uint8_t rows[kRows][kMaxWord + 1];
    uint8_t rowMin[kRows];
    for (int j = 0; j <= m; j++) rows[0][j] = (uint8_t)j;
    rowMin[0] = 0;
    int valid = 0;   // rows 0..valid hold the current word's prefix

    struct Cand {
        uint8_t dist;
        uint8_t freq;
        char word[kMaxWord + 1];
    } best[kMaxSuggest];
    int count = 0;

    char word[kMaxDictWord + 1];
    uint32_t pos = 0;
    for (uint32_t k = 0; k < d.wordCount && pos < d.size; k++) {
        uint8_t freq;
        int prefix;
        const int len = decode(d, pos, word, freq, prefix);
        if (valid > prefix) valid = prefix;
        if (len < m - maxDist || len > m + maxDist) continue;

        bool dead = false;
        while (valid < len) {
            // Row minima never fall, so once one is past the limit every word
            // that starts with these letters is too.
            if (rowMin[valid] > maxDist) { dead = true; break; }
            const int i = valid + 1;
            const char c = word[i - 1];
            uint8_t lo = (uint8_t)i;
            rows[i][0] = (uint8_t)i;
            for (int j = 1; j <= m; j++) {
                int v = rows[i - 1][j - 1] + (c != q[j - 1]);
                if (rows[i - 1][j] + 1 < v) v = rows[i - 1][j] + 1;
                if (rows[i][j - 1] + 1 < v) v = rows[i][j - 1] + 1;
                if (i > 1 && j > 1 && c == q[j - 2] && word[i - 2] == q[j - 1]
                    && rows[i - 2][j - 2] + 1 < v) {
                    v = rows[i - 2][j - 2] + 1;
                }
                rows[i][j] = (uint8_t)(v > 255 ? 255 : v);
                if (rows[i][j] < lo) lo = rows[i][j];
            }
            rowMin[i] = lo;
            valid = i;
        }
        if (dead) continue;

        uint8_t dist = rows[len][m];
        if (dist == 0 || dist > maxDist) continue;
        // A missing apostrophe is the likeliest "typo" of all -- on these
        // keyboards it is a symbol-layer key -- so that match goes first
        // whatever else is one edit away: "im" -> "I'm" ahead of "in".
        if (sameLetters(word, q)) dist = 0;

        // Keep the best maxOut: nearer first, then more common.
        int at = count;
        while (at > 0 && (dist < best[at - 1].dist
                          || (dist == best[at - 1].dist && freq > best[at - 1].freq))) {
            at--;
        }
        if (at >= maxOut) continue;
        const int last = count < maxOut ? count : maxOut - 1;
        for (int s = last; s > at; s--) best[s] = best[s - 1];
        best[at].dist = dist;
        best[at].freq = freq;
        memcpy(best[at].word, word, (size_t)len + 1);
        if (count < maxOut) count++;
    }

    for (int i = 0; i < count; i++) {
        memcpy(out[i], best[i].word, strlen(best[i].word) + 1);
        applyCase(w, n, out[i]);
    }
    return count;
}

}  // namespace spell
