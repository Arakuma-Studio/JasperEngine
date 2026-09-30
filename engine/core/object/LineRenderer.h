// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef LINE_RENDERER_H
#define LINE_RENDERER_H

#include "GameObject.h"

namespace jasperengine{

    class JASPERENGINE_API LineRenderer: public GameObject{
    public:

        // component checkbox (Unity Behaviour.enabled): a disabled Lines is skipped by its system
        void setEnabled(bool enabled);
        bool isEnabled() const;
        LineRenderer(Scene* scene);
        LineRenderer(Scene* scene, Entity entity);
        virtual ~LineRenderer();

        bool load();

        void setMaxLines(unsigned int maxLines);
        unsigned int getMaxLines() const;

        void addLine(LineData line);
        void addLine(Vector3 pointA, Vector3 pointB);
        void addLine(Vector3 pointA, Vector3 pointB, Vector3 color);
        void addLine(Vector3 pointA, Vector3 pointB, Vector4 color);
        void addLine(Vector3 pointA, Vector3 pointB, Vector4 colorA, Vector4 colorB);

        LineData& getLine(size_t index);

        void updateLine(size_t index, LineData line);
        void updateLine(size_t index, Vector3 pointA, Vector3 pointB);
        void updateLine(size_t index, Vector3 pointA, Vector3 pointB, Vector3 color);
        void updateLine(size_t index, Vector3 pointA, Vector3 pointB, Vector4 color);
        void updateLine(size_t index, Vector3 pointA, Vector3 pointB, Vector4 colorA, Vector4 colorB);
        void updateLine(size_t index, Vector3 color);
        void updateLine(size_t index, Vector4 color);
        void updateLine(size_t index, Vector4 colorA, Vector4 colorB);

        void removeLine(size_t index);

        void updateLines();
        size_t getNumLines();

        void clearLines();

        void setCustomShader(const std::string& path);
        std::string getCustomShader() const;

        void setShaderUniform(const std::string& name, const Vector4& value);
        void setShaderUniform(const std::string& name, const Vector3& value);
        void setShaderUniform(const std::string& name, const Vector2& value);
        void setShaderUniform(const std::string& name, float value);
        Vector4 getShaderUniform(const std::string& name) const;
        bool removeShaderUniform(const std::string& name);
    };
}

#endif //LINE_RENDERER_H