// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "LatticeTrackKeys.h"

#include "lattice/LatticeEditorOps.h"
#include "lattice/LatticeSolver.h"
#include "math/Interpolation.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;

editor::LatticeTrackData::LatticeTrackData(const KeyframeTracksComponent& keyframe, const LatticeTracksComponent& tracks)
    : times(keyframe.times), easings(keyframe.easings), points(tracks.points), values(tracks.values),
      inTangents(tracks.inTangents), outTangents(tracks.outTangents){
}

std::vector<int> editor::LatticeTrackKeys::pointsToKey(uint32_t sceneId, Entity latticeEntity, const LatticeComponent& lattice,
                                                       const LatticeTracksComponent* tracks){
    const int count = lattice::pointCount(lattice.resolutionX, lattice.resolutionY, lattice.resolutionZ);
    std::vector<char> chosen(count, 0);
    bool any = false;
    auto choose = [&](int point){
        if (point >= 0 && point < count && !chosen[point]){
            chosen[point] = 1;
            any = true;
        }
    };

    for (int point : LatticeEditorOps::getSelectedPoints(sceneId, latticeEntity)){
        choose(point);
    }

    if (!any){
        for (int point = 0; point < count; point++){
            if (currentOffset(lattice, point) != Vector3(0, 0, 0)){
                choose(point);
            }
        }
        if (tracks){
            for (int point : tracks->points){
                choose(point);
            }
        }
    }

    std::vector<int> points;
    for (int point = 0; point < count; point++){
        if (chosen[point] || !any){
            points.push_back(point);
        }
    }
    return points;
}

Vector3 editor::LatticeTrackKeys::currentOffset(const LatticeComponent& lattice, int point){
    if (point < 0 || (size_t)point >= lattice.offsets.size()){
        return Vector3(0, 0, 0);
    }
    return lattice.offsets[point];
}

Vector3 editor::LatticeTrackKeys::sample(const LatticeTrackData& data, size_t column, float time, const Vector3& fallback){
    if (data.times.empty() || data.values.empty()){
        return fallback;
    }

    // the key and segment factor ActionSystem::keyframeUpdate computes
    size_t index = 0;
    while (index < data.times.size() - 1 && data.times[index] < time){
        index++;
    }
    if (index >= data.values.size()){
        return fallback; // the runtime freezes such a track
    }

    float previousTime = index > 0 ? data.times[index - 1] : 0.0f;
    float nextTime = data.times[index];
    float t = 0.0f;
    if (nextTime > previousTime){
        t = std::clamp((time - previousTime) / (nextTime - previousTime), 0.0f, 1.0f);
    }
    if (index > 0){
        size_t segment = index - 1;
        if (segment < data.easings.size() && data.easings[segment] != EaseType::LINEAR){
            t = Ease::getFunction(data.easings[segment])(t);
        }
    }

    const std::vector<Vector3>& previous = data.values[index > 0 ? index - 1 : 0];
    const std::vector<Vector3>& next = data.values[index];
    if (column >= previous.size() || column >= next.size()){
        return fallback;
    }

    bool cubic = index > 0 && data.inTangents.size() == data.values.size() && data.outTangents.size() == data.values.size() &&
                 data.outTangents[index - 1].size() == previous.size() && data.inTangents[index].size() == next.size();
    if (cubic){
        return Interpolation::cubicSpline(t, nextTime - previousTime, previous[column], data.outTangents[index - 1][column],
                                          next[column], data.inTangents[index][column]);
    }
    return previous[column] + t * (next[column] - previous[column]);
}

void editor::LatticeTrackKeys::setKey(LatticeTrackData& data, float time, const std::vector<int>& points,
                                      const std::vector<Vector3>& values, const LatticeComponent& lattice, float epsilon){
    const Vector3 zero(0, 0, 0);
    const size_t keyCount = data.times.size();

    // one row per key and one value per point (rows of hand-edited data may be missing or short)
    auto completeRow = [&](std::vector<Vector3>& row){
        for (size_t column = row.size(); column < data.points.size(); column++){
            row.push_back(currentOffset(lattice, data.points[column]));
        }
        row.resize(data.points.size());
    };
    data.values.resize(keyCount);
    for (std::vector<Vector3>& row : data.values){
        completeRow(row);
    }
    // Hermite tangents stay mirrored with values (both or none, the runtime needs both)
    const bool hasTangents = !data.inTangents.empty() || !data.outTangents.empty();
    auto mirrorTangents = [&](std::vector<std::vector<Vector3>>& tangents){
        tangents.resize(keyCount);
        for (std::vector<Vector3>& row : tangents){
            row.resize(data.points.size(), zero);
        }
    };
    if (hasTangents){
        mirrorTangents(data.inTangents);
        mirrorTangents(data.outTangents);
    }

    // points new to the track: a column each, the existing keys hold the current offset
    for (int point : points){
        if (std::find(data.points.begin(), data.points.end(), point) != data.points.end()){
            continue;
        }
        data.points.push_back(point);
        for (std::vector<Vector3>& row : data.values){
            row.push_back(currentOffset(lattice, point));
        }
        if (hasTangents){
            for (std::vector<Vector3>& row : data.inTangents) row.push_back(zero);
            for (std::vector<Vector3>& row : data.outTangents) row.push_back(zero);
        }
    }

    // writes the recorded values into a row (every column of a point listed twice)
    auto record = [&](std::vector<Vector3>& row){
        for (size_t n = 0; n < points.size() && n < values.size(); n++){
            for (size_t column = 0; column < data.points.size(); column++){
                if (data.points[column] == points[n]){
                    row[column] = values[n];
                }
            }
        }
    };

    for (size_t i = 0; i < keyCount; i++){
        if (std::fabs(data.times[i] - time) <= epsilon){
            record(data.values[i]);
            return;
        }
    }

    // a new key: the columns it does not record keep what the track plays at that time
    std::vector<Vector3> row(data.points.size());
    for (size_t column = 0; column < data.points.size(); column++){
        row[column] = sample(data, column, time, currentOffset(lattice, data.points[column]));
    }
    record(row);

    size_t j = 0;
    while (j < keyCount && data.times[j] < time) j++;

    data.times.insert(data.times.begin() + (long int)j, time);
    data.values.insert(data.values.begin() + (long int)j, row);
    if (hasTangents){
        std::vector<Vector3> zeroRow(data.points.size(), zero);
        data.inTangents.insert(data.inTangents.begin() + (long int)j, zeroRow);
        data.outTangents.insert(data.outTangents.begin() + (long int)j, zeroRow);
    }

    // per-segment easings stay aligned with the keys
    if (!data.easings.empty()){
        if (j == 0){
            // a new leading segment
            data.easings.insert(data.easings.begin(), EaseType::LINEAR);
        }else if (j < keyCount && j - 1 < data.easings.size()){
            // a middle insert splits segment j-1: both halves keep its ease
            data.easings.insert(data.easings.begin() + (long int)(j - 1), data.easings[j - 1]);
        }
        // appending at the end needs no change (missing entries mean linear)
    }
}
