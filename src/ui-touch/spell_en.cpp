// The compiled-in English word list. Kept out of spell.cpp so the host tests
// can link the engine without the generated header. Only built where
// CAP_SPELLCHECK is on (device_caps.h): elsewhere nothing references it, and
// the ~115 KB list stays out of the image.
#include "device_caps.h"

#if CAP_SPELLCHECK
#include "spell.h"
#include "spell_dict_en.h"

namespace spell {

const Dict &english() {
    static const Dict d = {
        kSpellEnData, kSpellEnSize, kSpellEnBlocks,
        (uint32_t)(sizeof(kSpellEnBlocks) / sizeof(kSpellEnBlocks[0])), kSpellEnWords,
    };
    return d;
}

}  // namespace spell
#endif  // CAP_SPELLCHECK
