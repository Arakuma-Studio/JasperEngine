// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef REFLECTIONPROBE_H
#define REFLECTIONPROBE_H

#include "object/GameObject.h"
#include "component/ReflectionProbeComponent.h"

namespace jasperengine{

    class JASPERENGINE_API ReflectionProbe: public GameObject{

    private:
        // capture-affecting change: restart any in-flight six-face job so no
        // face is rendered with the old settings
        void invalidateCapture();

    public:
        ReflectionProbe(Scene* scene);
        ReflectionProbe(Scene* scene, Entity entity);
        virtual ~ReflectionProbe();

        ReflectionProbe(const ReflectionProbe& rhs);
        ReflectionProbe& operator=(const ReflectionProbe& rhs);
        ReflectionProbe(ReflectionProbe&& rhs) noexcept;
        ReflectionProbe& operator=(ReflectionProbe&& rhs) noexcept;

        void setMode(ReflectionProbeMode mode);
        ReflectionProbeMode getMode() const;

        void setUpdateMode(ReflectionProbeUpdateMode updateMode);
        ReflectionProbeUpdateMode getUpdateMode() const;

        void setBoxOffset(Vector3 boxOffset);
        void setBoxOffset(const float x, const float y, const float z);
        Vector3 getBoxOffset() const;

        void setBoxSize(Vector3 boxSize);
        void setBoxSize(const float x, const float y, const float z);
        Vector3 getBoxSize() const;

        void setBlendDistance(float blendDistance);
        float getBlendDistance() const;

        void setIntensity(float intensity);
        float getIntensity() const;

        void setPriority(int priority);
        int getPriority() const;

        void setResolution(unsigned int resolution);
        unsigned int getResolution() const;

        void setClipPlanes(float nearClip, float farClip);
        void setNearClip(float nearClip);
        float getNearClip() const;
        void setFarClip(float farClip);
        float getFarClip() const;

        void setUpdateInterval(float updateInterval);
        float getUpdateInterval() const;

        void setIncludeSky(bool includeSky);
        bool isIncludeSky() const;

        // Unity ReflectionProbe.cullingMask: layers the capture draws
        void setCullingMask(uint32_t cullingMask);
        uint32_t getCullingMask() const;

        // Authored six-face cubemap for static probes (see ReflectionProbeMode)
        void setTextures(const std::string& texturePositiveX, const std::string& textureNegativeX,
                        const std::string& texturePositiveY, const std::string& textureNegativeY,
                        const std::string& texturePositiveZ, const std::string& textureNegativeZ);
        void setTexture(const std::string& texture);

        // Requests a new capture (or re-bake of the authored cubemap),
        // regardless of mode and update mode
        void refresh();
    };
}

#endif //REFLECTIONPROBE_H
