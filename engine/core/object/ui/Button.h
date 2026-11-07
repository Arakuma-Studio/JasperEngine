// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef BUTTON_H
#define BUTTON_H

#include "Image.h"
#include "Text.h"
#include "component/UISelectable.h"

namespace jasperengine{
    class JASPERENGINE_API Button: public Image{
    private:
        void ensureLabel();

    public:
        Button(Scene* scene);
        Button(Scene* scene, Entity entity);
        virtual ~Button();

        Button(const Button& rhs);
        Button& operator=(const Button& rhs);
        Button(Button&& rhs) noexcept;
        Button& operator=(Button&& rhs) noexcept;

        bool hasLabel() const;
        Text getLabelObject() const;

        void setLabel(const std::string& text);
        std::string getLabel() const;

        void setLabelColor(Vector4 color);
        void setLabelColor(const float red, const float green, const float blue, const float alpha);
        Vector4 getLabelColor() const;

        void setLabelFont(const std::string& font);
        std::string getLabelFont() const;

        void setLabelFontSize(unsigned int fontSize);
        unsigned int getLabelFontSize() const;

        void setTextureNormal(const std::string& path);
        void setTextureHovered(const std::string& path);
        void setTexturePressed(const std::string& path);
        void setTextureDisabled(const std::string& path);

        void setColorNormal(Vector4 color);
        void setColorNormal(const float red, const float green, const float blue, const float alpha);
        void setColorNormal(const float red, const float green, const float blue);
        Vector4 getColorNormal() const;

        void setColorHovered(Vector4 color);
        void setColorHovered(const float red, const float green, const float blue, const float alpha);
        void setColorHovered(const float red, const float green, const float blue);
        Vector4 getColorHovered() const;

        void setColorPressed(Vector4 color);
        void setColorPressed(const float red, const float green, const float blue, const float alpha);
        void setColorPressed(const float red, const float green, const float blue);
        Vector4 getColorPressed() const;

        void setColorDisabled(Vector4 color);
        void setColorDisabled(const float red, const float green, const float blue, const float alpha);
        void setColorDisabled(const float red, const float green, const float blue);
        Vector4 getColorDisabled() const;

        void setDisabled(bool disabled);
        bool getDisabled() const;
        // Unity's Selectable.interactable (not disabled)
        void setInteractable(bool interactable);
        bool isInteractable() const;

        // the image's colour and texture: on a Button whose transition writes them, its Normal
        // state's (what the state changes would otherwise overwrite)
        void setColor(Vector4 color);
        void setColor(const float red, const float green, const float blue, const float alpha);
        void setColor(const float red, const float green, const float blue);
        using Image::setTexture;
        void setTexture(const std::string& path);

        void setTextureSelected(const std::string& path);
        void setColorSelected(Vector4 color);
        void setColorSelected(const float red, const float green, const float blue, const float alpha);
        void setColorSelected(const float red, const float green, const float blue);
        Vector4 getColorSelected() const;

        // ui-components: Unity's transitions
        void setTransition(ButtonTransition transition);
        ButtonTransition getTransition() const;
        void setFadeDuration(float seconds);
        float getFadeDuration() const;
        void setScaleHovered(float scale);
        float getScaleHovered() const;
        void setScalePressed(float scale);
        float getScalePressed() const;
        void setScaleSelected(float scale);
        float getScaleSelected() const;

        // Unity's Navigation and selection (keyboard, gamepad, TV remote)
        void setNavigationMode(NavigationMode mode);
        NavigationMode getNavigationMode() const;
        void setWrapAround(bool wrapAround);
        bool getWrapAround() const;
        void setSelectOnUp(Entity entity);
        Entity getSelectOnUp() const;
        void setSelectOnDown(Entity entity);
        Entity getSelectOnDown() const;
        void setSelectOnLeft(Entity entity);
        Entity getSelectOnLeft() const;
        void setSelectOnRight(Entity entity);
        Entity getSelectOnRight() const;
        void setSelectOnEnable(bool selectOnEnable);
        bool getSelectOnEnable() const;
        // becomes its scene's selection (Unity's Select); false when it cannot be (inactive, hidden)
        bool select();
        bool isSelected() const;
        SelectionState getState() const;
        // where navigation goes from it that way (Unity's FindSelectableOnUp...): NULL_ENTITY for none
        Entity findSelectableOnUp() const;
        Entity findSelectableOnDown() const;
        Entity findSelectableOnLeft() const;
        Entity findSelectableOnRight() const;
    };
}

#endif //BUTTON_H