// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Lua's spline classes (docs/specs/S18-splines.md R28): Spline (the curve in world space, its
// queries and knot editing), Spline Extrude, Spline Instantiate and Spline Animate, the frame
// and hit values their queries return, and their enums.

#include "LuaBinding.h"

#include "lua.hpp"

#include "LuaBridge.h"
#include "LuaBridgeAddon.h"

#include "object/spline/Spline.h"
#include "object/spline/SplineAnimate.h"
#include "object/spline/SplineExtrude.h"
#include "object/spline/SplineInstantiate.h"

using namespace jasperengine;
// the spline core's value types by their own names (the Lua API extractor matches bare names)
using jasperengine::spline::SplineFrame;
using jasperengine::spline::SplineHit;
using jasperengine::spline::SplineType;
using jasperengine::spline::TangentMode;

void LuaBinding::registerSplineClasses(lua_State *L){
    luabridge::getGlobalNamespace(L)
        .beginNamespace("SplineType")
        .addVariable("Linear", SplineType::Linear)
        .addVariable("CatmullRom", SplineType::CatmullRom)
        .addVariable("CubicBezier", SplineType::CubicBezier)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("SplineTangentMode")
        .addVariable("Mirrored", TangentMode::Mirrored)
        .addVariable("Aligned", TangentMode::Aligned)
        .addVariable("Broken", TangentMode::Broken)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("SplineExtrudeShape")
        .addVariable("Flat", SplineExtrudeShape::Flat)
        .addVariable("Bevel", SplineExtrudeShape::Bevel)
        .addVariable("Crown", SplineExtrudeShape::Crown)
        .addVariable("Rectangle", SplineExtrudeShape::Rectangle)
        .addVariable("Tube", SplineExtrudeShape::Tube)
        .addVariable("Custom", SplineExtrudeShape::Custom)
        .addVariable("Fill", SplineExtrudeShape::Fill)
        .addVariable("WaterBanks", SplineExtrudeShape::WaterBanks)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("SplineWidthSource")
        .addVariable("Radius", SplineWidthSource::Radius)
        .addVariable("Constant", SplineWidthSource::Constant)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("SplineCornerStyle")
        .addVariable("None", SplineCornerStyle::None)
        .addVariable("Mitre", SplineCornerStyle::Mitre)
        .addVariable("Round", SplineCornerStyle::Round)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("SplineGrade")
        .addVariable("Racked", SplineGrade::Racked)
        .addVariable("Stepped", SplineGrade::Stepped)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("SplineConform")
        .addVariable("None", SplineConform::None)
        .addVariable("TerrainHeight", SplineConform::TerrainHeight)
        .addVariable("TerrainHeightAndSlope", SplineConform::TerrainHeightAndSlope)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("SplineCollision")
        .addVariable("None", SplineCollision::None)
        .addVariable("Mesh", SplineCollision::Mesh)
        .addVariable("Chain2D", SplineCollision::Chain2D)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("SplineInstantiateMode")
        .addVariable("Spacing", SplineInstantiateMode::Spacing)
        .addVariable("Tile", SplineInstantiateMode::Tile)
        .addVariable("Fence", SplineInstantiateMode::Fence)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("SplineSpacingFit")
        .addVariable("FitToLength", SplineSpacingFit::FitToLength)
        .addVariable("FixedPitch", SplineSpacingFit::FixedPitch)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("SplinePlantMode")
        .addVariable("BoundsMin", SplinePlantMode::BoundsMin)
        .addVariable("Pivot", SplinePlantMode::Pivot)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("SplineAnimateMethod")
        .addVariable("Duration", SplineAnimateMethod::Duration)
        .addVariable("Speed", SplineAnimateMethod::Speed)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("SplineLoop")
        .addVariable("Once", SplineLoop::Once)
        .addVariable("Loop", SplineLoop::Loop)
        .addVariable("PingPong", SplineLoop::PingPong)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("SplineEasing")
        .addVariable("None", SplineEasing::None)
        .addVariable("EaseIn", SplineEasing::EaseIn)
        .addVariable("EaseOut", SplineEasing::EaseOut)
        .addVariable("EaseInOut", SplineEasing::EaseInOut)
        .endNamespace();

    luabridge::getGlobalNamespace(L)
        .beginNamespace("SplineAlignment")
        .addVariable("None", SplineAlignment::None)
        .addVariable("SplineTangent", SplineAlignment::SplineTangent)
        .addVariable("SplineTangentAndUp", SplineAlignment::SplineTangentAndUp)
        .addVariable("World", SplineAlignment::World)
        .endNamespace();

    // what the queries return: a frame on the curve and a closest point
    luabridge::getGlobalNamespace(L)
        .beginClass<SplineFrame>("SplineFrame")
        .addProperty("position", &SplineFrame::position)
        .addProperty("forward", &SplineFrame::forward)
        .addProperty("right", &SplineFrame::right)
        .addProperty("up", &SplineFrame::up)
        .addProperty("radius", &SplineFrame::radius)
        .addProperty("roll", &SplineFrame::rollDeg)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .beginClass<SplineHit>("SplineHit")
        .addProperty("t", &SplineHit::t)
        .addProperty("distance", &SplineHit::distance)
        .addProperty("position", &SplineHit::position)
        .addProperty("segment", &SplineHit::segment)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .deriveClass<Spline, EntityHandle>("Spline")
        .addConstructor <void (Scene*, Entity)> ()
        .addProperty("valid", &Spline::isValid)
        .addProperty("enabled", &Spline::isEnabled, &Spline::setEnabled)
        .addProperty("type", &Spline::getType, &Spline::setType)
        .addProperty("closed", &Spline::isClosed, &Spline::setClosed)
        .addProperty("is3D", &Spline::is3D, &Spline::set3D)
        .addProperty("knotCount", &Spline::getKnotCount)
        .addProperty("version", &Spline::getVersion)
        .addFunction("getKnotCount", &Spline::getKnotCount)
        .addFunction("addKnot", &Spline::addKnot)
        .addFunction("insertKnot", &Spline::insertKnot)
        .addFunction("removeKnot", &Spline::removeKnot)
        .addFunction("clearKnots", &Spline::clearKnots)
        .addFunction("getKnotPosition", &Spline::getKnotPosition)
        .addFunction("setKnotPosition", &Spline::setKnotPosition)
        .addFunction("getKnotRadius", &Spline::getKnotRadius)
        .addFunction("setKnotRadius", &Spline::setKnotRadius)
        .addFunction("getKnotRoll", &Spline::getKnotRoll)
        .addFunction("setKnotRoll", &Spline::setKnotRoll)
        .addFunction("getKnotTangentIn", &Spline::getKnotTangentIn)
        .addFunction("getKnotTangentOut", &Spline::getKnotTangentOut)
        .addFunction("setKnotTangents", &Spline::setKnotTangents)
        .addFunction("getKnotMode", &Spline::getKnotMode)
        .addFunction("setKnotMode", &Spline::setKnotMode)
        .addFunction("getKnotPositions", &Spline::getKnotPositions)
        .addFunction("getLength", &Spline::getLength)
        .addFunction("evaluate", &Spline::evaluate)
        .addFunction("evaluateAtDistance", &Spline::evaluateAtDistance)
        .addFunction("getPosition", &Spline::getPosition)
        .addFunction("tToDistance", &Spline::tToDistance)
        .addFunction("distanceToT", &Spline::distanceToT)
        .addFunction("getClosestPoint", &Spline::getClosestPoint)
        .addFunction("getClosestPointXZ", &Spline::getClosestPointXZ)
        .addFunction("getSignedDistance", &Spline::getSignedDistance)
        .addFunction("getSignedDistanceXZ", &Spline::getSignedDistanceXZ)
        .addFunction("getSignedDistanceBandXZ", &Spline::getSignedDistanceBandXZ)
        .addFunction("sampleUniform", &Spline::sampleUniform)
        .addFunction("resample", &Spline::resample)
        .addFunction("simplify", &Spline::simplify)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .deriveClass<SplineExtrude, EntityHandle>("SplineExtrude")
        .addConstructor <void (Scene*, Entity)> ()
        .addProperty("valid", &SplineExtrude::isValid)
        .addProperty("enabled", &SplineExtrude::isEnabled, &SplineExtrude::setEnabled)
        .addProperty("shape", &SplineExtrude::getShape, &SplineExtrude::setShape)
        .addProperty("widthSource", &SplineExtrude::getWidthSource, &SplineExtrude::setWidthSource)
        .addProperty("width", &SplineExtrude::getWidth, &SplineExtrude::setWidth)
        .addProperty("edgeDrop", &SplineExtrude::getEdgeDrop, &SplineExtrude::setEdgeDrop)
        .addProperty("edgeInset", &SplineExtrude::getEdgeInset, &SplineExtrude::setEdgeInset)
        .addProperty("crownRise", &SplineExtrude::getCrownRise, &SplineExtrude::setCrownRise)
        .addProperty("shoulderWidth", &SplineExtrude::getShoulderWidth, &SplineExtrude::setShoulderWidth)
        .addProperty("shoulderDrop", &SplineExtrude::getShoulderDrop, &SplineExtrude::setShoulderDrop)
        .addProperty("thickness", &SplineExtrude::getThickness, &SplineExtrude::setThickness)
        .addProperty("height", &SplineExtrude::getHeight, &SplineExtrude::setHeight)
        .addProperty("grade", &SplineExtrude::getGrade, &SplineExtrude::setGrade)
        .addProperty("stepHeight", &SplineExtrude::getStepHeight, &SplineExtrude::setStepHeight)
        .addProperty("tubeSides", &SplineExtrude::getTubeSides, &SplineExtrude::setTubeSides)
        .addProperty("maxHalfWidth", &SplineExtrude::getMaxHalfWidth, &SplineExtrude::setMaxHalfWidth)
        .addProperty("waterEdgeDrop", &SplineExtrude::getWaterEdgeDrop, &SplineExtrude::setWaterEdgeDrop)
        .addProperty("flow", &SplineExtrude::isFlow, &SplineExtrude::setFlow)
        .addProperty("flowSpeed", &SplineExtrude::getFlowSpeed, &SplineExtrude::setFlowSpeed)
        .addProperty("tilesPerMetreU", &SplineExtrude::getTilesPerMetreU, &SplineExtrude::setTilesPerMetreU)
        .addProperty("tilesPerMetreV", &SplineExtrude::getTilesPerMetreV, &SplineExtrude::setTilesPerMetreV)
        .addProperty("levelCourseV", &SplineExtrude::isLevelCourseV, &SplineExtrude::setLevelCourseV)
        .addProperty("corners", &SplineExtrude::getCorners, &SplineExtrude::setCorners)
        .addProperty("endCaps", &SplineExtrude::isEndCaps, &SplineExtrude::setEndCaps)
        .addProperty("endTaper", &SplineExtrude::getEndTaper, &SplineExtrude::setEndTaper)
        .addProperty("lateralOffset", &SplineExtrude::getLateralOffset, &SplineExtrude::setLateralOffset)
        .addProperty("verticalOffset", &SplineExtrude::getVerticalOffset, &SplineExtrude::setVerticalOffset)
        .addProperty("conform", &SplineExtrude::getConform, &SplineExtrude::setConform)
        .addProperty("slopeBlend", &SplineExtrude::getSlopeBlend, &SplineExtrude::setSlopeBlend)
        .addProperty("stationSpacing", &SplineExtrude::getStationSpacing, &SplineExtrude::setStationSpacing)
        .addProperty("collision", &SplineExtrude::getCollision, &SplineExtrude::setCollision)
        .addProperty("vertexCount", &SplineExtrude::getVertexCount)
        .addProperty("chunkCount", &SplineExtrude::getChunkCount)
        .addProperty("pending", &SplineExtrude::isPending)
        .addFunction("rebuild", &SplineExtrude::rebuild)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .deriveClass<SplineInstantiate, EntityHandle>("SplineInstantiate")
        .addConstructor <void (Scene*, Entity)> ()
        .addProperty("valid", &SplineInstantiate::isValid)
        .addProperty("enabled", &SplineInstantiate::isEnabled, &SplineInstantiate::setEnabled)
        .addProperty("mode", &SplineInstantiate::getMode, &SplineInstantiate::setMode)
        .addProperty("spacing", &SplineInstantiate::getSpacing, &SplineInstantiate::setSpacing)
        .addProperty("fit", &SplineInstantiate::getFit, &SplineInstantiate::setFit)
        .addProperty("postPitch", &SplineInstantiate::getPostPitch, &SplineInstantiate::setPostPitch)
        .addProperty("spanMaxStretch", &SplineInstantiate::getSpanMaxStretch, &SplineInstantiate::setSpanMaxStretch)
        .addProperty("fenceGrade", &SplineInstantiate::getFenceGrade, &SplineInstantiate::setFenceGrade)
        .addProperty("crestPitch", &SplineInstantiate::getCrestPitch, &SplineInstantiate::setCrestPitch)
        .addProperty("lateralOffset", &SplineInstantiate::getLateralOffset, &SplineInstantiate::setLateralOffset)
        .addProperty("verticalOffset", &SplineInstantiate::getVerticalOffset, &SplineInstantiate::setVerticalOffset)
        .addProperty("spacingJitter", &SplineInstantiate::getSpacingJitter, &SplineInstantiate::setSpacingJitter)
        .addProperty("yawJitter", &SplineInstantiate::getYawJitter, &SplineInstantiate::setYawJitter)
        .addProperty("lateralJitter", &SplineInstantiate::getLateralJitter, &SplineInstantiate::setLateralJitter)
        .addProperty("scaleRange", &SplineInstantiate::getScaleRange, &SplineInstantiate::setScaleRange)
        .addProperty("dropout", &SplineInstantiate::getDropout, &SplineInstantiate::setDropout)
        .addProperty("seed", &SplineInstantiate::getSeed, &SplineInstantiate::setSeed)
        .addProperty("endTrim", &SplineInstantiate::getEndTrim, &SplineInstantiate::setEndTrim)
        .addProperty("plantMode", &SplineInstantiate::getPlantMode, &SplineInstantiate::setPlantMode)
        .addProperty("maxTilt", &SplineInstantiate::getMaxTilt, &SplineInstantiate::setMaxTilt)
        .addProperty("conform", &SplineInstantiate::getConform, &SplineInstantiate::setConform)
        .addProperty("slopeBlend", &SplineInstantiate::getSlopeBlend, &SplineInstantiate::setSlopeBlend)
        .addProperty("stationSpacing", &SplineInstantiate::getStationSpacing, &SplineInstantiate::setStationSpacing)
        .addProperty("instanceCount", &SplineInstantiate::getInstanceCount)
        .addProperty("itemCount", &SplineInstantiate::getItemCount)
        .addProperty("pending", &SplineInstantiate::isPending)
        .addFunction("addItem", &SplineInstantiate::addItem)
        .addFunction("clearItems", &SplineInstantiate::clearItems)
        .addFunction("rebuild", &SplineInstantiate::rebuild)
        .endClass();

    luabridge::getGlobalNamespace(L)
        .deriveClass<SplineAnimate, EntityHandle>("SplineAnimate")
        .addConstructor <void (Scene*, Entity)> ()
        .addProperty("valid", &SplineAnimate::isValid)
        .addProperty("enabled", &SplineAnimate::isEnabled, &SplineAnimate::setEnabled)
        .addProperty("spline", &SplineAnimate::getSpline, &SplineAnimate::setSpline)
        .addProperty("method", &SplineAnimate::getMethod, &SplineAnimate::setMethod)
        .addProperty("duration", &SplineAnimate::getDuration, &SplineAnimate::setDuration)
        .addProperty("speed", &SplineAnimate::getSpeed, &SplineAnimate::setSpeed)
        .addProperty("loop", &SplineAnimate::getLoop, &SplineAnimate::setLoop)
        .addProperty("easing", &SplineAnimate::getEasing, &SplineAnimate::setEasing)
        .addProperty("alignment", &SplineAnimate::getAlignment, &SplineAnimate::setAlignment)
        .addProperty("startOffset", &SplineAnimate::getStartOffset, &SplineAnimate::setStartOffset)
        .addProperty("lateralOffset", &SplineAnimate::getLateralOffset, &SplineAnimate::setLateralOffset)
        .addProperty("verticalOffset", &SplineAnimate::getVerticalOffset, &SplineAnimate::setVerticalOffset)
        .addProperty("playOnAwake", &SplineAnimate::isPlayOnAwake, &SplineAnimate::setPlayOnAwake)
        .addProperty("playing", &SplineAnimate::isPlaying)
        .addProperty("completed", &SplineAnimate::isCompleted)
        .addProperty("normalizedTime", &SplineAnimate::getNormalizedTime)
        .addProperty("distance", &SplineAnimate::getDistance, &SplineAnimate::setDistance)
        .addProperty("onComplete", [] (SplineAnimate* self, lua_State*) -> FunctionSubscribe<void()>* { return &self->getOnComplete(); },
                                   [] (SplineAnimate* self, lua_State* L) { self->getOnComplete() = L; })
        .addFunction("play", &SplineAnimate::play)
        .addFunction("pause", &SplineAnimate::pause)
        .addFunction("restart", &SplineAnimate::restart)
        .addFunction("setOnComplete", [] (SplineAnimate* self, lua_State* L) { self->getOnComplete() = L; })
        .endClass();
}
