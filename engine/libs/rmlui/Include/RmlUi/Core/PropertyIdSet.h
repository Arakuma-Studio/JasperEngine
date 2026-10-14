#pragma once

#include "Header.h"
#include "ID.h"
#include "Types.h"
#include <stdint.h>
#if defined(_MSC_VER) && !defined(__clang__)
	#include <intrin.h>
#endif

// The small members of PropertyIdSet are inlined into every caller: they are on the style hot paths.
#if defined(_MSC_VER)
	#define RMLUI_IDSET_INLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__)
	#define RMLUI_IDSET_INLINE inline __attribute__((always_inline))
#else
	#define RMLUI_IDSET_INLINE inline
#endif

namespace Rml {

class PropertyIdSetIterator;

/*
    PropertyIdSet is a 'set'-like container for PropertyIds.

    Implemented as a fixed bit set of 64-bit words. It is cheap to construct and use, requiring no dynamic allocation.

    Supports union and intersection operations between two sets, as well as iteration through the IDs that are inserted.
    Iteration skips empty words, so its cost follows the number of inserted IDs rather than the number of possible IDs.

    Fork (phase 5, registered custom properties, FORK.md P5-4): the ids below InlineIds, every id RmlUi and the engine
    define, are bits of the inline words, as before; the ids above them, up to PropertyId::MaxNumIds (the registered
    custom properties of the documents), are bits of a heap array the set allocates when one is inserted. Bit 0 (the id
    Invalid, never inserted) of the inline words says whether the heap array holds any id, so that Empty() and the
    inline ids cost what they did with 256 ids in all.
*/

class RMLUICORE_API PropertyIdSet {
private:
	static constexpr size_t N = size_t(PropertyId::MaxNumIds);
	static constexpr size_t InlineIds = 256;
	static constexpr size_t NumWords = InlineIds / 64;
	static_assert(size_t(PropertyId::NumDefinedIds) < InlineIds, "The defined property ids must be inline ids.");
	uint64_t words[NumWords] = {};
	// The words of the ids from InlineIds on, num_high of them (null: none inserted since the set was made or emptied by
	// an intersection).
	uint64_t* high = nullptr;
	uint32_t num_high = 0;
	// Bit 0 of words[0]: the high words hold an id.
	static constexpr uint64_t HighFlag = 1;

	static uint64_t Bit(size_t index) { return uint64_t(1) << (index % 64); }

	// The out-of-line parts for high ids (PropertyIdSet.cpp).
	void InsertHigh(size_t index);
	void EraseHigh(size_t index);
	bool HighEmpty() const;
	size_t HighSize() const;
	void CopyHigh(const PropertyIdSet& other);
	void OrHigh(const PropertyIdSet& other);
	void AndHigh(const PropertyIdSet& other);
	void ClearHigh();
	bool HighIntersects(const PropertyIdSet& other) const;
	// The high words of a | b (intersect false) or a & b (true) into this new set.
	void HighOf(const PropertyIdSet& a, const PropertyIdSet& b, bool intersect);
	void FreeHigh();
	size_t NextFromHigh(size_t index) const;

public:
	RMLUI_IDSET_INLINE PropertyIdSet() { static_assert((size_t)PropertyId::Invalid == 0, "PropertyIdSet makes an assumption that PropertyId::Invalid is zero."); }
	RMLUI_IDSET_INLINE ~PropertyIdSet()
	{
		if (high)
			FreeHigh();
	}
	PropertyIdSet(const PropertyIdSet& other)
	{
		for (size_t i = 0; i < NumWords; i++)
			words[i] = other.words[i];
		if (other.high)
			CopyHigh(other);
	}
	RMLUI_IDSET_INLINE PropertyIdSet(PropertyIdSet&& other) noexcept : high(other.high), num_high(other.num_high)
	{
		for (size_t i = 0; i < NumWords; i++)
			words[i] = other.words[i];
		other.high = nullptr;
		other.num_high = 0;
		other.words[0] &= ~HighFlag;
	}
	PropertyIdSet& operator=(const PropertyIdSet& other)
	{
		if (this == &other)
			return *this;
		for (size_t i = 0; i < NumWords; i++)
			words[i] = other.words[i];
		if (other.high)
			CopyHigh(other);
		else if (high)
			ClearHigh();
		return *this;
	}
	PropertyIdSet& operator=(PropertyIdSet&& other) noexcept
	{
		if (this == &other)
			return *this;
		for (size_t i = 0; i < NumWords; i++)
			words[i] = other.words[i];
		if (high)
			FreeHigh();
		high = other.high;
		num_high = other.num_high;
		other.high = nullptr;
		other.num_high = 0;
		other.words[0] &= ~HighFlag;
		return *this;
	}

	RMLUI_IDSET_INLINE void Insert(PropertyId id)
	{
		RMLUI_ASSERT(size_t(id) < N);
		if (size_t(id) < InlineIds)
			words[size_t(id) / 64] |= Bit(size_t(id));
		else
			InsertHigh(size_t(id));
	}

	RMLUI_IDSET_INLINE void Clear()
	{
		for (uint64_t& word : words)
			word = 0;
		if (high)
			ClearHigh();
	}
	RMLUI_IDSET_INLINE void Erase(PropertyId id)
	{
		RMLUI_ASSERT(size_t(id) < N);
		if (size_t(id) < InlineIds)
			words[size_t(id) / 64] &= ~Bit(size_t(id));
		else if (words[0] & HighFlag)
			EraseHigh(size_t(id));
	}

	RMLUI_IDSET_INLINE bool Empty() const
	{
		// One test of the OR-ed words rather than a branch per word.
		uint64_t any = 0;
		for (uint64_t word : words)
			any |= word;
		return any == 0;
	}
	RMLUI_IDSET_INLINE bool Contains(PropertyId id) const
	{
		const size_t index = size_t(id);
		if (index < InlineIds)
			return index != 0 && (words[index / 64] & Bit(index)) != 0;
		const size_t word = (index - InlineIds) / 64;
		return (words[0] & HighFlag) && word < num_high && (high[word] & Bit(index)) != 0;
	}

	size_t Size() const
	{
		size_t count = 0;
		for (uint64_t word : words)
			count += PopCount(word);
		return (words[0] & HighFlag) ? count - 1 + HighSize() : count;
	}

	// Union with another set
	RMLUI_IDSET_INLINE PropertyIdSet& operator|=(const PropertyIdSet& other)
	{
		for (size_t i = 0; i < NumWords; i++)
			words[i] |= other.words[i];
		if (other.words[0] & HighFlag)
			OrHigh(other);
		return *this;
	}
	PropertyIdSet operator|(const PropertyIdSet& other) const
	{
		PropertyIdSet result;
		for (size_t i = 0; i < NumWords; i++)
			result.words[i] = words[i] | other.words[i];
		if (result.words[0] & HighFlag)
			result.HighOf(*this, other, false);
		return result;
	}

	// Intersection with another set
	RMLUI_IDSET_INLINE PropertyIdSet& operator&=(const PropertyIdSet& other)
	{
		const bool had_high = (words[0] & HighFlag) != 0;
		for (size_t i = 0; i < NumWords; i++)
			words[i] &= other.words[i];
		if (had_high)
			AndHigh(other);
		return *this;
	}
	RMLUI_IDSET_INLINE PropertyIdSet operator&(const PropertyIdSet& other) const
	{
		PropertyIdSet result;
		for (size_t i = 0; i < NumWords; i++)
			result.words[i] = words[i] & other.words[i];
		if (result.words[0] & HighFlag)
			result.HighOf(*this, other, true);
		return result;
	}

	// True if the two sets have an id in common: !(a & b).Empty() without the intersection (fork, phase 5).
	RMLUI_IDSET_INLINE bool Intersects(const PropertyIdSet& other) const
	{
		uint64_t any = 0;
		for (size_t i = 0; i < NumWords; i++)
			any |= words[i] & other.words[i];
		if (any & ~HighFlag)
			return true;
		return (any & HighFlag) && HighIntersects(other);
	}

	// Iterator support. Iterates through all the PropertyIds that are set (contained).
	// @note: Only const_iterators are provided.
	inline PropertyIdSetIterator begin() const;
	inline PropertyIdSetIterator end() const;

	// Erases the property id represented by a valid iterator. Iterator must be in the range [begin, end).
	// @return A new iterator pointing to the next element or end().
	inline PropertyIdSetIterator Erase(PropertyIdSetIterator it);

private:
	// The index of the first contained id at or after 'index', or N if there is none.
	size_t NextFrom(size_t index) const
	{
		if (index < InlineIds)
		{
			size_t word_index = index / 64;
			uint64_t word = words[word_index] & (~uint64_t(0) << (index % 64));
			if (word_index == 0)
				word &= ~HighFlag;
			while (true)
			{
				if (word)
					return word_index * 64 + CountTrailingZeros(word);
				if (++word_index >= NumWords)
					break;
				word = words[word_index];
			}
			index = InlineIds;
		}
		return (words[0] & HighFlag) ? NextFromHigh(index) : N;
	}

	static size_t CountTrailingZeros(uint64_t word)
	{
		RMLUI_ASSERT(word != 0);
#if defined(__GNUC__) || defined(__clang__)
		return (size_t)__builtin_ctzll(word);
#elif defined(_MSC_VER) && (defined(_M_X64) || defined(_M_ARM64))
		unsigned long index = 0;
		_BitScanForward64(&index, word);
		return (size_t)index;
#else
		size_t count = 0;
		while ((word & 1) == 0)
		{
			word >>= 1;
			count++;
		}
		return count;
#endif
	}

	static size_t PopCount(uint64_t word)
	{
#if defined(__GNUC__) || defined(__clang__)
		return (size_t)__builtin_popcountll(word);
#else
		size_t count = 0;
		for (; word; word &= word - 1)
			count++;
		return count;
#endif
	}

	friend class PropertyIdSetIterator;
};

class RMLUICORE_API PropertyIdSetIterator {
public:
	PropertyIdSetIterator() = default;
	PropertyIdSetIterator(const PropertyIdSet* container, size_t id_index) : container(container), id_index(id_index) { ProceedToNextValid(); }

	PropertyIdSetIterator& operator++()
	{
		++id_index;
		ProceedToNextValid();
		return *this;
	}

	bool operator==(const PropertyIdSetIterator& other) const { return container == other.container && id_index == other.id_index; }
	bool operator!=(const PropertyIdSetIterator& other) const { return !(*this == other); }
	PropertyId operator*() const { return static_cast<PropertyId>(id_index); }

private:
	inline void ProceedToNextValid()
	{
		if (container && id_index < size_t(PropertyId::MaxNumIds))
			id_index = container->NextFrom(id_index);
	}

	const PropertyIdSet* container = nullptr;
	size_t id_index = 0;

	friend PropertyIdSetIterator PropertyIdSet::Erase(PropertyIdSetIterator);
};

PropertyIdSetIterator PropertyIdSet::begin() const
{
	if (Empty())
		return end();
	return PropertyIdSetIterator(this, 1);
}

PropertyIdSetIterator PropertyIdSet::end() const
{
	return PropertyIdSetIterator(this, N);
}

PropertyIdSetIterator PropertyIdSet::Erase(PropertyIdSetIterator it)
{
	RMLUI_ASSERT(it.container == this && it.id_index < N);
	Erase(PropertyId(it.id_index));
	++it;
	return it;
}

} // namespace Rml
