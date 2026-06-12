/*******************************************************************************
 * KeyboardLayouts.cpp — keyboard map data for on-screen + T-Deck physical input
 *
 * To add a new language later:
 *   1. Add an entry to KeyboardLayoutId in KeyboardLayouts.h
 *   2. Define OsKeyboardLayout and HwKeyboardLayout structs below
 *   3. Append them to k_os_layouts[] and k_hw_layouts[]
 *   4. Update KEYBOARD_LAYOUT_COUNT in KeyboardLayouts.h
 ******************************************************************************/
#include "KeyboardLayouts.h"

#if 1
#include "lvgl.h"
#else
#include "lvgl/lvgl.h"
#endif

/* ================================================================
 * On-screen keyboard (LVGL lv_keyboard) maps
 * ================================================================ */

/* Helper: build a control entry for a special key with a given width multiplier. */
static constexpr lv_btnmatrix_ctrl_t KC(uint8_t width_mul) {
    return LV_BTNMATRIX_CTRL_NO_REPEAT | (width_mul << 4);
}

/* ---------- Bulgarian on-screen keyboard ---------- */

/* Lower-case Cyrillic, 3 letter rows + 1 control row. */
static const char* kb_bg_lower[] = {
    "я","в","е","р","т","ъ","у","и","о","п","\n",
    "а","с","д","ф","г","х","й","к","л","ь","\n",
    "з","ц","ч","ж","б","н","м","ш","щ","ю","\n",
    LV_SYMBOL_UP, "1#", " ", LV_SYMBOL_BACKSPACE, LV_SYMBOL_OK, NULL
};

static const lv_btnmatrix_ctrl_t kb_bg_lower_ctrl[] = {
    0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,
    KC(2), KC(1), KC(4), KC(2), KC(1)
};

/* Upper-case Cyrillic. */
static const char* kb_bg_upper[] = {
    "Я","В","Е","Р","Т","Ъ","У","И","О","П","\n",
    "А","С","Д","Ф","Г","Х","Й","К","Л","Ь","\n",
    "З","Ц","Ч","Ж","Б","Н","М","Ш","Щ","Ю","\n",
    LV_SYMBOL_UP, "1#", " ", LV_SYMBOL_BACKSPACE, LV_SYMBOL_OK, NULL
};

static const lv_btnmatrix_ctrl_t kb_bg_upper_ctrl[] = {
    0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,
    KC(2), KC(1), KC(4), KC(2), KC(1)
};

/* ---------- Russian (ЙЦУКЕН) on-screen keyboard ---------- */
/* Standard Russian layout, 3 rows of 11 = all 33 letters. */
static const char* kb_ru_lower[] = {
    "й","ц","у","к","е","н","г","ш","щ","з","х","\n",
    "ф","ы","в","а","п","р","о","л","д","ж","э","\n",
    "я","ч","с","м","и","т","ь","б","ю","ъ","ё","\n",
    LV_SYMBOL_UP, "1#", " ", LV_SYMBOL_BACKSPACE, LV_SYMBOL_OK, NULL
};

static const lv_btnmatrix_ctrl_t kb_ru_lower_ctrl[] = {
    0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,
    KC(2), KC(1), KC(4), KC(2), KC(1)
};

static const char* kb_ru_upper[] = {
    "Й","Ц","У","К","Е","Н","Г","Ш","Щ","З","Х","\n",
    "Ф","Ы","В","А","П","Р","О","Л","Д","Ж","Э","\n",
    "Я","Ч","С","М","И","Т","Ь","Б","Ю","Ъ","Ё","\n",
    LV_SYMBOL_UP, "1#", " ", LV_SYMBOL_BACKSPACE, LV_SYMBOL_OK, NULL
};

static const lv_btnmatrix_ctrl_t kb_ru_upper_ctrl[] = {
    0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,
    KC(2), KC(1), KC(4), KC(2), KC(1)
};

/* ---------- Ukrainian on-screen keyboard ---------- */
/* Standard Ukrainian layout, 3 rows of 11 = all 33 letters. */
static const char* kb_uk_lower[] = {
    "й","ц","у","к","е","н","г","ш","щ","з","х","\n",
    "ф","і","в","а","п","р","о","л","д","ж","є","\n",
    "я","ч","с","м","и","т","ь","б","ю","ї","ґ","\n",
    LV_SYMBOL_UP, "1#", " ", LV_SYMBOL_BACKSPACE, LV_SYMBOL_OK, NULL
};
static const lv_btnmatrix_ctrl_t kb_uk_lower_ctrl[] = {
    0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,
    KC(2), KC(1), KC(4), KC(2), KC(1)
};
static const char* kb_uk_upper[] = {
    "Й","Ц","У","К","Е","Н","Г","Ш","Щ","З","Х","\n",
    "Ф","І","В","А","П","Р","О","Л","Д","Ж","Є","\n",
    "Я","Ч","С","М","И","Т","Ь","Б","Ю","Ї","Ґ","\n",
    LV_SYMBOL_UP, "1#", " ", LV_SYMBOL_BACKSPACE, LV_SYMBOL_OK, NULL
};
static const lv_btnmatrix_ctrl_t kb_uk_upper_ctrl[] = {
    0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,
    KC(2), KC(1), KC(4), KC(2), KC(1)
};

/* ---------- Serbian (Cyrillic) on-screen keyboard ---------- */
/* Serbian azbuka, 3 rows of 10 = all 30 letters. */
static const char* kb_sr_lower[] = {
    "љ","њ","е","р","т","з","у","и","о","п","\n",
    "а","с","д","ф","г","х","ј","к","л","ч","\n",
    "ж","џ","ц","в","б","н","м","ђ","ћ","ш","\n",
    LV_SYMBOL_UP, "1#", " ", LV_SYMBOL_BACKSPACE, LV_SYMBOL_OK, NULL
};
static const lv_btnmatrix_ctrl_t kb_sr_lower_ctrl[] = {
    0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,
    KC(2), KC(1), KC(4), KC(2), KC(1)
};
static const char* kb_sr_upper[] = {
    "Љ","Њ","Е","Р","Т","З","У","И","О","П","\n",
    "А","С","Д","Ф","Г","Х","Ј","К","Л","Ч","\n",
    "Ж","Џ","Ц","В","Б","Н","М","Ђ","Ћ","Ш","\n",
    LV_SYMBOL_UP, "1#", " ", LV_SYMBOL_BACKSPACE, LV_SYMBOL_OK, NULL
};
static const lv_btnmatrix_ctrl_t kb_sr_upper_ctrl[] = {
    0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,
    KC(2), KC(1), KC(4), KC(2), KC(1)
};

/* ---------- Greek on-screen keyboard ---------- */
/* ΕΛΟΤ-style positions; 9+9+7 = all 24 letters (+ final sigma ς). */
static const char* kb_el_lower[] = {
    "ς","ε","ρ","τ","υ","θ","ι","ο","π","\n",
    "α","σ","δ","φ","γ","η","ξ","κ","λ","\n",
    "ζ","χ","ψ","ω","β","ν","μ","\n",
    LV_SYMBOL_UP, "1#", " ", LV_SYMBOL_BACKSPACE, LV_SYMBOL_OK, NULL
};
static const lv_btnmatrix_ctrl_t kb_el_lower_ctrl[] = {
    0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,
    KC(2), KC(1), KC(4), KC(2), KC(1)
};
static const char* kb_el_upper[] = {
    "Σ","Ε","Ρ","Τ","Υ","Θ","Ι","Ο","Π","\n",
    "Α","Σ","Δ","Φ","Γ","Η","Ξ","Κ","Λ","\n",
    "Ζ","Χ","Ψ","Ω","Β","Ν","Μ","\n",
    LV_SYMBOL_UP, "1#", " ", LV_SYMBOL_BACKSPACE, LV_SYMBOL_OK, NULL
};
static const lv_btnmatrix_ctrl_t kb_el_upper_ctrl[] = {
    0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,
    KC(2), KC(1), KC(4), KC(2), KC(1)
};

/* ---------- Arabic on-screen keyboard (RTL) ---------- */
/* Arabic-101 letter positions, 3 rows of 11 = all 28 letters + ة ى ء ئ ؤ و.
 * Arabic is unicameral, so the same map serves LOWER and UPPER. The text
 * field renders RTL + shaped via LV_USE_BIDI / LV_USE_ARABIC_PERSIAN_CHARS. */
static const char* kb_ar_lower[] = {
    "ض","ص","ث","ق","ف","غ","ع","ه","خ","ح","ج","\n",
    "ش","س","ي","ب","ل","ا","ت","ن","م","ك","ط","\n",
    "ئ","ء","ؤ","ر","ى","ة","و","ز","ظ","ذ","د","\n",
    LV_SYMBOL_UP, "1#", " ", LV_SYMBOL_BACKSPACE, LV_SYMBOL_OK, NULL
};
static const lv_btnmatrix_ctrl_t kb_ar_lower_ctrl[] = {
    0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,
    KC(2), KC(1), KC(4), KC(2), KC(1)
};

/* ---------- English on-screen keyboard ---------- */
/* EXACT verbatim copy of LVGL's built-in default keyboard — the layout the alpha
 * builds shipped before the multi-language maps existed (working "ABC" shift, the
 * 1# symbols key, full punctuation row, magnified key-press preview). lv_keyboard
 * keeps the maps in a GLOBAL kb_map[] that lv_keyboard_set_map overwrites in place,
 * so once a non-Latin layout is applied the stock map is gone — we re-apply this
 * copy to restore it when the user cycles back to English. EN_KB_BTN mirrors the
 * library's private LV_KB_BTN (popover preview + width). */
#define EN_KB_BTN(w) (LV_BTNMATRIX_CTRL_POPOVER | (w))
static const char* kb_en_lower[] = {
    "1#", "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", LV_SYMBOL_BACKSPACE, "\n",
    "ABC", "a", "s", "d", "f", "g", "h", "j", "k", "l", LV_SYMBOL_NEW_LINE, "\n",
    "_", "-", "z", "x", "c", "v", "b", "n", "m", ".", ",", ":", "\n",
    LV_SYMBOL_KEYBOARD, LV_SYMBOL_LEFT, " ", LV_SYMBOL_RIGHT, LV_SYMBOL_OK, ""
};

static const lv_btnmatrix_ctrl_t kb_en_lower_ctrl[] = {
    LV_KEYBOARD_CTRL_BTN_FLAGS | 5, EN_KB_BTN(4), EN_KB_BTN(4), EN_KB_BTN(4), EN_KB_BTN(4), EN_KB_BTN(4), EN_KB_BTN(4), EN_KB_BTN(4), EN_KB_BTN(4), EN_KB_BTN(4), EN_KB_BTN(4), LV_BTNMATRIX_CTRL_CHECKED | 7,
    LV_KEYBOARD_CTRL_BTN_FLAGS | 6, EN_KB_BTN(3), EN_KB_BTN(3), EN_KB_BTN(3), EN_KB_BTN(3), EN_KB_BTN(3), EN_KB_BTN(3), EN_KB_BTN(3), EN_KB_BTN(3), EN_KB_BTN(3), LV_BTNMATRIX_CTRL_CHECKED | 7,
    LV_BTNMATRIX_CTRL_CHECKED | EN_KB_BTN(1), LV_BTNMATRIX_CTRL_CHECKED | EN_KB_BTN(1), EN_KB_BTN(1), EN_KB_BTN(1), EN_KB_BTN(1), EN_KB_BTN(1), EN_KB_BTN(1), EN_KB_BTN(1), EN_KB_BTN(1), LV_BTNMATRIX_CTRL_CHECKED | EN_KB_BTN(1), LV_BTNMATRIX_CTRL_CHECKED | EN_KB_BTN(1), LV_BTNMATRIX_CTRL_CHECKED | EN_KB_BTN(1),
    LV_KEYBOARD_CTRL_BTN_FLAGS | 2, LV_BTNMATRIX_CTRL_CHECKED | 2, 6, LV_BTNMATRIX_CTRL_CHECKED | 2, LV_KEYBOARD_CTRL_BTN_FLAGS | 2
};

static const char* kb_en_upper[] = {
    "1#", "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", LV_SYMBOL_BACKSPACE, "\n",
    "abc", "A", "S", "D", "F", "G", "H", "J", "K", "L", LV_SYMBOL_NEW_LINE, "\n",
    "_", "-", "Z", "X", "C", "V", "B", "N", "M", ".", ",", ":", "\n",
    LV_SYMBOL_KEYBOARD, LV_SYMBOL_LEFT, " ", LV_SYMBOL_RIGHT, LV_SYMBOL_OK, ""
};

static const lv_btnmatrix_ctrl_t kb_en_upper_ctrl[] = {
    LV_KEYBOARD_CTRL_BTN_FLAGS | 5, EN_KB_BTN(4), EN_KB_BTN(4), EN_KB_BTN(4), EN_KB_BTN(4), EN_KB_BTN(4), EN_KB_BTN(4), EN_KB_BTN(4), EN_KB_BTN(4), EN_KB_BTN(4), EN_KB_BTN(4), LV_BTNMATRIX_CTRL_CHECKED | 7,
    LV_KEYBOARD_CTRL_BTN_FLAGS | 6, EN_KB_BTN(3), EN_KB_BTN(3), EN_KB_BTN(3), EN_KB_BTN(3), EN_KB_BTN(3), EN_KB_BTN(3), EN_KB_BTN(3), EN_KB_BTN(3), EN_KB_BTN(3), LV_BTNMATRIX_CTRL_CHECKED | 7,
    LV_BTNMATRIX_CTRL_CHECKED | EN_KB_BTN(1), LV_BTNMATRIX_CTRL_CHECKED | EN_KB_BTN(1), EN_KB_BTN(1), EN_KB_BTN(1), EN_KB_BTN(1), EN_KB_BTN(1), EN_KB_BTN(1), EN_KB_BTN(1), EN_KB_BTN(1), LV_BTNMATRIX_CTRL_CHECKED | EN_KB_BTN(1), LV_BTNMATRIX_CTRL_CHECKED | EN_KB_BTN(1), LV_BTNMATRIX_CTRL_CHECKED | EN_KB_BTN(1),
    LV_KEYBOARD_CTRL_BTN_FLAGS | 2, LV_BTNMATRIX_CTRL_CHECKED | 2, 6, LV_BTNMATRIX_CTRL_CHECKED | 2, LV_KEYBOARD_CTRL_BTN_FLAGS | 2
};

struct OsKeyboardLayout {
    KeyboardLayoutId id;
    const char*      name;
    const char**     lower_map;
    const lv_btnmatrix_ctrl_t* lower_ctrl;
    const char**     upper_map;
    const lv_btnmatrix_ctrl_t* upper_ctrl;
};

static const OsKeyboardLayout k_os_layouts[] = {
    { KeyboardLayoutId::EN, "EN",
      kb_en_lower, kb_en_lower_ctrl,
      kb_en_upper, kb_en_upper_ctrl },
    { KeyboardLayoutId::BG, "BG",
      kb_bg_lower, kb_bg_lower_ctrl,
      kb_bg_upper, kb_bg_upper_ctrl },
    { KeyboardLayoutId::RU, "RU",
      kb_ru_lower, kb_ru_lower_ctrl,
      kb_ru_upper, kb_ru_upper_ctrl },
    { KeyboardLayoutId::UK, "UK",
      kb_uk_lower, kb_uk_lower_ctrl,
      kb_uk_upper, kb_uk_upper_ctrl },
    { KeyboardLayoutId::SR, "SR",
      kb_sr_lower, kb_sr_lower_ctrl,
      kb_sr_upper, kb_sr_upper_ctrl },
    { KeyboardLayoutId::EL, "EL",
      kb_el_lower, kb_el_lower_ctrl,
      kb_el_upper, kb_el_upper_ctrl },
    { KeyboardLayoutId::AR, "AR",
      kb_ar_lower, kb_ar_lower_ctrl,
      kb_ar_lower, kb_ar_lower_ctrl },   // unicameral: same map for both
};

/* ================================================================
 * T-Deck physical keyboard (phonetic transliteration)
 * ================================================================ */

struct HwKeyboardLayout {
    KeyboardLayoutId id;
    const char* name;
};

static const HwKeyboardLayout k_hw_layouts[] = {
    { KeyboardLayoutId::EN, "EN" },
    { KeyboardLayoutId::BG, "BG" },
    { KeyboardLayoutId::RU, "RU" },
    { KeyboardLayoutId::UK, "UK" },
    { KeyboardLayoutId::SR, "SR" },
    { KeyboardLayoutId::EL, "EL" },
    { KeyboardLayoutId::AR, "AR" },
};

/* Bulgarian phonetic mapping for T-Deck.
 * Index 0 = 'a' / 'A', 1 = 'b' / 'B', etc.
 * nullptr = pass through unchanged (not a mapped letter). */
static const char* hw_bg_lower[26] = {
    "а", "б", "ц", "д", "е", "ф", "г", "х", "и", "й",
    "к", "л", "м", "н", "о", "п", "я", "р", "с", "т",
    "у", "ж", "в", "ь", "ъ", "з"
};

static const char* hw_bg_upper[26] = {
    "А", "Б", "Ц", "Д", "Е", "Ф", "Г", "Х", "И", "Й",
    "К", "Л", "М", "Н", "О", "П", "Я", "Р", "С", "Т",
    "У", "Ж", "В", "Ь", "Ъ", "З"
};

/* Digits 1-4 map to Cyrillic letters; 5-9,0 pass through as numbers.
 * Note: 'ю' replaces the earlier Russian 'э' so the Bulgarian alphabet
 * is fully covered (30 letters). */
static const char* hw_bg_digits[10] = {
    /* 0 */ nullptr,
    /* 1 */ "ш",
    /* 2 */ "щ",
    /* 3 */ "ч",
    /* 4 */ "ю",
    /* 5 */ nullptr,
    /* 6 */ nullptr,
    /* 7 */ nullptr,
    /* 8 */ nullptr,
    /* 9 */ nullptr
};

static const char* hw_bg_digits_shift[10] = {
    /* 0 */ nullptr,
    /* 1 */ "Ш",
    /* 2 */ "Щ",
    /* 3 */ "Ч",
    /* 4 */ "Ю",
    /* 5 */ nullptr,
    /* 6 */ nullptr,
    /* 7 */ nullptr,
    /* 8 */ nullptr,
    /* 9 */ nullptr
};

/* Russian phonetic mapping for T-Deck (homophonic transliteration).
 * a..z -> nearest-sounding Cyrillic; the 7 letters with no Latin homophone
 * (ч щ ъ ь э ю ё) ride on digit keys 1-7. All 33 letters reachable. */
static const char* hw_ru_lower[26] = {
    "а", "б", "ц", "д", "е", "ф", "г", "х", "и", "й",
    "к", "л", "м", "н", "о", "п", "я", "р", "с", "т",
    "у", "в", "ш", "ж", "ы", "з"
};

static const char* hw_ru_upper[26] = {
    "А", "Б", "Ц", "Д", "Е", "Ф", "Г", "Х", "И", "Й",
    "К", "Л", "М", "Н", "О", "П", "Я", "Р", "С", "Т",
    "У", "В", "Ш", "Ж", "Ы", "З"
};

static const char* hw_ru_digits[10] = {
    /* 0 */ nullptr,
    /* 1 */ "ч",
    /* 2 */ "щ",
    /* 3 */ "ъ",
    /* 4 */ "ь",
    /* 5 */ "э",
    /* 6 */ "ю",
    /* 7 */ "ё",
    /* 8 */ nullptr,
    /* 9 */ nullptr
};

static const char* hw_ru_digits_shift[10] = {
    /* 0 */ nullptr,
    /* 1 */ "Ч",
    /* 2 */ "Щ",
    /* 3 */ "Ъ",
    /* 4 */ "Ь",
    /* 5 */ "Э",
    /* 6 */ "Ю",
    /* 7 */ "Ё",
    /* 8 */ nullptr,
    /* 9 */ nullptr
};

/* Ukrainian phonetic for T-Deck (best-effort; g=ґ, h=г, x=х, y=и, i=і).
 * є ж ї ч ш щ ь ю ride digits 1-8. */
static const char* hw_uk_lower[26] = {
    "а", "б", "ц", "д", "е", "ф", "ґ", "г", "і", "й",
    "к", "л", "м", "н", "о", "п", "я", "р", "с", "т",
    "у", "в", "в", "х", "и", "з"
};
static const char* hw_uk_upper[26] = {
    "А", "Б", "Ц", "Д", "Е", "Ф", "Ґ", "Г", "І", "Й",
    "К", "Л", "М", "Н", "О", "П", "Я", "Р", "С", "Т",
    "У", "В", "В", "Х", "И", "З"
};
static const char* hw_uk_digits[10] = {
    nullptr, "ж", "ч", "ш", "щ", "є", "ї", "ь", "ю", nullptr
};
static const char* hw_uk_digits_shift[10] = {
    nullptr, "Ж", "Ч", "Ш", "Щ", "Є", "Ї", "Ь", "Ю", nullptr
};

/* Serbian phonetic for T-Deck (best-effort; Gaj's-Latin homophones, single key:
 * q=љ w=ш x=џ y=њ). ч ћ ђ ж ride digits 1-4. */
static const char* hw_sr_lower[26] = {
    "а", "б", "ц", "д", "е", "ф", "г", "х", "и", "ј",
    "к", "л", "м", "н", "о", "п", "љ", "р", "с", "т",
    "у", "в", "ш", "џ", "њ", "з"
};
static const char* hw_sr_upper[26] = {
    "А", "Б", "Ц", "Д", "Е", "Ф", "Г", "Х", "И", "Ј",
    "К", "Л", "М", "Н", "О", "П", "Љ", "Р", "С", "Т",
    "У", "В", "Ш", "Џ", "Њ", "З"
};
static const char* hw_sr_digits[10] = {
    nullptr, "ч", "ћ", "ђ", "ж", nullptr, nullptr, nullptr, nullptr, nullptr
};
static const char* hw_sr_digits_shift[10] = {
    nullptr, "Ч", "Ћ", "Ђ", "Ж", nullptr, nullptr, nullptr, nullptr, nullptr
};

/* Greek phonetic for T-Deck (ΕΛΟΤ Latin positions). All 24 letters fit a..z;
 * 'q' has no Greek letter (pass-through), 'w' = final sigma ς. */
static const char* hw_el_lower[26] = {
    "α", "β", "ψ", "δ", "ε", "φ", "γ", "η", "ι", "ξ",
    "κ", "λ", "μ", "ν", "ο", "π", nullptr, "ρ", "σ", "τ",
    "θ", "ω", "ς", "χ", "υ", "ζ"
};
static const char* hw_el_upper[26] = {
    "Α", "Β", "Ψ", "Δ", "Ε", "Φ", "Γ", "Η", "Ι", "Ξ",
    "Κ", "Λ", "Μ", "Ν", "Ο", "Π", nullptr, "Ρ", "Σ", "Τ",
    "Θ", "Ω", "Σ", "Χ", "Υ", "Ζ"
};
static const char* hw_el_digits[10]       = { nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr };
static const char* hw_el_digits_shift[10] = { nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr };

/* Arabic-101 physical positions for T-Deck (unicameral -> upper == lower).
 * a..z follow the standard Arabic keyboard; the 7 letters that live on
 * punctuation keys there (ج د ذ ز ط ظ و) ride digits 1-7 so they stay
 * reachable on the T-Deck's compact keyboard. EXPERIMENTAL. */
static const char* hw_ar_lower[26] = {
    "ش", "لا", "ؤ", "ي", "ث", "ب", "ل", "ا", "ه", "ت",
    "ن", "م", "ة", "ى", "خ", "ح", "ض", "ق", "س", "ف",
    "ع", "ر", "ص", "ء", "غ", "ئ"
};
static const char* hw_ar_digits[10] = {
    nullptr, "ج", "د", "ذ", "ز", "ط", "ظ", "و", nullptr, nullptr
};

/* ================================================================
 * Runtime state
 * ================================================================ */
static KeyboardLayoutId s_current_layout = KeyboardLayoutId::EN;
static uint32_t s_enabled_mask = 0;   // bit i = KeyboardLayoutId i is in the space-cycle (EN implicit)

/* ================================================================
 * Public API
 * ================================================================ */

const char* keyboardLayoutName(KeyboardLayoutId id) {
    if (static_cast<uint8_t>(id) >= KEYBOARD_LAYOUT_COUNT) id = KeyboardLayoutId::EN;
    return k_os_layouts[static_cast<int>(id)].name;
}

void keyboardLayoutsApply(lv_obj_t* keyboard, KeyboardLayoutId id) {
    if (!keyboard) return;
    if (static_cast<uint8_t>(id) >= KEYBOARD_LAYOUT_COUNT) id = KeyboardLayoutId::EN;
    s_current_layout = id;

  const OsKeyboardLayout& lo = k_os_layouts[static_cast<int>(id)];
  lv_keyboard_set_map(keyboard, LV_KEYBOARD_MODE_TEXT_LOWER, lo.lower_map, lo.lower_ctrl);
  lv_keyboard_set_map(keyboard, LV_KEYBOARD_MODE_TEXT_UPPER, lo.upper_map, lo.upper_ctrl);
}

KeyboardLayoutId keyboardLayoutsGetCurrent() {
    return s_current_layout;
}

KeyboardLayoutId keyboardLayoutsCycle(lv_obj_t* keyboard) {
    uint32_t mask = s_enabled_mask | 1u;   // English is always part of the cycle
    int cur = static_cast<int>(s_current_layout);
    // Walk forward from the current layout to the next enabled one, wrapping.
    for (int step = 1; step <= KEYBOARD_LAYOUT_COUNT; ++step) {
        int n = (cur + step) % KEYBOARD_LAYOUT_COUNT;
        if (mask & (1u << n)) {
            keyboardLayoutsApply(keyboard, static_cast<KeyboardLayoutId>(n));
            return s_current_layout;
        }
    }
    return s_current_layout;  // nothing enabled but EN — stay put
}

uint32_t keyboardLayoutsGetEnabledMask() { return s_enabled_mask; }

bool keyboardLayoutsAnySecondary() {
    return (s_enabled_mask & ~1u) != 0u;   // any non-English layout switched on
}

void keyboardLayoutsSetEnabledMask(uint32_t mask) {
    s_enabled_mask = mask & ((1u << KEYBOARD_LAYOUT_COUNT) - 1u);
    // If the active layout was just switched off, fall back to English.
    if (s_current_layout != KeyboardLayoutId::EN &&
        !(s_enabled_mask & (1u << static_cast<int>(s_current_layout)))) {
        s_current_layout = KeyboardLayoutId::EN;
    }
}

/* Per-layout physical-key tables. Index by KeyboardLayoutId. A null `lower`
 * means pass-through (English). Adding a language = add one row here plus its
 * four arrays above — no code changes. Keep this in enum order. */
struct HwPhoneticMap {
    const char* const* lower;          // 26, a..z
    const char* const* upper;          // 26, A..Z
    const char* const* digits;         // 10, '0'..'9'
    const char* const* digits_shift;   // 10, shifted number row
};
static const HwPhoneticMap k_hw_maps[KEYBOARD_LAYOUT_COUNT] = {
    /* EN */ { nullptr,      nullptr,      nullptr,        nullptr },
    /* BG */ { hw_bg_lower,  hw_bg_upper,  hw_bg_digits,   hw_bg_digits_shift },
    /* RU */ { hw_ru_lower,  hw_ru_upper,  hw_ru_digits,   hw_ru_digits_shift },
    /* UK */ { hw_uk_lower,  hw_uk_upper,  hw_uk_digits,   hw_uk_digits_shift },
    /* SR */ { hw_sr_lower,  hw_sr_upper,  hw_sr_digits,   hw_sr_digits_shift },
    /* EL */ { hw_el_lower,  hw_el_upper,  hw_el_digits,   hw_el_digits_shift },
    /* AR */ { hw_ar_lower,  hw_ar_lower,  hw_ar_digits,   hw_ar_digits },  // unicameral
};

const char* keyboardLayoutMapHwKey(KeyboardLayoutId id, int key, bool shifted) {
    uint8_t idx = static_cast<uint8_t>(id);
    if (idx >= KEYBOARD_LAYOUT_COUNT) return nullptr;
    const HwPhoneticMap& m = k_hw_maps[idx];
    if (!m.lower) return nullptr;  // English / pass-through

    if (key >= 'a' && key <= 'z') return m.lower[key - 'a'];
    if (key >= 'A' && key <= 'Z') return m.upper[key - 'A'];
    if (key >= '0' && key <= '9') {
        int d = key - '0';
        return shifted ? m.digits_shift[d] : m.digits[d];
    }
    /* A keyboard that sends the shifted symbol ('!' for Shift+1, etc.) instead
     * of the digit+shift flag: map the US-QWERTY top-row symbol to its digit
     * index and reuse the layout's shifted-digit glyph. */
    static const char sym[] = ")!@#$%^&*(";  // sym[d] == Shift+d
    for (int d = 0; d <= 9; ++d) {
        if (key == sym[d]) return m.digits_shift[d];
    }
    return nullptr;  /* pass through unchanged (space, punctuation, etc.) */
}
