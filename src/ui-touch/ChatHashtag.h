// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <ctype.h>
#include <string.h>
#include "Utf8Text.h"

namespace ChatHashtag
{

    inline size_t wordSequence(const char *text, size_t remaining)
    {
        const uint8_t *bytes = reinterpret_cast<const uint8_t *>(text);
        const size_t sequence = Utf8Text::sequenceLength(bytes, remaining);
        if (!sequence)
            return 0;
        if (sequence == 1)
            return isalnum(bytes[0]) || bytes[0] == '-' || bytes[0] == '_' ? 1 : 0;
        uint32_t codepoint = bytes[0] & (0x7F >> sequence);
        for (size_t offset = 1; offset < sequence; ++offset)
            codepoint = (codepoint << 6) | (bytes[offset] & 0x3F);
        const bool latin = (codepoint >= 0x00C0 && codepoint <= 0x02AF &&
                            codepoint != 0x00D7 && codepoint != 0x00F7) ||
                           (codepoint >= 0x1E00 && codepoint <= 0x1EFF);
        const bool combining = codepoint >= 0x0300 && codepoint <= 0x036F;
        return latin || combining ? sequence : 0;
    }

    inline bool isHexColor(const char *text, int start, int end)
    {
        const int length = end - start;
        if (length != 3 && length != 4 && length != 6 && length != 8)
            return false;
        for (int offset = start; offset < end; ++offset)
            if (!isxdigit((unsigned char)text[offset]))
                return false;
        return true;
    }

    inline bool span(const char *text, int from, int *start, int *end)
    {
        if (!text)
            return false;
        const size_t length = strlen(text);
        for (int offset = 0; offset < (int)length; ++offset)
        {
            if (text[offset] == '@' && text[offset + 1] == '[')
            {
                const char *close = strchr(text + offset + 2, ']');
                if (!close)
                    return false;
                offset = (int)(close - text);
                continue;
            }
            if (offset < from || text[offset] != '#')
                continue;
            if (offset > 0)
            {
                int previous = offset - 1;
                while (previous > 0 && Utf8Text::continuation((uint8_t)text[previous]))
                    --previous;
                if ((text[previous] != '-' && wordSequence(text + previous, offset - previous)) ||
                    text[offset - 1] == '/' || text[offset - 1] == '#')
                    continue;
            }
            int finish = offset + 1;
            while (finish < (int)length)
            {
                const size_t sequence = wordSequence(text + finish, length - finish);
                if (!sequence)
                    break;
                finish += (int)sequence;
            }
            if ((unsigned char)text[finish] >= 0x80)
            {
                offset = finish;
                continue;
            }
            if (finish == offset + 1 || finish - offset >= 32 ||
                isdigit((unsigned char)text[offset + 1]) ||
                isHexColor(text, offset + 1, finish))
                continue;
            *start = offset;
            *end = finish;
            return true;
        }
        return false;
    }

}