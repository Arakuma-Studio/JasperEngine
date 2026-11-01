// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "AnimationClip.h"

#include "animation/AnimationClipIO.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;

Quaternion jasperengine::quaternionFromXYZW(const float* xyzw){
    return Quaternion(xyzw[3], xyzw[0], xyzw[1], xyzw[2]);
}

void jasperengine::quaternionToXYZW(const Quaternion& q, float* xyzw){
    xyzw[0] = q.x;
    xyzw[1] = q.y;
    xyzw[2] = q.z;
    xyzw[3] = q.w;
}

// ClipData helpers (complete: pure functions of the value form)

float ClipData::effectiveLength() const{
    if (hasExplicitLength()) return length;
    float last = 0.0f;
    for (const ClipTrackData& track : tracks){
        if (track.channel == ClipChannel::Property) continue;
        last = std::max(last, track.lastKeyTime());
    }
    for (const ClipEventData& event : events){
        last = std::max(last, event.time);
    }
    return last;
}

int ClipData::findTrack(const std::string& path, ClipChannel channel) const{
    if (channel == ClipChannel::Property) return -1;
    // a humanoid track by its bone name (its identity in editors and tools)
    if (isHumanoidChannel(channel)){
        int bone = -1;
        return anim::humanoidTrackBoneFromName(channel, path, bone) ? findHumanoidTrack(channel, bone) : -1;
    }
    for (size_t i = 0; i < tracks.size(); i++){
        if (tracks[i].channel == channel && tracks[i].path == path) return (int)i;
    }
    return -1;
}

bool ClipData::isHumanoid() const{
    for (const ClipTrackData& track : tracks){
        if (isHumanoidChannel(track.channel)) return true;
    }
    return false;
}

int ClipData::findHumanoidTrack(ClipChannel channel, int humanBone) const{
    if (!isHumanoidChannel(channel)) return -1;
    for (size_t i = 0; i < tracks.size(); i++){
        if (tracks[i].channel == channel && tracks[i].humanBone == humanBone) return (int)i;
    }
    return -1;
}

size_t ClipData::valueWidth(ClipChannel channel, size_t morphCount, size_t pointCount){
    switch (channel){
        case ClipChannel::Position:
        case ClipChannel::Scale:
        case ClipChannel::HumanoidPosition:
            return 3;
        case ClipChannel::Rotation:
        case ClipChannel::Humanoid:
            return 4;
        case ClipChannel::Morph:
            return morphCount;
        case ClipChannel::Lattice:
            return 3 * pointCount;
        case ClipChannel::Property:
            return 0;
    }
    return 0;
}

// AnimationClip

AnimationClip::~AnimationClip(){
}

std::shared_ptr<const AnimationClip> AnimationClip::build(const ClipData& data, std::vector<ClipWarning>* warnings){
    return build(data, std::vector<ClipWarning>(), warnings);
}

std::shared_ptr<const AnimationClip> AnimationClip::build(const ClipData& input, const std::vector<ClipWarning>& loadWarnings,
                                                          std::vector<ClipWarning>* warnings){
    ClipData data = input;
    std::vector<ClipWarning> normalizeWarnings;
    anim::normalize(data, &normalizeWarnings);
    if (warnings) warnings->insert(warnings->end(), normalizeWarnings.begin(), normalizeWarnings.end());
    std::vector<ClipWarning> all = loadWarnings;
    all.insert(all.end(), normalizeWarnings.begin(), normalizeWarnings.end());
    return buildNormalized(data, std::move(all));
}

std::shared_ptr<const AnimationClip> AnimationClip::build(const anim::ClipParseResult& parsed){
    if (!parsed.ok()) return nullptr;
    // parseClip's output is normalized already
    return buildNormalized(*parsed.data, parsed.warnings);
}

std::shared_ptr<const AnimationClip> AnimationClip::buildNormalized(const ClipData& data, std::vector<ClipWarning> clipWarnings){
    AnimationClip* clip = new AnimationClip();
    std::shared_ptr<const AnimationClip> shared(clip);
    clip->frameRate = data.frameRate;
    clip->wrap = data.wrap;
    clip->length = data.effectiveLength();
    clip->explicitLength = data.hasExplicitLength();
    clip->additiveReference = std::isfinite(data.additiveReference) ? std::min(std::max(data.additiveReference, 0.0f), clip->length) : 0.0f;
    clip->guid = data.guid;
    clip->previewModel = data.previewModel;
    clip->source = data.source;
    clip->humanoid = data.humanoid;
    clip->unknownKeys = data.unknownKeys;
    clip->warnings = std::move(clipWarnings);
    clip->events = data.events;

    // sizes first: the Track pointers point into the storage, which must not move afterwards
    size_t times = 0, vectors = 0, rotations = 0, weights = 0, points = 0;
    for (const ClipTrackData& track : data.tracks){
        if (track.channel == ClipChannel::Property) continue;
        size_t keys = track.keys.size();
        bool tangents = keys > 0 && track.keys[0].hasTangents();
        size_t copies = tangents ? 3 : 1;
        times += keys;
        switch (track.channel){
            case ClipChannel::Position:
            case ClipChannel::Scale:
            case ClipChannel::HumanoidPosition:
                vectors += keys * copies;
                break;
            case ClipChannel::Rotation:
            case ClipChannel::Humanoid:
                rotations += keys * copies;
                break;
            case ClipChannel::Morph:
                weights += keys * copies * (keys > 0 ? track.keys[0].value.size() : 0);
                break;
            case ClipChannel::Lattice:
                vectors += keys * copies * track.points.size();
                points += track.points.size();
                break;
            case ClipChannel::Property:
                break;
        }
    }
    clip->timeStorage.reserve(times);
    clip->easingStorage.reserve(times);
    clip->vectorStorage.reserve(vectors);
    clip->rotationStorage.reserve(rotations);
    clip->weightStorage.reserve(weights);
    clip->pointStorage.reserve(points);

    for (const ClipTrackData& source : data.tracks){
        Track track;
        track.path = source.path;
        track.channel = source.channel;
        track.relative = source.relative;
        track.loop = source.loop;
        track.humanBone = isHumanoidChannel(source.channel) ? source.humanBone : -1;
        if (isHumanoidChannel(source.channel)) clip->humanoidTracks++;
        track.component = source.component;
        track.property = source.property;
        track.preserved = source.preserved;
        if (source.channel == ClipChannel::Property){
            clip->tracks.push_back(std::move(track));
            continue;
        }
        size_t keys = source.keys.size();
        bool tangents = keys > 0 && source.keys[0].hasTangents();
        track.keyCount = keys;
        track.duration = source.lastKeyTime();

        track.times = clip->timeStorage.data() + clip->timeStorage.size();
        track.easings = clip->easingStorage.data() + clip->easingStorage.size();
        for (const ClipKey& key : source.keys){
            clip->timeStorage.push_back(key.time);
            clip->easingStorage.push_back(key.ease);
        }

        if (isVectorChannel(source.channel)){
            auto append = [&](const std::vector<float> ClipKey::* member){
                const Vector3* start = clip->vectorStorage.data() + clip->vectorStorage.size();
                for (const ClipKey& key : source.keys){
                    const std::vector<float>& v = key.*member;
                    clip->vectorStorage.push_back(Vector3(v[0], v[1], v[2]));
                }
                return start;
            };
            track.vectors = append(&ClipKey::value);
            if (tangents){
                track.inVectors = append(&ClipKey::inTangent);
                track.outVectors = append(&ClipKey::outTangent);
            }
        }else if (isRotationChannel(source.channel)){
            auto append = [&](const std::vector<float> ClipKey::* member){
                const Quaternion* start = clip->rotationStorage.data() + clip->rotationStorage.size();
                for (const ClipKey& key : source.keys){
                    clip->rotationStorage.push_back(quaternionFromXYZW((key.*member).data()));
                }
                return start;
            };
            track.rotations = append(&ClipKey::value);
            if (tangents){
                track.inRotations = append(&ClipKey::inTangent);
                track.outRotations = append(&ClipKey::outTangent);
            }
        }else if (source.channel == ClipChannel::Morph){
            track.width = keys > 0 ? source.keys[0].value.size() : 0;
            auto append = [&](const std::vector<float> ClipKey::* member){
                const float* start = clip->weightStorage.data() + clip->weightStorage.size();
                for (const ClipKey& key : source.keys){
                    const std::vector<float>& v = key.*member;
                    clip->weightStorage.insert(clip->weightStorage.end(), v.begin(), v.end());
                }
                return start;
            };
            track.weights = append(&ClipKey::value);
            if (tangents){
                track.inWeights = append(&ClipKey::inTangent);
                track.outWeights = append(&ClipKey::outTangent);
            }
        }else if (source.channel == ClipChannel::Lattice){
            track.width = source.points.size();
            track.points = clip->pointStorage.data() + clip->pointStorage.size();
            clip->pointStorage.insert(clip->pointStorage.end(), source.points.begin(), source.points.end());
            auto append = [&](const std::vector<float> ClipKey::* member){
                const Vector3* start = clip->vectorStorage.data() + clip->vectorStorage.size();
                for (const ClipKey& key : source.keys){
                    const std::vector<float>& v = key.*member;
                    for (size_t i = 0; i < track.width; i++){
                        clip->vectorStorage.push_back(Vector3(v[3 * i], v[3 * i + 1], v[3 * i + 2]));
                    }
                }
                return start;
            };
            track.vectors = append(&ClipKey::value);
            if (tangents){
                track.inVectors = append(&ClipKey::inTangent);
                track.outVectors = append(&ClipKey::outTangent);
            }
        }
        clip->tracks.push_back(std::move(track));
    }
    return shared;
}

ClipData AnimationClip::toData() const{
    ClipData data;
    data.guid = guid;
    data.frameRate = frameRate;
    data.wrap = wrap;
    data.length = explicitLength ? length : -1.0f;
    data.additiveReference = additiveReference;
    data.events = events;
    data.previewModel = previewModel;
    data.source = source;
    data.humanoid = humanoid;
    data.unknownKeys = unknownKeys;
    for (const Track& track : tracks){
        ClipTrackData out;
        out.path = track.path;
        out.channel = track.channel;
        out.relative = track.relative;
        out.loop = track.loop;
        out.humanBone = track.humanBone;
        out.component = track.component;
        out.property = track.property;
        out.preserved = track.preserved;
        if (track.channel == ClipChannel::Lattice && track.points){
            out.points.assign(track.points, track.points + track.width);
        }
        for (size_t k = 0; k < track.keyCount; k++){
            ClipKey key;
            key.time = track.times[k];
            key.ease = track.easings[k];
            auto vectorRow = [&](const Vector3* values, size_t width, std::vector<float>& row){
                for (size_t i = 0; i < width; i++){
                    const Vector3& v = values[k * width + i];
                    row.push_back(v.x);
                    row.push_back(v.y);
                    row.push_back(v.z);
                }
            };
            auto rotation = [&](const Quaternion* values, std::vector<float>& row){
                row.resize(4);
                quaternionToXYZW(values[k], row.data());
            };
            auto weightRow = [&](const float* values, std::vector<float>& row){
                row.assign(values + k * track.width, values + (k + 1) * track.width);
            };
            switch (track.channel){
                case ClipChannel::Position:
                case ClipChannel::Scale:
                case ClipChannel::HumanoidPosition:
                    vectorRow(track.vectors, 1, key.value);
                    if (track.inVectors){
                        vectorRow(track.inVectors, 1, key.inTangent);
                        vectorRow(track.outVectors, 1, key.outTangent);
                    }
                    break;
                case ClipChannel::Lattice:
                    vectorRow(track.vectors, track.width, key.value);
                    if (track.inVectors){
                        vectorRow(track.inVectors, track.width, key.inTangent);
                        vectorRow(track.outVectors, track.width, key.outTangent);
                    }
                    break;
                case ClipChannel::Rotation:
                case ClipChannel::Humanoid:
                    rotation(track.rotations, key.value);
                    if (track.inRotations){
                        rotation(track.inRotations, key.inTangent);
                        rotation(track.outRotations, key.outTangent);
                    }
                    break;
                case ClipChannel::Morph:
                    weightRow(track.weights, key.value);
                    if (track.inWeights){
                        weightRow(track.inWeights, key.inTangent);
                        weightRow(track.outWeights, key.outTangent);
                    }
                    break;
                case ClipChannel::Property:
                    break;
            }
            out.keys.push_back(std::move(key));
        }
        data.tracks.push_back(std::move(out));
    }
    return data;
}

int AnimationClip::findHumanoidTrack(ClipChannel channel, int humanBone) const{
    if (!isHumanoidChannel(channel)) return -1;
    for (size_t i = 0; i < tracks.size(); i++){
        if (tracks[i].channel == channel && tracks[i].humanBone == humanBone) return (int)i;
    }
    return -1;
}

int AnimationClip::findTrack(const std::string& path, ClipChannel channel) const{
    if (channel == ClipChannel::Property) return -1;
    // a humanoid track by its bone name (its identity in editors and tools)
    if (isHumanoidChannel(channel)){
        int bone = -1;
        return anim::humanoidTrackBoneFromName(channel, path, bone) ? findHumanoidTrack(channel, bone) : -1;
    }
    for (size_t i = 0; i < tracks.size(); i++){
        if (tracks[i].channel == channel && tracks[i].path == path) return (int)i;
    }
    return -1;
}
