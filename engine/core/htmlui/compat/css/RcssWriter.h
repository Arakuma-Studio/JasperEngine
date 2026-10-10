// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COMPAT_CSS_RCSSWRITER_H
#define COMPAT_CSS_RCSSWRITER_H

#include "Export.h"
#include "htmlui/compat/CompatTypes.h"

#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace jasperengine::htmlui::compat {

    // One RCSS @media block condition: features joined by "and", with an optional leading
    // "not" (RmlUi 6.3 has no comma lists, media types or nesting; MediaQuery, p2-css, turns
    // a CSS query into one or more of these).
    struct RcssMediaCondition {
        bool negated = false;
        std::vector<std::string> features;   // "min-width: 600px", "theme: dark", "orientation: landscape"
        std::string raw;                      // used verbatim when not empty (pass-through)

        std::string text() const;             // "not (theme: dark) and (min-width: 600px)"
    };

    // Writes RCSS, one construct per line, '\n' line ends, no comments, and maps every
    // output line to the source of what it came from (RcssLineMap), so RmlUi's messages
    // about a line of the output point back at the user's file (RmlLogTranslator).
    class JASPERENGINE_API RcssWriter {
    public:
        enum class Mode {
            Sheet,             // rules, @media blocks, at-rules
            DeclarationList,   // style="": "name: value; name: value;" on one line
        };

        explicit RcssWriter(Mode mode = Mode::Sheet);

        // "@media <condition> {" (nesting is not allowed: an open block is closed first)
        void beginMedia(const RcssMediaCondition& condition, SourceRef ref = {});
        void endMedia();
        // "<selectors> {"
        void beginRule(std::string_view selectors, SourceRef ref);
        void endRule();
        // "name: value;"
        void declaration(std::string_view name, std::string_view value, SourceRef ref);
        // a whole at-rule block (@font-face, @keyframes, @decorator, ...); each of its lines
        // maps to ref
        void atRule(std::string_view text, SourceRef ref);

        bool inMedia() const { return mediaOpen; }
        bool inRule() const { return ruleOpen; }
        bool empty() const { return output.empty(); }
        uint32_t lineCount() const;          // lines written so far (Sheet mode)

        // The output (open blocks closed) and its line map; the writer is empty afterwards.
        std::string take(RcssLineMap& lines);
        std::string take();

    private:
        void line(std::string_view text, SourceRef ref);

        // the output: grown without being filled first (each byte is written once), handed
        // over as a string by take()
        class Output {
        public:
            char* extend(size_t count){
                if (size_ + count > capacity_)
                    grow(size_ + count);
                char* const at = data_.get() + size_;
                size_ += count;
                return at;
            }
            void append(std::string_view text){ std::memcpy(extend(text.size()), text.data(), text.size()); }
            void push(char c){ *extend(1) = c; }
            bool empty() const { return size_ == 0; }
            char back() const { return data_[size_ - 1]; }
            std::string take(){
                std::string text(data_.get(), size_);
                size_ = 0;
                return text;
            }

        private:
            void grow(size_t needed);

            std::unique_ptr<char[]> data_;
            size_t size_ = 0;
            size_t capacity_ = 0;
        };

        Mode mode;
        Output output;
        RcssLineMap map;
        bool mediaOpen = false;
        bool ruleOpen = false;
        SourceRef mediaRef, ruleRef;
    };

}

#endif //COMPAT_CSS_RCSSWRITER_H
