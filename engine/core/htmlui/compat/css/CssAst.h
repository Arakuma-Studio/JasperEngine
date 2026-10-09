// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef COMPAT_CSS_CSSAST_H
#define COMPAT_CSS_CSSAST_H

// Tokens, component values, declarations and rules of CSS Syntax Level 3 (phase-2 §3.9.1),
// with byte positions into the source file for diagnostics and line maps.

#include "htmlui/compat/Arena.h"
#include "htmlui/compat/CompatTypes.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace jasperengine::htmlui::compat {

    enum class TokenType : uint8_t { Ident, Function, AtKeyword, Hash, String, BadString, Url,
        BadUrl, Delim, Number, Percentage, Dimension, UnicodeRange, Whitespace, CDO, CDC, Colon,
        Semicolon, Comma, LeftBracket, RightBracket, LeftParen, RightParen, LeftBrace, RightBrace,
        Comment, EndOfFile };

    // The text of a token: bytes of the source or of the current Arena, never freed one by one
    // (16 bytes, copied as a pointer and a length). Made from a string it copies the string
    // into the current arena; Text::view() refers to bytes that outlive it (the parser's copy
    // of the source, literals). Reads as a std::string_view.
    class Text {
    public:
        constexpr Text() noexcept: data_(""), size_(0) {}
        Text(std::string_view text): data_(text.empty() ? "" : Arena::current().copy(text.data(), text.size())), size_((uint32_t)text.size()) {}
        Text(const std::string& text): Text(std::string_view(text)) {}
        Text(const char* text): Text(std::string_view(text)) {}
        static constexpr Text view(std::string_view text) noexcept { return Text(text.data(), (uint32_t)text.size()); }

        constexpr operator std::string_view() const noexcept { return std::string_view(data_, size_); }
        operator std::string() const { return std::string(data_, size_); }
        constexpr std::string_view view() const noexcept { return std::string_view(data_, size_); }
        std::string str() const { return std::string(data_, size_); }

        constexpr const char* data() const noexcept { return data_; }
        constexpr size_t size() const noexcept { return size_; }
        constexpr size_t length() const noexcept { return size_; }
        constexpr bool empty() const noexcept { return size_ == 0; }
        constexpr const char* begin() const noexcept { return data_; }
        constexpr const char* end() const noexcept { return data_ + size_; }
        constexpr char operator[](size_t index) const noexcept { return data_[index]; }
        constexpr char front() const noexcept { return data_[0]; }
        constexpr char back() const noexcept { return data_[size_ - 1]; }
        int compare(std::string_view other) const noexcept { return view().compare(other); }
        int compare(size_t position, size_t count, std::string_view other) const { return view().compare(position, count, other); }
        size_t find(std::string_view text, size_t position = 0) const noexcept { return view().find(text, position); }
        size_t find(char c, size_t position = 0) const noexcept { return view().find(c, position); }
        std::string substr(size_t position = 0, size_t count = std::string::npos) const { return std::string(view().substr(position, count)); }

        friend bool operator==(const Text& a, const Text& b) noexcept { return a.view() == b.view(); }
        friend bool operator==(const Text& a, std::string_view b) noexcept { return a.view() == b; }
        friend bool operator==(const Text& a, const std::string& b) noexcept { return a.view() == std::string_view(b); }
        friend bool operator==(const Text& a, const char* b) noexcept { return a.view() == std::string_view(b); }
        friend bool operator==(std::string_view a, const Text& b) noexcept { return a == b.view(); }
        friend bool operator==(const std::string& a, const Text& b) noexcept { return std::string_view(a) == b.view(); }
        friend bool operator==(const char* a, const Text& b) noexcept { return std::string_view(a) == b.view(); }
        friend bool operator!=(const Text& a, const Text& b) noexcept { return !(a == b); }
        friend bool operator!=(const Text& a, std::string_view b) noexcept { return !(a == b); }
        friend bool operator!=(const Text& a, const std::string& b) noexcept { return !(a == b); }
        friend bool operator!=(const Text& a, const char* b) noexcept { return !(a == b); }
        friend bool operator!=(std::string_view a, const Text& b) noexcept { return !(a == b); }
        friend bool operator!=(const std::string& a, const Text& b) noexcept { return !(a == b); }
        friend bool operator!=(const char* a, const Text& b) noexcept { return !(a == b); }
        friend bool operator<(const Text& a, const Text& b) noexcept { return a.view() < b.view(); }

        friend std::string operator+(const std::string& a, const Text& b){ std::string out(a); out.append(b.data_, b.size_); return out; }
        friend std::string operator+(std::string&& a, const Text& b){ a.append(b.data_, b.size_); return std::move(a); }
        friend std::string operator+(const char* a, const Text& b){ std::string out(a); out.append(b.data_, b.size_); return out; }
        friend std::string operator+(const Text& a, const std::string& b){ std::string out(a.data_, a.size_); out += b; return out; }
        friend std::string operator+(const Text& a, const char* b){ std::string out(a.data_, a.size_); out += b; return out; }
        friend std::string operator+(const Text& a, const Text& b){ std::string out(a.data_, a.size_); out.append(b.data_, b.size_); return out; }

    private:
        constexpr Text(const char* data, uint32_t size) noexcept: data_(data), size_(size) {}

        const char* data_;
        uint32_t size_;
    };

    // (members ordered by size: 64 bytes, copied as they are)
    struct Token {
        TokenType type = TokenType::EndOfFile;
        bool isInteger = false;            // Number/Percentage/Dimension
        bool hashIsId = false;             // Hash: "id" type (a valid identifier follows '#')
        uint32_t offset = 0, length = 0;   // source bytes (with the base offset of tokenize())
        uint32_t rangeStart = 0, rangeEnd = 0;   // UnicodeRange
        double number = 0;                 // Number/Percentage/Dimension (Percentage: 50 for "50%")
        Text value;                        // unescaped ident/string/url/function/at-keyword/hash name; Delim char (UTF-8)
        Text unit;                         // Dimension, unescaped, case kept

        bool is(TokenType t) const { return type == t; }
        bool isDelim(char c) const { return type == TokenType::Delim && value.size() == 1 && value[0] == c; }
        // Ident whose value equals `lowercase` ASCII case-insensitively
        bool isIdent(const char* lowercase) const;
    };

    struct ComponentValue;

    // A list of component values in the current Arena: the std::vector operations the
    // compatibility layer uses, a copy copies the values (and their children), nothing is
    // freed (16 bytes, no destructor). Growing leaves the old storage to the arena, so lists
    // are reserved to their size where it is known.
    class ValueList {
    public:
        using value_type = ComponentValue;
        using iterator = ComponentValue*;
        using const_iterator = const ComponentValue*;

        constexpr ValueList() noexcept = default;
        inline ValueList(const ValueList& other);
        ValueList(ValueList&& other) noexcept: data_(other.data_), size_(other.size_), capacity_(other.capacity_) {
            other.data_ = nullptr;
            other.size_ = other.capacity_ = 0;
        }
        inline ValueList(std::initializer_list<ComponentValue> values);
        inline ValueList(const ComponentValue* first, const ComponentValue* last);
        inline ValueList& operator=(const ValueList& other);
        ValueList& operator=(ValueList&& other) noexcept {
            if (this != &other){
                data_ = other.data_;
                size_ = other.size_;
                capacity_ = other.capacity_;
                other.data_ = nullptr;
                other.size_ = other.capacity_ = 0;
            }
            return *this;
        }

        size_t size() const noexcept { return size_; }
        size_t capacity() const noexcept { return capacity_; }
        bool empty() const noexcept { return size_ == 0; }
        ComponentValue* data() noexcept { return data_; }
        const ComponentValue* data() const noexcept { return data_; }
        inline ComponentValue* begin() noexcept;
        inline const ComponentValue* begin() const noexcept;
        inline ComponentValue* end() noexcept;
        inline const ComponentValue* end() const noexcept;
        inline ComponentValue& operator[](size_t index) noexcept;
        inline const ComponentValue& operator[](size_t index) const noexcept;
        inline ComponentValue& front() noexcept;
        inline const ComponentValue& front() const noexcept;
        inline ComponentValue& back() noexcept;
        inline const ComponentValue& back() const noexcept;

        void clear() noexcept { size_ = 0; }
        inline void reserve(size_t count);
        inline void resize(size_t count);
        inline void push_back(const ComponentValue& value);
        inline void push_back(ComponentValue&& value);
        inline ComponentValue& emplace_back();
        inline ComponentValue& emplace_back(const ComponentValue& value);
        inline ComponentValue& emplace_back(ComponentValue&& value);
        inline void pop_back() noexcept;
        inline ComponentValue* insert(const ComponentValue* position, const ComponentValue& value);
        inline ComponentValue* insert(const ComponentValue* position, const ComponentValue* first, const ComponentValue* last);
        inline ComponentValue* erase(const ComponentValue* position);
        inline ComponentValue* erase(const ComponentValue* first, const ComponentValue* last);
        inline void assign(const ComponentValue* first, const ComponentValue* last);
        void swap(ValueList& other) noexcept {
            std::swap(data_, other.data_);
            std::swap(size_, other.size_);
            std::swap(capacity_, other.capacity_);
        }

    private:
        inline void grow(size_t count);
        // the values of [first, last) written over uninitialized storage
        inline static void copyInto(ComponentValue* to, const ComponentValue* first, const ComponentValue* last);

        ComponentValue* data_ = nullptr;
        uint32_t size_ = 0;
        uint32_t capacity_ = 0;
    };

    // A token, a simple block ({}, [], ()) or a function (phase-2 §3.9.1). 96 bytes; moved as
    // its bytes.
    struct ComponentValue {
        enum class Kind : uint8_t { Token, Block, Function };
        Token token;                        // Token; for a block its opening token, for a function the Function token
        ValueList children;
        uint32_t offset = 0, length = 0;    // whole extent in the source (blocks: through the closing token, if any)
        Kind kind = Kind::Token;
        bool closed = true;                 // Block/Function: false when the input ended before the closing token

        bool isToken(TokenType t) const { return kind == Kind::Token && token.type == t; }
        bool isBlock(TokenType opening) const { return kind == Kind::Block && token.type == opening; }
        bool isFunction(const char* lowercaseName) const;
        bool isWhitespace() const { return isToken(TokenType::Whitespace); }
        // Function name / block opening without the '('
        const Text& name() const { return token.value; }
    };

    inline void ValueList::copyInto(ComponentValue* to, const ComponentValue* first, const ComponentValue* last){
        const size_t count = (size_t)(last - first);
        if (count == 0)
            return;
        std::memcpy((void*)to, (const void*)first, count * sizeof(ComponentValue));
        // (the children were copied as pointers: each list gets its own)
        for (size_t i = 0; i < count; i++){
            ValueList& children = to[i].children;
            if (children.size_ > 0){
                const ComponentValue* source = children.data_;
                const uint32_t size = children.size_;
                children.data_ = Arena::current().allocateArray<ComponentValue>(size);
                children.capacity_ = size;
                copyInto(children.data_, source, source + size);
            }else{
                children.data_ = nullptr;
                children.capacity_ = 0;
            }
        }
    }
    inline ValueList::ValueList(const ValueList& other){
        if (other.size_ > 0){
            data_ = Arena::current().allocateArray<ComponentValue>(other.size_);
            size_ = capacity_ = other.size_;
            copyInto(data_, other.data_, other.data_ + other.size_);
        }
    }
    inline ValueList::ValueList(std::initializer_list<ComponentValue> values): ValueList(values.begin(), values.end()) {}
    inline ValueList::ValueList(const ComponentValue* first, const ComponentValue* last){
        if (last > first){
            size_ = capacity_ = (uint32_t)(last - first);
            data_ = Arena::current().allocateArray<ComponentValue>(size_);
            copyInto(data_, first, last);
        }
    }
    inline ValueList& ValueList::operator=(const ValueList& other){
        if (this != &other)
            assign(other.data_, other.data_ + other.size_);
        return *this;
    }
    inline ComponentValue* ValueList::begin() noexcept { return data_; }
    inline const ComponentValue* ValueList::begin() const noexcept { return data_; }
    inline ComponentValue* ValueList::end() noexcept { return data_ + size_; }
    inline const ComponentValue* ValueList::end() const noexcept { return data_ + size_; }
    inline ComponentValue& ValueList::operator[](size_t index) noexcept { return data_[index]; }
    inline const ComponentValue& ValueList::operator[](size_t index) const noexcept { return data_[index]; }
    inline ComponentValue& ValueList::front() noexcept { return data_[0]; }
    inline const ComponentValue& ValueList::front() const noexcept { return data_[0]; }
    inline ComponentValue& ValueList::back() noexcept { return data_[size_ - 1]; }
    inline const ComponentValue& ValueList::back() const noexcept { return data_[size_ - 1]; }
    inline void ValueList::grow(size_t count){
        // (count > capacity_) the values move as their bytes
        ComponentValue* storage = Arena::current().allocateArray<ComponentValue>(count);
        if (size_ > 0)
            std::memcpy((void*)storage, (const void*)data_, size_ * sizeof(ComponentValue));
        data_ = storage;
        capacity_ = (uint32_t)count;
    }
    inline void ValueList::reserve(size_t count){
        if (count > capacity_)
            grow(count);
    }
    inline void ValueList::resize(size_t count){
        if (count > capacity_)
            grow(count);
        for (size_t i = size_; i < count; i++)
            new ((void*)(data_ + i)) ComponentValue();
        size_ = (uint32_t)count;
    }
    inline ComponentValue& ValueList::emplace_back(){
        if (size_ == capacity_)
            grow(capacity_ ? (size_t)capacity_ * 2 : 4);
        return *new ((void*)(data_ + size_++)) ComponentValue();
    }
    inline void ValueList::push_back(const ComponentValue& value){
        // (the value may be one of the list's own)
        ComponentValue copy;
        copyInto(&copy, &value, &value + 1);
        if (size_ == capacity_)
            grow(capacity_ ? (size_t)capacity_ * 2 : 4);
        std::memcpy((void*)(data_ + size_++), (const void*)&copy, sizeof(ComponentValue));
    }
    inline void ValueList::push_back(ComponentValue&& value){
        ComponentValue moved;
        std::memcpy((void*)&moved, (const void*)&value, sizeof(ComponentValue));
        value.children.data_ = nullptr;
        value.children.size_ = value.children.capacity_ = 0;
        if (size_ == capacity_)
            grow(capacity_ ? (size_t)capacity_ * 2 : 4);
        std::memcpy((void*)(data_ + size_++), (const void*)&moved, sizeof(ComponentValue));
    }
    inline ComponentValue& ValueList::emplace_back(const ComponentValue& value){
        push_back(value);
        return back();
    }
    inline ComponentValue& ValueList::emplace_back(ComponentValue&& value){
        push_back(std::move(value));
        return back();
    }
    inline void ValueList::pop_back() noexcept { size_--; }
    inline ComponentValue* ValueList::insert(const ComponentValue* position, const ComponentValue& value){
        return insert(position, &value, &value + 1);
    }
    inline ComponentValue* ValueList::insert(const ComponentValue* position, const ComponentValue* first, const ComponentValue* last){
        const size_t at = (size_t)(position - data_);
        const size_t count = (size_t)(last - first);
        if (count == 0)
            return data_ + at;
        // (the values may be the list's own: copied before anything moves)
        ComponentValue* copies = Arena::current().allocateArray<ComponentValue>(count);
        copyInto(copies, first, last);
        if (size_ + count > capacity_)
            grow(std::max((size_t)capacity_ * 2, (size_t)size_ + count));
        std::memmove((void*)(data_ + at + count), (const void*)(data_ + at), (size_ - at) * sizeof(ComponentValue));
        std::memcpy((void*)(data_ + at), (const void*)copies, count * sizeof(ComponentValue));
        size_ += (uint32_t)count;
        return data_ + at;
    }
    inline ComponentValue* ValueList::erase(const ComponentValue* position){
        return erase(position, position + 1);
    }
    inline ComponentValue* ValueList::erase(const ComponentValue* first, const ComponentValue* last){
        const size_t at = (size_t)(first - data_);
        const size_t count = (size_t)(last - first);
        if (count > 0){
            std::memmove((void*)(data_ + at), (const void*)(data_ + at + count), (size_ - at - count) * sizeof(ComponentValue));
            size_ -= (uint32_t)count;
        }
        return data_ + at;
    }
    inline void ValueList::assign(const ComponentValue* first, const ComponentValue* last){
        const size_t count = (size_t)(last - first);
        // (a range of the list itself is copied before the list changes)
        ComponentValue* storage = count > 0 ? Arena::current().allocateArray<ComponentValue>(count) : nullptr;
        copyInto(storage, first, last);
        data_ = storage;
        size_ = capacity_ = (uint32_t)count;
    }

    struct Declaration {
        std::string name;                   // lowercase unless custom property (--x keeps case)
        ValueList value;  // trimmed, without !important
        bool important = false;
        SourceRef ref, nameRef, valueRef;   // whole declaration (through the value / !important), name, value

        bool isCustomProperty() const { return name.size() >= 2 && name[0] == '-' && name[1] == '-'; }
    };

    struct Rule {
        enum class Kind : uint8_t { Style, At };
        Kind kind = Kind::Style;
        std::string atName;                       // lowercase, without '@'
        ValueList prelude;      // selector list or at-rule prelude (raw, whitespace kept)
        SourceRef preludeRef, ref;                // the prelude; the whole rule
        bool hasBlock = false;
        std::vector<Declaration> declarations;    // style rules, @font-face, @page, keyframe blocks, ...
        std::vector<Rule> rules;                  // nested style rules, @media/@supports/@layer bodies, keyframe selectors
        // The block's raw contents, as "consume a simple block" sees them; declarations and
        // rules above are the same tokens parsed as a block's contents (CSS Syntax 2024):
        // every block holds declarations and rules mixed, in source order by their refs.
        // Filled by the CSS Syntax entry points (parseRuleList, parseRule, ...) only:
        // parseStylesheet and parseDeclarationList leave it empty (one parse per block).
        ValueList block;
        SourceRef blockRef;                       // the block, braces included

        bool isAt(const char* lowercaseName) const { return kind == Kind::At && atName == lowercaseName; }
    };

    struct Stylesheet {
        std::vector<Rule> rules;
        uint32_t file = 0;
    };

}

#endif //COMPAT_CSS_CSSAST_H
