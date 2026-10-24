// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef SIGNATURE_H
#define SIGNATURE_H

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include "BitOps.h"

namespace jasperengine {

    // ComponentId (unsigned char) addresses every bit, so a signature can never be indexed
    // out of range. A wider id goes together with a wider mask.
    inline constexpr size_t kMaxComponentTypes = 256;

    // The component types of an entity: a fixed 256-bit mask, 32 bytes, no allocation. Keeps
    // the std::bitset subset the engine used when it was a std::bitset<64>, including its
    // string form (MSB first).
    class ComponentMask {
    private:
        static constexpr size_t kWords = kMaxComponentTypes / 64;
        uint64_t words[kWords] = {};

    public:
        ComponentMask() = default;

        // '0'/'1' characters, the last one is bit 0 (std::bitset's form). Shorter strings set
        // the low bits only, so the 64-character strings of older scripts still parse; of a
        // longer one only the first 256 characters count, as std::bitset does. Any other
        // character throws std::invalid_argument, like std::bitset.
        explicit ComponentMask(const std::string& bits){
            if (!parse(bits.c_str(), bits.size(), *this)){
                throw std::invalid_argument("ComponentMask: string has a character other than '0' or '1'");
            }
        }

        explicit ComponentMask(const char* bits): ComponentMask(std::string(bits ? bits : "")) {}

        // The string form without exceptions: false (and `out` unchanged) on a bad character
        static bool fromString(const std::string& bits, ComponentMask& out){
            ComponentMask parsed;
            if (!parse(bits.c_str(), bits.size(), parsed)){
                return false;
            }
            out = parsed;
            return true;
        }

        static constexpr size_t size() { return kMaxComponentTypes; }

        bool test(size_t pos) const {
            return pos < kMaxComponentTypes && ((words[pos >> 6] >> (pos & 63)) & 1u);
        }

        ComponentMask& set(size_t pos, bool value = true){
            if (pos < kMaxComponentTypes){
                const uint64_t bit = uint64_t(1) << (pos & 63);
                words[pos >> 6] = value ? (words[pos >> 6] | bit) : (words[pos >> 6] & ~bit);
            }
            return *this;
        }

        ComponentMask& set(){
            for (uint64_t& word : words) word = ~uint64_t(0);
            return *this;
        }

        ComponentMask& reset(size_t pos){
            return set(pos, false);
        }

        ComponentMask& reset(){
            for (uint64_t& word : words) word = 0;
            return *this;
        }

        bool any() const {
            for (uint64_t word : words){
                if (word) return true;
            }
            return false;
        }

        bool none() const { return !any(); }

        JE_ECS_INLINE size_t count() const {
            return ecs::popCountWords(words, kWords);
        }

        bool intersects(const ComponentMask& other) const {
            for (size_t i = 0; i < kWords; i++){
                if (words[i] & other.words[i]) return true;
            }
            return false;
        }

        // every bit of `other` is set here too
        bool contains(const ComponentMask& other) const {
            for (size_t i = 0; i < kWords; i++){
                if ((words[i] & other.words[i]) != other.words[i]) return false;
            }
            return true;
        }

        // fn(size_t bit) for each set bit, ascending
        template<typename F>
        void forEachSet(F&& fn) const {
            for (size_t i = 0; i < kWords; i++){
                uint64_t word = words[i];
                while (word){
                    const unsigned bit = ecs::countTrailingZeros(word);
                    fn(i * 64 + bit);
                    word &= word - 1;
                }
            }
        }

        std::string to_string() const {
            std::string text(kMaxComponentTypes, '0');
            forEachSet([&text](size_t bit){ text[kMaxComponentTypes - 1 - bit] = '1'; });
            return text;
        }

        ComponentMask& operator|=(const ComponentMask& other){
            for (size_t i = 0; i < kWords; i++) words[i] |= other.words[i];
            return *this;
        }

        ComponentMask& operator&=(const ComponentMask& other){
            for (size_t i = 0; i < kWords; i++) words[i] &= other.words[i];
            return *this;
        }

        ComponentMask operator~() const {
            ComponentMask result;
            for (size_t i = 0; i < kWords; i++) result.words[i] = ~words[i];
            return result;
        }

        friend ComponentMask operator|(ComponentMask a, const ComponentMask& b){ return a |= b; }
        friend ComponentMask operator&(ComponentMask a, const ComponentMask& b){ return a &= b; }

        friend bool operator==(const ComponentMask& a, const ComponentMask& b){
            for (size_t i = 0; i < kWords; i++){
                if (a.words[i] != b.words[i]) return false;
            }
            return true;
        }

        friend bool operator!=(const ComponentMask& a, const ComponentMask& b){ return !(a == b); }

    private:
        static bool parse(const char* text, size_t length, ComponentMask& out){
            if (length > kMaxComponentTypes){
                length = kMaxComponentTypes;
            }
            ComponentMask parsed;
            for (size_t i = 0; i < length; i++){
                const char c = text[i];
                if (c == '1'){
                    parsed.set(length - 1 - i);
                }else if (c != '0'){
                    return false;
                }
            }
            out = parsed;
            return true;
        }
    };

    using Signature = ComponentMask;

}

#endif //SIGNATURE_H
