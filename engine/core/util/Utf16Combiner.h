// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef UTF16COMBINER_H
#define UTF16COMBINER_H

#include <cstdint>
#include <vector>

namespace jasperengine {

    // Combines character input that arrives one UTF-16 unit at a time (Windows WM_CHAR, ImGui's
    // 16-bit characters, wchar_t where it is 16 bits) into code points. A high surrogate waits
    // for its low one; a lone surrogate becomes U+FFFD. Units that are whole code points pass
    // through, so 32-bit wchar_t input is unchanged.
    class Utf16Combiner {
    public:
        static constexpr uint32_t REPLACEMENT = 0xFFFD;

        // one unit in, complete code points appended to out (zero, one or two)
        void push(uint32_t unit, std::vector<uint32_t>& out){
            if (unit >= 0xD800 && unit <= 0xDBFF){
                if (pendingHigh != 0)
                    out.push_back(REPLACEMENT);
                pendingHigh = unit;
                return;
            }

            if (unit >= 0xDC00 && unit <= 0xDFFF){
                if (pendingHigh != 0){
                    out.push_back(0x10000u + ((pendingHigh - 0xD800u) << 10) + (unit - 0xDC00u));
                    pendingHigh = 0;
                }else{
                    out.push_back(REPLACEMENT);
                }
                return;
            }

            if (pendingHigh != 0){
                out.push_back(REPLACEMENT);
                pendingHigh = 0;
            }
            out.push_back(unit);
        }

        // the input ended: a high surrogate still waiting becomes U+FFFD
        void flush(std::vector<uint32_t>& out){
            if (pendingHigh != 0){
                out.push_back(REPLACEMENT);
                pendingHigh = 0;
            }
        }

        bool hasPending() const{
            return pendingHigh != 0;
        }

        void reset(){
            pendingHigh = 0;
        }

    private:
        uint32_t pendingHigh = 0;
    };

}

#endif //UTF16COMBINER_H
