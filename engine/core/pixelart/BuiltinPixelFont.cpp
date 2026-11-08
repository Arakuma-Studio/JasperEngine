// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

#include "pixelart/BuiltinPixelFont.h"

#include <algorithm>
#include <cstring>

namespace jasperengine::pixelart {

    namespace{
        // rows top to bottom, space separated; capitals and digits stand on row 6 (the baseline is
        // 7), lowercase bodies span rows 2-6 and descenders rows 7-8. Each glyph starts at column 0:
        // its advance is measured (rightmost ink column + 1)
        struct GlyphArt {
            uint32_t codepoint;
            const char* rows;
        };

        const GlyphArt kGlyphs[] = {
            {'!', "# # # # # . #"},
            {'"', "#.# #.#"},
            {'#', "..... .#.#. ##### .#.#. ##### .#.#."},
            {'$', "..#.. .#### #.#.. .###. ..#.# ####. ..#.."},
            {'%', "##... ##..# ...#. ..#.. .#... #..## ...##"},
            {'&', ".##.. #..#. #.#.. .#... #.#.# #..#. .##.#"},
            {'\'', "# #"},
            {'(', ".# #. #. #. #. #. .#"},
            {')', "#. .# .# .# .# .# #."},
            {'*', "..... ..#.. #.#.# .###. #.#.# ..#.."},
            {'+', "..... ..#.. ..#.. ##### ..#.. ..#.."},
            {',', ".. .. .. .. .. .# .# #."},
            {'-', ".... .... .... ####"},
            {'.', ". . . . . . #"},
            {'/', "....# ...#. ...#. ..#.. .#... .#... #...."},
            {'0', ".###. #...# #..## #.#.# ##..# #...# .###."},
            {'1', ".#. ##. .#. .#. .#. .#. ###"},
            {'2', ".###. #...# ....# ...#. ..#.. .#... #####"},
            {'3', "##### ...#. ..#.. ...#. ....# #...# .###."},
            {'4', "...#. ..##. .#.#. #..#. ##### ...#. ...#."},
            {'5', "##### #.... ####. ....# ....# #...# .###."},
            {'6', "..##. .#... #.... ####. #...# #...# .###."},
            {'7', "##### ....# ...#. ..#.. .#... .#... .#..."},
            {'8', ".###. #...# #...# .###. #...# #...# .###."},
            {'9', ".###. #...# #...# .#### ....# ...#. .##.."},
            {':', ". . # . . # ."},
            {';', ".. .. .# .. .. .# .# #."},
            {'<', "...# ..#. .#.. #... .#.. ..#. ...#"},
            {'=', "..... ..... ##### ..... #####"},
            {'>', "#... .#.. ..#. ...# ..#. .#.. #..."},
            {'?', ".###. #...# ....# ...#. ..#.. ..... ..#.."},
            {'@', ".###. #...# #.### #.#.# #.### #.... .####"},
            {'A', ".###. #...# #...# ##### #...# #...# #...#"},
            {'B', "####. #...# #...# ####. #...# #...# ####."},
            {'C', ".###. #...# #.... #.... #.... #...# .###."},
            {'D', "####. #...# #...# #...# #...# #...# ####."},
            {'E', "##### #.... #.... ####. #.... #.... #####"},
            {'F', "##### #.... #.... ####. #.... #.... #...."},
            {'G', ".###. #...# #.... #.### #...# #...# .####"},
            {'H', "#...# #...# #...# ##### #...# #...# #...#"},
            {'I', "### .#. .#. .#. .#. .#. ###"},
            {'J', "..### ...#. ...#. ...#. ...#. #..#. .##.."},
            {'K', "#...# #..#. #.#.. ##... #.#.. #..#. #...#"},
            {'L', "#.... #.... #.... #.... #.... #.... #####"},
            {'M', "#...# ##.## #.#.# #.#.# #...# #...# #...#"},
            {'N', "#...# #...# ##..# #.#.# #..## #...# #...#"},
            {'O', ".###. #...# #...# #...# #...# #...# .###."},
            {'P', "####. #...# #...# ####. #.... #.... #...."},
            {'Q', ".###. #...# #...# #...# #.#.# #..#. .##.#"},
            {'R', "####. #...# #...# ####. #.#.. #..#. #...#"},
            {'S', ".#### #.... #.... .###. ....# ....# ####."},
            {'T', "##### ..#.. ..#.. ..#.. ..#.. ..#.. ..#.."},
            {'U', "#...# #...# #...# #...# #...# #...# .###."},
            {'V', "#...# #...# #...# #...# #...# .#.#. ..#.."},
            {'W', "#...# #...# #...# #.#.# #.#.# #.#.# .#.#."},
            {'X', "#...# #...# .#.#. ..#.. .#.#. #...# #...#"},
            {'Y', "#...# #...# .#.#. ..#.. ..#.. ..#.. ..#.."},
            {'Z', "##### ....# ...#. ..#.. .#... #.... #####"},
            {'[', "## #. #. #. #. #. ##"},
            {'\\', "#.... .#... .#... ..#.. ...#. ...#. ....#"},
            {']', "## .# .# .# .# .# ##"},
            {'^', "..#.. .#.#. #...#"},
            {'_', "..... ..... ..... ..... ..... ..... ..... #####"},
            {'`', "#. .#"},
            {'a', "..... ..... .###. ....# .#### #...# .####"},
            {'b', "#.... #.... ####. #...# #...# #...# ####."},
            {'c', ".... .... .### #... #... #... .###"},
            {'d', "....# ....# .#### #...# #...# #...# .####"},
            {'e', "..... ..... .###. #...# ##### #.... .###."},
            {'f', "..## .#.. .#.. ###. .#.. .#.. .#.."},
            {'g', "..... ..... .#### #...# #...# #...# .#### ....# .###."},
            {'h', "#.... #.... ####. #...# #...# #...# #...#"},
            {'i', "# . # # # # #"},
            {'j', "..# ... ..# ..# ..# ..# ..# #.# .#."},
            {'k', "#... #... #..# #.#. ##.. #.#. #..#"},
            {'l', "#. #. #. #. #. #. .#"},
            {'m', "..... ..... ##.#. #.#.# #.#.# #.#.# #.#.#"},
            {'n', ".... .... ###. #..# #..# #..# #..#"},
            {'o', ".... .... .##. #..# #..# #..# .##."},
            {'p', ".... .... ###. #..# #..# #..# ###. #... #..."},
            {'q', ".... .... .### #..# #..# #..# .### ...# ...#"},
            {'r', ".... .... #.## ##.. #... #... #..."},
            {'s', ".... .... .### #... .##. ...# ###."},
            {'t', ".#.. .#.. ###. .#.. .#.. .#.. ..##"},
            {'u', ".... .... #..# #..# #..# #..# .###"},
            {'v', "..... ..... #...# #...# #...# .#.#. ..#.."},
            {'w', "..... ..... #...# #...# #.#.# #.#.# .#.#."},
            {'x', "..... ..... #...# .#.#. ..#.. .#.#. #...#"},
            {'y', ".... .... #..# #..# #..# #..# .### ...# .##."},
            {'z', ".... .... #### ...# ..#. .#.. ####"},
            {'{', "..# .#. .#. #.. .#. .#. ..#"},
            {'|', "# # # # # # #"},
            {'}', "#.. .#. .#. ..# .#. .#. #.."},
            {'~', "..... ..... .#... #.#.# ...#."},
            // Latin-1 letters no accent mark makes
            {0xC5, "..#.. .#.#. .###. #...# ##### #...# #...#"},
            {0xC6, ".#### #.#.. #.#.. ##### #.#.. #.#.. #.###"},
            {0xC7, ".###. #...# #.... #.... #.... #...# .###. ..#.. .##.."},
            {0xD0, "####. .#..# .#..# ###.# .#..# .#..# ####."},
            {0xD7, "..... #...# .#.#. ..#.. .#.#. #...#"},
            {0xD8, ".#### #..## #.#.# #.#.# #.#.# ##..# ####."},
            {0xDE, "#.... ####. #...# #...# ####. #.... #...."},
            {0xDF, ".##.. #..#. #..#. #.#.. #..#. #...# #.##."},
            {0xE5, "..#.. .#.#. .###. ....# .#### #...# .####"},
            {0xE6, "..... ..... ##.#. ..#.# .#### #.#.. .#.##"},
            {0xE7, ".... .... .### #... #... #... .### ..#. .##."},
            {0xF0, ".#.#. ..#.. .#.#. ....# .#### #...# .###."},
            {0xF7, "..... ..#.. ..... ##### ..... ..#.."},
            {0xF8, "..... ..... .###. #..## #.#.# ##..# .###."},
            {0xFE, "#... #... ###. #..# #..# #..# ###. #... #..."},
        };

        // the accent marks, two rows above a body (rows 0-1)
        enum Mark { GRAVE, ACUTE, CIRCUMFLEX, TILDE, DIAERESIS };
        const char* const kMarks[] = {
            "#. .#",
            ".# #.",
            ".#. #.#",
            ".#.# #.#.",
            "#.# ...",
        };

        struct Composed {
            uint32_t codepoint;
            char base;
            Mark mark;
        };

        const Composed kComposed[] = {
            {0xC0, 'A', GRAVE}, {0xC1, 'A', ACUTE}, {0xC2, 'A', CIRCUMFLEX}, {0xC3, 'A', TILDE}, {0xC4, 'A', DIAERESIS},
            {0xC8, 'E', GRAVE}, {0xC9, 'E', ACUTE}, {0xCA, 'E', CIRCUMFLEX}, {0xCB, 'E', DIAERESIS},
            {0xCC, 'I', GRAVE}, {0xCD, 'I', ACUTE}, {0xCE, 'I', CIRCUMFLEX}, {0xCF, 'I', DIAERESIS},
            {0xD1, 'N', TILDE},
            {0xD2, 'O', GRAVE}, {0xD3, 'O', ACUTE}, {0xD4, 'O', CIRCUMFLEX}, {0xD5, 'O', TILDE}, {0xD6, 'O', DIAERESIS},
            {0xD9, 'U', GRAVE}, {0xDA, 'U', ACUTE}, {0xDB, 'U', CIRCUMFLEX}, {0xDC, 'U', DIAERESIS},
            {0xDD, 'Y', ACUTE},
            {0xE0, 'a', GRAVE}, {0xE1, 'a', ACUTE}, {0xE2, 'a', CIRCUMFLEX}, {0xE3, 'a', TILDE}, {0xE4, 'a', DIAERESIS},
            {0xE8, 'e', GRAVE}, {0xE9, 'e', ACUTE}, {0xEA, 'e', CIRCUMFLEX}, {0xEB, 'e', DIAERESIS},
            {0xEC, 'i', GRAVE}, {0xED, 'i', ACUTE}, {0xEE, 'i', CIRCUMFLEX}, {0xEF, 'i', DIAERESIS},
            {0xF1, 'n', TILDE},
            {0xF2, 'o', GRAVE}, {0xF3, 'o', ACUTE}, {0xF4, 'o', CIRCUMFLEX}, {0xF5, 'o', TILDE}, {0xF6, 'o', DIAERESIS},
            {0xF9, 'u', GRAVE}, {0xFA, 'u', ACUTE}, {0xFB, 'u', CIRCUMFLEX}, {0xFC, 'u', DIAERESIS},
            {0xFD, 'y', ACUTE}, {0xFF, 'y', DIAERESIS},
        };

        std::vector<std::string> splitRows(const char* text){
            std::vector<std::string> rows;
            std::string row;
            for (const char* c = text; ; c++){
                if (*c == ' ' || *c == '\0'){
                    rows.push_back(row);
                    row.clear();
                    if (*c == '\0') break;
                }else{
                    row += *c;
                }
            }
            return rows;
        }

        // 9 rows of 5 columns
        std::vector<std::string> normalize(std::vector<std::string> rows){
            rows.resize(kBuiltinCellHeight);
            for (std::string& row : rows){
                row.resize(5, '.');
                std::replace(row.begin(), row.end(), ' ', '.');
            }
            return rows;
        }

        std::vector<std::string> drawn(uint32_t codepoint){
            for (const GlyphArt& art : kGlyphs)
                if (art.codepoint == codepoint)
                    return normalize(splitRows(art.rows));
            return {};
        }

        int inkWidth(const std::vector<std::string>& rows){
            int right = -1;
            for (const std::string& row : rows)
                for (int x = 0; x < (int)row.size(); x++)
                    if (row[(size_t)x] == '#') right = std::max(right, x);
            return right + 1;
        }

        // a capital's 7 rows lose rows 1 and 5 and stand on rows 2-6; a lowercase body loses
        // what it has above row 2 (the dot of i); the mark takes rows 0-1, centred on the ink
        std::vector<std::string> compose(char base, Mark mark){
            std::vector<std::string> rows = drawn((uint32_t)(unsigned char)base);
            const bool capital = base >= 'A' && base <= 'Z';
            std::vector<std::string> out(kBuiltinCellHeight, std::string(5, '.'));
            if (capital){
                const int kept[] = {0, 2, 3, 4, 6};
                for (int i = 0; i < 5; i++) out[(size_t)(2 + i)] = rows[(size_t)kept[i]];
            }else{
                for (int y = 2; y < kBuiltinCellHeight; y++) out[(size_t)y] = rows[(size_t)y];
            }
            const std::vector<std::string> markRows = splitRows(kMarks[mark]);
            const int markWidth = (int)markRows[0].size();
            const int left = std::max(0, (inkWidth(rows) - markWidth) / 2);
            for (int y = 0; y < 2 && y < (int)markRows.size(); y++)
                for (int x = 0; x < markWidth && left + x < 5; x++)
                    if (markRows[(size_t)y][(size_t)x] == '#') out[(size_t)y][(size_t)(left + x)] = '#';
            return out;
        }
    }

    std::vector<std::string> builtinPixelGlyphRows(uint32_t codepoint){
        if (codepoint == ' ')
            return normalize({});
        for (const Composed& c : kComposed)
            if (c.codepoint == codepoint)
                return compose(c.base, c.mark);
        return drawn(codepoint);
    }

    RawFont builtinPixelRaw(){
        RawFont raw;
        raw.cellWidth = kBuiltinCellWidth;
        raw.cellHeight = kBuiltinCellHeight;
        raw.nativeSize = kBuiltinCellHeight;
        raw.baseline = kBuiltinBaseline;
        std::vector<uint32_t> codepoints;
        for (uint32_t c = 0x20; c <= 0x7E; c++) codepoints.push_back(c);
        for (uint32_t c = 0xC0; c <= 0xFF; c++) codepoints.push_back(c);
        raw.cellsTotal = (int)codepoints.size();
        int cell = 0;
        for (uint32_t c : codepoints){
            const std::vector<std::string> rows = builtinPixelGlyphRows(c);
            RawGlyph glyph;
            glyph.codepoint = c;
            glyph.cell = cell++;
            glyph.width = kBuiltinCellWidth;
            glyph.height = kBuiltinCellHeight;
            glyph.rgba.assign((size_t)kBuiltinCellWidth * kBuiltinCellHeight * 4, 0);
            for (int y = 0; y < (int)rows.size() && y < kBuiltinCellHeight; y++){
                for (int x = 0; x < 5; x++){
                    if (rows[(size_t)y][(size_t)x] != '#')
                        continue;
                    uint8_t* px = &glyph.rgba[((size_t)y * kBuiltinCellWidth + (size_t)x) * 4];
                    px[0] = px[1] = px[2] = px[3] = 255;
                }
            }
            raw.glyphs.push_back(std::move(glyph));
        }
        raw.detections.push_back("built-in 6x9 grid");
        return raw;
    }

}
