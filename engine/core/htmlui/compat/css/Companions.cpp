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

void companions::emitBoxShadow(DeclContext& context, std::string_view value){
    if (context.keyframe){
        context.emit("box-shadow", Text(value));
        return;
    }
    context.emit(kBoxShadow, value == "none" ? Text::view(kNoShadow) : Text(value));
    context.emit("box-shadow", boxShadowList());
}

void companions::emitOutline(DeclContext& context, std::string_view ring){
    if (context.keyframe){
        context.emit("box-shadow", Text(ring));
        return;
    }
    context.emit(kOutlineShadow, Text(ring));
    context.emit("box-shadow", boxShadowList());
}

void companions::emitBackgroundLayers(DeclContext& context, std::string_view layers){
    if (context.keyframe || !context.session.usesBorderImage){
        context.emit("decorator", layers.empty() ? Text::view("none") : Text(layers));
        return;
    }
    context.emit(kBgLayers, layers.empty() ? Text::view(kNoLayer) : Text(layers));
    context.emit("decorator", decoratorList());
}

void companions::emitBorderImage(DeclContext& context, std::string_view decorator){
    if (context.keyframe){
        context.emit("decorator", decorator.empty() ? Text::view("none") : Text(decorator));
        return;
    }
    context.emit(kBorderImage, decorator.empty() ? Text::view(kNoLayer) : Text(decorator));
    context.emit("decorator", decoratorList());
}

void companions::emitTextShadow(DeclContext& context, std::string_view effects){
    if (context.keyframe || !(context.session.usesTextShadow && context.session.usesTextStroke)){
        context.emit("font-effect", effects.empty() ? Text::view("none") : Text(effects));
        return;
    }
    context.emit(kTextShadow, effects.empty() ? Text::view(kNoEffect) : Text(effects));
    context.emit("font-effect", fontEffectList());
}

void companions::emitTextStroke(DeclContext& context, std::string_view effect){
    if (context.keyframe || !(context.session.usesTextShadow && context.session.usesTextStroke)){
        context.emit("font-effect", effect.empty() ? Text::view("none") : Text(effect));
        return;
    }
    context.emit(kTextStroke, effect.empty() ? Text::view(kNoEffect) : Text(effect));
    context.emit("font-effect", fontEffectList());
}
