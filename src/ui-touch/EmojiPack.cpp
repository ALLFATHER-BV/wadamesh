#include "EmojiPack.h"
#include <string.h>
#include <atomic>
#if defined(ESP32)
#include <esp_heap_caps.h>
#endif

// On-disk layout (little-endian throughout):
//
//   header  32 B : magic "WMEP", u16 version, u16 px, u32 glyph_count,
//                  u32 index_count, u32 items_off, u32 items_len,
//                  u32 blob_off, u32 blob_len
//   index        : index_count x 12 B, SORTED by cp and strictly increasing —
//                  u32 cp, u32 blob_rel_off (0xFFFFFFFF = zero-width), u8 cat,
//                  u8 flags, u16 reserved
//   items        : u16 count, then per item u8 cat, u8 utf8_len, utf8 bytes
//   blobs        : glyph_count x 768 B RGB565+alpha, same byte order as the
//                  baked glyphs in emoji_data.c ([lo, hi, alpha], swap=0)
//
// Multi-codepoint sequences (ZWJ, regional-indicator flag pairs) put the image
// on the LEAD codepoint and map every trailing codepoint to the zero-width
// entry, mirroring the SEQ/FLAGS trick in scripts/build/add-emoji.py.

namespace
{

    constexpr uint32_t kMagic = 0x50454D57u; // 'W','M','E','P'
    constexpr uint16_t kVersion = 1;
    constexpr uint16_t kPx = 16;
    constexpr uint32_t kBlobBytes = (uint32_t)kPx * kPx * 3;
    constexpr uint32_t kHeaderSize = 32;
    constexpr uint32_t kIndexEntry = 12;
    constexpr uint32_t kZeroOff = 0xFFFFFFFFu;
    // A skin-tone-free full Noto set is ~1.24 MB of blobs; the rest is headroom.
    constexpr uint32_t kMaxFile = 2u * 1024u * 1024u;
    constexpr uint32_t kMaxIndex = 8192;
    constexpr uint32_t kMaxItems = 4096;
    constexpr uint32_t kMaxItemLen = 32;
    constexpr uint32_t kMaxGlyphs = kMaxFile / kBlobBytes;

    uint8_t *s_file = nullptr;     // whole pack; every dsc points into it
    uint32_t *s_cp = nullptr;      // sorted codepoints, one per index entry
    lv_img_dsc_t *s_dsc = nullptr; // parallel descriptors
    char *s_strbuf = nullptr;      // picker strings, re-copied NUL-terminated
    const char **s_items = nullptr;
    uint8_t *s_cats = nullptr;

    // Published last, with release ordering: a reader that sees a non-zero count is
    // guaranteed to see the fully built tables above it.
    std::atomic<uint32_t> s_count{0};
    std::atomic<uint32_t> s_items_n{0};

    File s_in; // held open across pumps
    uint32_t s_want = 0;
    uint32_t s_got = 0;

    // 2x2 transparent, matching emoji_data.c's d_zero.
    const uint8_t s_zero_px[12] = {0};

    void *psAlloc(size_t n)
    {
        if (!n)
            return nullptr;
#if defined(ESP32)
        if (void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT))
            return p;
#endif
        return malloc(n);
    }

    void freeAll()
    {
        free(s_file);
        s_file = nullptr;
        free(s_cp);
        s_cp = nullptr;
        free(s_dsc);
        s_dsc = nullptr;
        free(s_strbuf);
        s_strbuf = nullptr;
        free(s_items);
        s_items = nullptr;
        free(s_cats);
        s_cats = nullptr;
        s_count.store(0, std::memory_order_release);
        s_items_n.store(0, std::memory_order_release);
    }

    inline uint32_t rd32(const uint8_t *p)
    {
        return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    }
    inline uint16_t rd16(const uint8_t *p) { return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8)); }

    // Picker strings reach lv_label and _lv_txt_encoded_next, which walk multi-byte
    // sequences without bounds checks — reject anything malformed before it lands.
    bool utf8Valid(const uint8_t *p, uint32_t n)
    {
        for (uint32_t i = 0; i < n;)
        {
            const uint8_t c = p[i];
            uint32_t extra, cp;
            if (c < 0x80)
            {
                extra = 0;
                cp = c;
            }
            else if (c < 0xC2)
                return false; // continuation byte or overlong 2-byte lead
            else if (c < 0xE0)
            {
                extra = 1;
                cp = c & 0x1F;
            }
            else if (c < 0xF0)
            {
                extra = 2;
                cp = c & 0x0F;
            }
            else if (c < 0xF5)
            {
                extra = 3;
                cp = c & 0x07;
            }
            else
                return false;
            if (i + extra + 1 > n)
                return false;
            for (uint32_t k = 1; k <= extra; ++k)
            {
                if ((p[i + k] & 0xC0) != 0x80)
                    return false;
                cp = (cp << 6) | (uint32_t)(p[i + k] & 0x3F);
            }
            if (cp > 0x10FFFF)
                return false;
            if (cp >= 0xD800 && cp <= 0xDFFF)
                return false;
            if (extra == 2 && cp < 0x800)
                return false; // overlong
            if (extra == 3 && cp < 0x10000)
                return false;
            i += extra + 1;
        }
        return true;
    }

} // namespace

// Validates the buffered file and builds the lookup tables. Nothing is published
// until both are complete, so a partial or hostile pack leaves no trace.
namespace
{
    bool parseLoaded(size_t size)
    {
        const uint8_t *h = s_file;
        if (rd32(h) != kMagic || rd16(h + 4) != kVersion || rd16(h + 6) != kPx)
        {
            freeAll();
            return false;
        }
        const uint32_t glyphs = rd32(h + 8);
        const uint32_t idxn = rd32(h + 12);
        const uint32_t ioff = rd32(h + 16);
        const uint32_t ilen = rd32(h + 20);
        const uint32_t boff = rd32(h + 24);
        const uint32_t blen = rd32(h + 28);

        if (glyphs == 0 || glyphs > kMaxGlyphs)
        {
            freeAll();
            return false;
        }
        if (idxn == 0 || idxn > kMaxIndex)
        {
            freeAll();
            return false;
        }
        // Division rather than glyphs * kBlobBytes so a hostile glyph_count can't wrap.
        if (blen % kBlobBytes || blen / kBlobBytes != glyphs)
        {
            freeAll();
            return false;
        }
        if (kHeaderSize + (uint64_t)idxn * kIndexEntry > size)
        {
            freeAll();
            return false;
        }
        if (ioff < kHeaderSize || (uint64_t)ioff + ilen > size)
        {
            freeAll();
            return false;
        }
        if (boff < kHeaderSize || (uint64_t)boff + blen > size)
        {
            freeAll();
            return false;
        }

        s_cp = (uint32_t *)psAlloc((size_t)idxn * sizeof(uint32_t));
        s_dsc = (lv_img_dsc_t *)psAlloc((size_t)idxn * sizeof(lv_img_dsc_t));
        if (!s_cp || !s_dsc)
        {
            freeAll();
            return false;
        }

        const uint8_t *ip = s_file + kHeaderSize;
        for (uint32_t i = 0; i < idxn; ++i, ip += kIndexEntry)
        {
            const uint32_t cp = rd32(ip);
            const uint32_t off = rd32(ip + 4);
            if (cp > 0x10FFFF)
            {
                freeAll();
                return false;
            }
            if (i && cp <= s_cp[i - 1])
            {
                freeAll();
                return false;
            } // the lookup binary-searches this
            s_cp[i] = cp;

            lv_img_dsc_t &d = s_dsc[i];
            memset(&d, 0, sizeof d);
            d.header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA;
            if (off == kZeroOff)
            {
                d.header.w = 2;
                d.header.h = 2;
                d.data_size = sizeof s_zero_px;
                d.data = s_zero_px;
            }
            else
            {
                if (off % kBlobBytes || off > blen - kBlobBytes)
                {
                    freeAll();
                    return false;
                }
                d.header.w = kPx;
                d.header.h = kPx;
                d.data_size = kBlobBytes;
                d.data = s_file + boff + off;
            }
        }

        if (ilen < 2)
        {
            freeAll();
            return false;
        }
        const uint8_t *it = s_file + ioff;
        const uint32_t n = rd16(it);
        if (n > kMaxItems)
        {
            freeAll();
            return false;
        }

        uint32_t pos = 2, bytes = 0;
        for (uint32_t i = 0; i < n; ++i)
        {
            if (pos + 2 > ilen)
            {
                freeAll();
                return false;
            }
            const uint32_t len = it[pos + 1];
            pos += 2;
            if (len == 0 || len > kMaxItemLen || pos + len > ilen)
            {
                freeAll();
                return false;
            }
            if (!utf8Valid(it + pos, len))
            {
                freeAll();
                return false;
            }
            pos += len;
            bytes += len + 1;
        }

        if (n)
        {
            s_strbuf = (char *)psAlloc(bytes);
            s_items = (const char **)psAlloc((size_t)n * sizeof(char *));
            s_cats = (uint8_t *)psAlloc(n);
            if (!s_strbuf || !s_items || !s_cats)
            {
                freeAll();
                return false;
            }
            char *w = s_strbuf;
            pos = 2;
            for (uint32_t i = 0; i < n; ++i)
            {
                const uint8_t cat = it[pos];
                const uint32_t len = it[pos + 1];
                pos += 2;
                memcpy(w, it + pos, len);
                w[len] = '\0';
                s_items[i] = w;
                s_cats[i] = cat;
                w += len + 1;
                pos += len;
            }
        }
        s_items_n.store(n, std::memory_order_release);
        s_count.store(idxn, std::memory_order_release);

        return true;
    }
} // namespace

bool emojiPackBegin(File f)
{
    freeAll();
    s_want = s_got = 0;
    if (!f)
        return false;
    const size_t size = f.size();
    if (size < kHeaderSize || size > kMaxFile)
    {
        f.close();
        return false;
    }
    s_file = (uint8_t *)psAlloc(size);
    if (!s_file)
    {
        f.close();
        return false;
    }
    s_in = f;
    s_want = (uint32_t)size;
    return true;
}

bool emojiPackPump(size_t budget)
{
    if (!s_want)
        return true;
    size_t left = s_want - s_got;
    if (budget > left)
        budget = left;
    while (budget)
    {
        const int n = s_in.read(s_file + s_got, budget);
        if (n <= 0)
        { // short read: the card went away mid-load
            s_in.close();
            s_want = 0;
            freeAll();
            return true;
        }
        s_got += (uint32_t)n;
        budget -= (size_t)n;
    }
    if (s_got < s_want)
        return false;
    s_in.close();
    parseLoaded(s_got);
    s_want = 0;
    return true;
}

bool emojiPackLoading() { return s_want != 0; }
bool emojiPackLoaded() { return s_count.load(std::memory_order_acquire) != 0; }

const lv_img_dsc_t *emojiPackLookup(uint32_t cp)
{
    const uint32_t n = s_count.load(std::memory_order_acquire);
    if (!n)
        return nullptr;
    int lo = 0, hi = (int)n - 1;
    while (lo <= hi)
    {
        const int mid = (lo + hi) >> 1;
        const uint32_t v = s_cp[mid];
        if (cp == v)
            return &s_dsc[mid];
        if (cp < v)
            hi = mid - 1;
        else
            lo = mid + 1;
    }
    return nullptr;
}

int emojiPackItemCount() { return (int)s_items_n.load(std::memory_order_acquire); }
const char *emojiPackItem(int i)
{
    const uint32_t n = s_items_n.load(std::memory_order_acquire);
    return (i >= 0 && (uint32_t)i < n) ? s_items[i] : nullptr;
}
uint8_t emojiPackItemCat(int i)
{
    const uint32_t n = s_items_n.load(std::memory_order_acquire);
    return (i >= 0 && (uint32_t)i < n) ? s_cats[i] : EMOJI_CAT_EXTRA;
}
