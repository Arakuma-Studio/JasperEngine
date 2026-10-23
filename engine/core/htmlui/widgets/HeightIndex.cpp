// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "htmlui/widgets/HeightIndex.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine::htmlui;

namespace {

    // a row that takes the estimate (heights are never negative)
    constexpr float kUnmeasured = -1.0f;

    inline int lowBit(int value){
        return value & -value;
    }

}

void HeightIndex::reset(int count, float estimate){
    count = std::max(0, count);
    heights.assign((size_t)count, kUnmeasured);
    tree.assign((size_t)count + 1, 0.0);
    deltaSum = 0.0;
    measured = 0;
    estimateValue = std::max(0.0f, estimate);
}

void HeightIndex::setEstimate(float estimate){
    estimate = std::max(0.0f, estimate);
    if (estimate == estimateValue)
        return;
    estimateValue = estimate;
    // the measured rows keep their heights: their differences to the estimate change (O(n),
    // once a view knows its first measured row)
    if (measured > 0)
        rebuild();
}

void HeightIndex::rebuild(){
    const int size = count();
    tree.assign((size_t)size + 1, 0.0);
    deltaSum = 0.0;
    for (int i = 0; i < size; i++){
        if (heights[i] >= 0.0f){
            tree[i + 1] = (double)heights[i] - (double)estimateValue;
            deltaSum += tree[i + 1];
        }
    }
    // linear construction: every node adds its sum to its parent
    for (int node = 1; node <= size; node++){
        const int parent = node + lowBit(node);
        if (parent <= size)
            tree[parent] += tree[node];
    }
}

bool HeightIndex::isMeasured(int index) const{
    return index >= 0 && index < count() && heights[index] >= 0.0f;
}

float HeightIndex::heightOf(int index) const{
    if (index < 0 || index >= count())
        return 0.0f;
    return heights[index] < 0.0f ? estimateValue : heights[index];
}

void HeightIndex::addDelta(int index, double delta){
    deltaSum += delta;
    const int size = count();
    for (int node = index + 1; node <= size; node += lowBit(node))
        tree[node] += delta;
}

double HeightIndex::prefixDelta(int n) const{
    double sum = 0.0;
    for (int node = std::min(n, count()); node > 0; node -= lowBit(node))
        sum += tree[node];
    return sum;
}

void HeightIndex::set(int index, float height){
    if (index < 0 || index >= count())
        return;
    height = std::max(0.0f, height);
    const float old = heightOf(index);
    if (heights[index] < 0.0f)
        measured++;
    else if (heights[index] == height)
        return;
    heights[index] = height;
    if (height != old)
        addDelta(index, (double)height - (double)old);
}

void HeightIndex::clear(int index){
    if (!isMeasured(index))
        return;
    const double delta = (double)estimateValue - (double)heights[index];
    heights[index] = kUnmeasured;
    measured--;
    if (delta != 0.0)
        addDelta(index, delta);
}

void HeightIndex::append(int n){
    if (n <= 0)
        return;
    heights.reserve(heights.size() + (size_t)n);
    tree.reserve(tree.size() + (size_t)n);
    for (int i = 0; i < n; i++){
        heights.push_back(kUnmeasured);
        // a new node k holds the sum of (k - lowbit(k), k]: its children's sums (its own value is 0)
        const int node = (int)heights.size();
        double sum = 0.0;
        for (int child = node - 1; child > node - lowBit(node); child -= lowBit(child))
            sum += tree[child];
        tree.push_back(sum);
    }
}

void HeightIndex::truncate(int keep){
    keep = std::clamp(keep, 0, count());
    if (keep == count())
        return;
    for (int i = keep; i < count(); i++){
        if (heights[i] >= 0.0f){
            deltaSum -= (double)heights[i] - (double)estimateValue;
            measured--;
        }
    }
    heights.resize((size_t)keep);
    // the nodes of the rows kept cover only kept rows
    tree.resize((size_t)keep + 1);
    if (measured == 0)
        deltaSum = 0.0;
}

void HeightIndex::resize(int size){
    size = std::max(0, size);
    if (size > count())
        append(size - count());
    else
        truncate(size);
}

float HeightIndex::offsetOf(int index) const{
    index = std::clamp(index, 0, count());
    if (measured == 0)
        return (float)((double)index * (double)estimateValue);
    return (float)((double)index * (double)estimateValue + prefixDelta(index));
}

float HeightIndex::total() const{
    return (float)((double)count() * (double)estimateValue + (measured ? deltaSum : 0.0));
}

int HeightIndex::indexAt(float y) const{
    const int size = count();
    if (size == 0)
        return -1;
    if (y <= 0.0f)
        return 0;
    if (measured == 0){
        if (estimateValue <= 0.0f)
            return size - 1;
        return std::clamp((int)std::floor((double)y / (double)estimateValue), 0, size - 1);
    }
    // descent: the largest number of rows whose heights sum to at most y; a node of `step` rows
    // sums to step * estimate + its difference
    int position = 0;
    double remaining = (double)y;
    int step = 1;
    while (step * 2 <= size)
        step *= 2;
    for (; step > 0; step /= 2){
        const int next = position + step;
        if (next > size)
            continue;
        const double span = (double)step * (double)estimateValue + tree[next];
        if (span <= remaining){
            position = next;
            remaining -= span;
        }
    }
    return std::min(position, size - 1);
}

size_t HeightIndex::memoryBytes() const{
    return heights.capacity() * sizeof(float) + tree.capacity() * sizeof(double);
}
