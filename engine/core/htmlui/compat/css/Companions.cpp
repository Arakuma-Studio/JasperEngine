// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "htmlui/compat/css/Companions.h"
#include "htmlui/compat/LiteralCompare.h"

#include "htmlui/compat/css/RewriteSession.h"

using namespace jasperengine::htmlui::compat;

std::string companions::boxShadowList(){
    return std::string("var(") + kOutlineShadow + ", " + kNoShadow + "), var(" + kBoxShadow + ", " + kNoShadow + ")";
}

std::string companions::decoratorList(){
    return std::string("var(") + kBorderImage + ", " + kNoLayer + "), var(" + kBgLayers + ", " + kNoLayer + ")";
}

std::string companions::fontEffectList(){
    return std::string("var(") + kTextStroke + ", " + kNoEffect + "), var(" + kTextShadow + ", " + kNoEffect + ")";
}

std::string companions::inheritName(const std::string& rcssProperty){
    return "--html-inh-" + rcssProperty;
}

void companions::emitBoxShadow(DeclContext& context, const std::string& value){
    if (context.keyframe){
        context.emit("box-shadow", value);
        return;
    }
    context.emit(kBoxShadow, value == "none" ? kNoShadow : value);
    context.emit("box-shadow", boxShadowList());
}

void companions::emitOutline(DeclContext& context, const std::string& ring){
    if (context.keyframe){
        context.emit("box-shadow", ring);
        return;
    }
    context.emit(kOutlineShadow, ring);
    context.emit("box-shadow", boxShadowList());
}

void companions::emitBackgroundLayers(DeclContext& context, const std::string& layers){
    if (context.keyframe || !context.session.usesBorderImage){
        context.emit("decorator", layers.empty() ? "none" : layers);
        return;
    }
    context.emit(kBgLayers, layers.empty() ? kNoLayer : layers);
    context.emit("decorator", decoratorList());
}

void companions::emitBorderImage(DeclContext& context, const std::string& decorator){
    if (context.keyframe){
        context.emit("decorator", decorator.empty() ? "none" : decorator);
        return;
    }
    context.emit(kBorderImage, decorator.empty() ? kNoLayer : decorator);
    context.emit("decorator", decoratorList());
}

void companions::emitTextShadow(DeclContext& context, const std::string& effects){
    if (context.keyframe || !(context.session.usesTextShadow && context.session.usesTextStroke)){
        context.emit("font-effect", effects.empty() ? "none" : effects);
        return;
    }
    context.emit(kTextShadow, effects.empty() ? kNoEffect : effects);
    context.emit("font-effect", fontEffectList());
}

void companions::emitTextStroke(DeclContext& context, const std::string& effect){
    if (context.keyframe || !(context.session.usesTextShadow && context.session.usesTextStroke)){
        context.emit("font-effect", effect.empty() ? "none" : effect);
        return;
    }
    context.emit(kTextStroke, effect.empty() ? kNoEffect : effect);
    context.emit("font-effect", fontEffectList());
}
