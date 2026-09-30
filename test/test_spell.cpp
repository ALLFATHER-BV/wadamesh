// Host test for src/ui-touch/spell.cpp. Build and run from the repo root:
//   c++ -std=c++17 -Isrc test/test_spell.cpp src/ui-touch/spell.cpp -o /tmp/test_spell && /tmp/test_spell
//
// Runs against a small list encoded here the way scripts/build/gen_spell_dict.py
// encodes the real one, rather than the generated English header, so the
// tests pin the engine's behaviour and not the word list's. Enough words to
// span several blocks, and one with a suffix long enough to need the escaped
// length byte.
#include "ui-touch/spell.h"

#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

static int g_fail = 0;
static int g_run = 0;

static void ok(bool cond, const char *what) {
    g_run++;
    if (!cond) {
        g_fail++;
        printf("  FAIL  %s\n", what);
    }
}

// word, frequency 0..255 -- already scaled, unlike the generator's counts.
static const std::pair<const char *, int> kWords[] = {
    {"the", 255}, {"then", 180}, {"ten", 150}, {"them", 200}, {"there", 190},
    {"their", 185}, {"they", 210}, {"these", 170}, {"thief", 60}, {"tier", 40},
    {"receive", 120}, {"relieve", 80}, {"believe", 140}, {"message", 130},
    {"messages", 100}, {"manage", 90}, {"repeater", 120}, {"peter", 90},
    {"i'm", 230}, {"it", 250}, {"in", 245}, {"is", 248}, {"don't", 220},
    {"done", 160}, {"mike", 70}, {"node", 110}, {"nodes", 100}, {"none", 95},
    {"with", 240}, {"which", 200}, {"wish", 120}, {"a", 255}, {"an", 230},
    {"and", 252}, {"as", 200}, {"at", 210}, {"be", 220}, {"by", 190},
    {"cat", 90}, {"dog", 90}, {"eh", 50}, {"go", 180}, {"he", 230},
    {"hi", 150}, {"if", 200}, {"me", 230}, {"my", 225}, {"no", 230},
    {"of", 250}, {"on", 240}, {"or", 200}, {"so", 220}, {"to", 252},
    {"up", 200}, {"us", 180}, {"we", 230}, {"yes", 200}, {"you", 255},
    {"internationalization", 5}, {"zebra", 10},
};

struct Built {
    std::vector<uint8_t> data;
    std::vector<uint32_t> blocks;
    spell::Dict dict;
};

static void build(Built &b) {
    std::vector<std::pair<std::string, int>> ws;
    for (const auto &w : kWords) ws.emplace_back(w.first, w.second);
    std::sort(ws.begin(), ws.end());
    std::string prev;
    for (size_t i = 0; i < ws.size(); i++) {
        const std::string &w = ws[i].first;
        size_t p = 0;
        if (i % spell::kBlockWords == 0) {
            b.blocks.push_back((uint32_t)b.data.size());
        } else {
            while (p < std::min({w.size(), prev.size(), (size_t)15}) && w[p] == prev[p]) p++;
        }
        const size_t suffix = w.size() - p;
        if (suffix < 15) {
            b.data.push_back((uint8_t)((p << 4) | suffix));
        } else {
            b.data.push_back((uint8_t)((p << 4) | 15));
            b.data.push_back((uint8_t)suffix);
        }
        for (size_t k = p; k < w.size(); k++) b.data.push_back((uint8_t)w[k]);
        b.data.push_back((uint8_t)ws[i].second);
        prev = w;
    }
    b.dict = {b.data.data(), (uint32_t)b.data.size(), b.blocks.data(),
              (uint32_t)b.blocks.size(), (uint32_t)ws.size()};
}

static bool isKnown(const spell::Dict &d, const char *w) {
    return spell::known(d, w, strlen(w));
}

// Suggestions joined with spaces, for one-line expectations.
static std::string sugg(const spell::Dict &d, const char *w) {
    char out[spell::kMaxSuggest][spell::kMaxWord + 1];
    const int n = spell::suggest(d, w, strlen(w), out, spell::kMaxSuggest);
    std::string s;
    for (int i = 0; i < n; i++) {
        if (i) s += ' ';
        s += out[i];
    }
    return s;
}

static void okSugg(const spell::Dict &d, const char *w, const char *want) {
    const std::string got = sugg(d, w);
    char what[128];
    snprintf(what, sizeof(what), "suggest(%s) = \"%s\", want \"%s\"", w, got.c_str(), want);
    ok(got == want, what);
}

static void testKnown(const spell::Dict &d) {
    ok(d.blockCount > 1, "test list spans several blocks");
    // Every word, which walks every block and every prefix length.
    for (const auto &w : kWords) {
        char what[64];
        snprintf(what, sizeof(what), "known(%s)", w.first);
        ok(isKnown(d, w.first), what);
    }
    ok(isKnown(d, "The"), "known ignores case");
    ok(isKnown(d, "Mike's"), "possessive of a known word is known");
    ok(!isKnown(d, "teh"), "typo is not known");
    ok(!isKnown(d, "aa"), "before the first word");
    ok(!isKnown(d, "zzz"), "after the last word");
    ok(!isKnown(d, "thei"), "a prefix of a word is not the word");
    ok(!isKnown(d, "xyz's"), "possessive of an unknown word");
}

static void testShouldCheck() {
    auto chk = [](const char *w) { return spell::shouldCheck(w, strlen(w)); };
    ok(chk("hello"), "plain word");
    ok(chk("Hello"), "capitalised word");
    ok(chk("don't"), "contraction");
    ok(!chk("a"), "single letter");
    ok(!chk("LORA"), "acronym");
    ok(!chk("LongFast"), "camel case");
    ok(!chk("abc123"), "digits");
    ok(!chk("'tis"), "leading apostrophe");
    ok(!chk("dogs'"), "trailing apostrophe");
    ok(!chk("caf\xc3\xa9"), "non-ASCII");
    ok(!chk("abcdefghijklmnopqrstuvwxyz"), "too long");
}

static void testSuggest(const spell::Dict &d) {
    okSugg(d, "teh", "the ten eh");        // swap first, then by frequency
    okSugg(d, "Teh", "The Ten Eh");        // case carried over
    okSugg(d, "TEH", "THE TEN EH");
    okSugg(d, "recieve", "receive relieve believe");
    okSugg(d, "repeter", "repeater peter");
    okSugg(d, "mesage", "message messages manage");   // one edit, then two
    okSugg(d, "im", "I'm it is");          // apostrophe match first, capital I
    okSugg(d, "dont", "don't done on");
    okSugg(d, "nodez", "node nodes none");
    okSugg(d, "qqqqqqq", "");              // nothing close
    // Short words get one edit, not two: "xy" would otherwise match half the
    // two-letter words.
    ok(sugg(d, "zeb").empty(), "no two-edit matches for three letters");
    okSugg(d, "zebr", "zebra");
    okSugg(d, "internationalisation", "internationalization");

    char out[spell::kMaxSuggest][spell::kMaxWord + 1];
    ok(spell::suggest(d, "teh", 3, out, 1) == 1 && !strcmp(out[0], "the"),
       "maxOut limits the count and keeps the best");
    ok(spell::suggest(d, "teh", 3, out, 0) == 0, "maxOut 0");
    const char *longWord = "abcdefghijklmnopqrstuvwx";
    ok(spell::suggest(d, longWord, strlen(longWord), out, 3) == 0, "too long to suggest for");
}

int main() {
    Built b;
    build(b);
    testKnown(b.dict);
    testShouldCheck();
    testSuggest(b.dict);

    printf("%s  %d checks, %d failed\n", g_fail ? "FAILED" : "ok", g_run, g_fail);
    return g_fail ? 1 : 0;
}
