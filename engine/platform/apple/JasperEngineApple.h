// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef JasperEngineApple_h
#define JasperEngineApple_h

#include "System.h"

class JasperEngineApple: public jasperengine::System{

public:

    JasperEngineApple();

    virtual ~JasperEngineApple();

    virtual sg_environment getSokolEnvironment() override;
    virtual sg_swapchain getSokolSwapchain() override;

    virtual void setMouseCursor(jasperengine::CursorType type) override;
    virtual void setMouseMode(jasperengine::MouseMode mode) override;

    virtual int getScreenWidth() override;
    virtual int getScreenHeight() override;

    virtual int getSampleCount() override;

    virtual bool isTouchDevice() override;

    virtual void showVirtualKeyboard(std::wstring text) override;
    virtual void hideVirtualKeyboard() override;

    virtual void setClipboardText(const std::string& utf8) override;
    virtual std::string getClipboardText() override;
    virtual void setTextInputArea(float x, float y, float width, float height) override;

    virtual void quit() override;

    virtual std::string getAssetPath() override;
    virtual std::string getUserDataPath() override;
    virtual std::string getCachePath() override;
    virtual std::string getCrashReportPath() override;
    virtual std::string getLuaPath() override;
    virtual std::string getShaderPath() override;

    virtual bool getBoolForKey(const char *key, bool defaultValue) override;
    virtual int getIntegerForKey(const char *key, int defaultValue) override;
    virtual long getLongForKey(const char *key, long defaultValue) override;
    virtual float getFloatForKey(const char *key, float defaultValue) override;
    virtual double getDoubleForKey(const char *key, double defaultValue) override;
    virtual jasperengine::Data getDataForKey(const char* key, const jasperengine::Data& defaultValue) override;
    virtual std::string getStringForKey(const char *key, const std::string& defaultValue) override;

    virtual void setBoolForKey(const char *key, bool value) override;
    virtual void setIntegerForKey(const char *key, int value) override;
    virtual void setLongForKey(const char *key, long value) override;
    virtual void setFloatForKey(const char *key, float value) override;
    virtual void setDoubleForKey(const char *key, double value) override;
    virtual void setDataForKey(const char* key, jasperengine::Data& value) override;
    virtual void setStringForKey(const char* key, const std::string& value) override;

    virtual void removeKey(const char *key) override;
    virtual bool hasKey(const char *key) override;
    virtual void removeAllKeys() override;
    virtual void saveUserSettings() override;

    // platform services (main bundle, NSLocale, UIKit / AppKit)
    virtual std::vector<std::string> getPreferredLocales() override;
    virtual std::string getRegionOverride() override;
    virtual std::string getAppVersion() override;
    virtual int64_t getAppBuild() override;
    virtual std::string getAppIdentifier() override;
    virtual std::string getAppName() override;
    virtual std::string getOperatingSystem() override;
    virtual std::string getOperatingSystemVersion() override;
    virtual std::string getDeviceModel() override;
    virtual jasperengine::DeviceType getDeviceType() override;
    virtual int getSystemMemorySize() override;
    virtual jasperengine::NetworkReachability getInternetReachability() override;
    virtual bool vibrate(int milliseconds) override;
    virtual bool hapticFeedback(jasperengine::HapticFeedback type) override;
    virtual float getScreenDpi() override;
    virtual float getRefreshRate() override;
    virtual void setKeepScreenOn(bool keepOn) override;
    virtual void applyOrientationPolicy(jasperengine::ScreenOrientation requested, uint8_t allowedMask) override;
    virtual jasperengine::ScreenOrientation getScreenOrientation() override;
    virtual jasperengine::DeviceOrientation getDeviceOrientation() override;
    virtual void applyFramePacing(const jasperengine::FramePacing& pacing) override;
    // the MTKView backend of macOS presents synchronized
    virtual bool canDisableVSync() override;

protected:
    virtual bool platformOpenURL(const std::string& url) override;
};


#endif /* JasperEngineApple_h */
