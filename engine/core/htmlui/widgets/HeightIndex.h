// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef HTMLUI_WIDGETS_HEIGHTINDEX_H
#define HTMLUI_WIDGETS_HEIGHTINDEX_H

// Row offsets of a virtual view (S17 R3): a Fenwick tree over row heights, so changing one
// height, the offset of a row and the row at an offset are O(log n) (OpenEngine's ListView
// rebuilt an O(n) prefix array on every change). Rows that were never measured take the
// estimate: the tree holds each measured row's difference to it, so changing the estimate
// moves every unmeasured row in O(1), and a view of uniform rows (nothing measured) answers
// in O(1). 12 bytes per row (S17 R34).

#include "Export.h"

#include <cstddef>
#include <vector>

namespace jasperengine::htmlui {

    class JASPERENGINE_API HeightIndex {
    public:
        // count rows, none measured
        void reset(int count, float estimate);
        // the height of the rows not measured (a uniform view's item-height)
        void setEstimate(float estimate);
        // a row's measured height (>= 0)
        void set(int index, float height);
        // the row takes the estimate again
        void clear(int index);
        // n unmeasured rows after the last one
        void append(int n);
        // keeps the first `count` rows
        void truncate(int count);
        // grows (unmeasured rows) or truncates to `count`
        void resize(int count);

        int count() const { return (int)heights.size(); }
        float estimate() const { return estimateValue; }
        int measuredCount() const { return measured; }
        bool isMeasured(int index) const;
        float heightOf(int index) const;
        // the top of row `index`; index == count() gives total()
        float offsetOf(int index) const;
        // the row whose span [offset, offset + height) holds y, clamped to the rows (0 above the
        // first, the last below the end); -1 without rows
        int indexAt(float y) const;
        float total() const;
        // what the index keeps per row (R34)
        size_t memoryBytes() const;

    private:
        // the sum of the differences of rows [0, n)
        double prefixDelta(int n) const;
        void addDelta(int index, double delta);
        // the tree again from the heights (O(n))
        void rebuild();

        std::vector<float> heights;   // measured height, or -1 for the estimate
        std::vector<double> tree;     // Fenwick tree (1-based) of height - estimate of measured rows
        double deltaSum = 0.0;
        float estimateValue = 0.0f;
        int measured = 0;
    };

}

#endif //HTMLUI_WIDGETS_HEIGHTINDEX_H
