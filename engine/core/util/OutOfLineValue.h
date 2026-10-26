// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef OUTOFLINEVALUE_H
#define OUTOFLINEVALUE_H

#include <memory>

namespace jasperengine{

    // A T kept out of line, so the owners that leave it at its defaults (most of them: a
    // Material is held 16 times inline in every MeshComponent of a game build) pay a pointer
    // instead of the whole value. A value all the same: copies are deep, comparisons by value,
    // and an unset value reads and compares as T(). Read through get(), * and -> (they never
    // allocate); write through edit(), which allocates it. A copy of a value equal to T()
    // stays unset. T needs a default constructor, a copy constructor and operator==.
    template<typename T>
    class OutOfLineValue{
    private:
        std::unique_ptr<T> value;

        static const T& defaults(){
            static const T none;
            return none;
        }
        static std::unique_ptr<T> copyOf(const T* other){
            return (other && !(*other == defaults())) ? std::make_unique<T>(*other) : nullptr;
        }

    public:
        OutOfLineValue() = default;
        OutOfLineValue(const T& other): value(copyOf(&other)) {}
        OutOfLineValue(const OutOfLineValue& other): value(copyOf(other.value.get())) {}
        OutOfLineValue(OutOfLineValue&& other) noexcept = default;
        OutOfLineValue& operator = (OutOfLineValue&& other) noexcept = default;
        OutOfLineValue& operator = (const OutOfLineValue& other){
            if (this != &other)
                value = copyOf(other.value.get());
            return *this;
        }
        // keeps its storage when it has one (a Lua reference to it stays valid)
        OutOfLineValue& operator = (const T& other){
            if (value)
                *value = other;
            else
                value = copyOf(&other);
            return *this;
        }

        const T& get() const { return value ? *value : defaults(); }
        const T& operator * () const { return get(); }
        const T* operator -> () const { return &get(); }
        operator const T& () const { return get(); }

        // the value to write, made (as T()) when unset
        T& edit(){
            if (!value)
                value = std::make_unique<T>();
            return *value;
        }
        // the value to write if there is one, else null (nothing is made)
        T* editIfSet() { return value.get(); }
        // whether it is allocated (it may still hold T())
        bool isSet() const { return value != nullptr; }
        void reset() { value.reset(); }

        bool operator == (const OutOfLineValue& other) const { return get() == other.get(); }
        bool operator != (const OutOfLineValue& other) const { return !(get() == other.get()); }
        bool operator == (const T& other) const { return get() == other; }
        bool operator != (const T& other) const { return !(get() == other); }
    };

}

#endif //OUTOFLINEVALUE_H
