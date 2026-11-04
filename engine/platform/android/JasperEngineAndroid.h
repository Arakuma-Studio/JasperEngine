// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef JasperEngineAndroid_H_
#define JasperEngineAndroid_H_

#include <stdio.h>
#include <string>
#include "System.h"

class JasperEngineAndroid: public jasperengine::System {

public:
    virtual int getScreenWidth() override;
    virtual int getScreenHeight() override;

    virtual bool isTouchDevice() override;

    virtual void showVirtualKeyboard(std::wstring text) override;
    virtual void hideVirtualKeyboard() override;

    virtual void setClipboardText(const std::string& utf8) override;
    virtual std::string getClipboardText() override;

    virtual void quit() override;

    virtual std::string getUserDataPath() override;
    // a prebuilt player's content (game/ of the APK's assets, S30); else the assets root
    virtual std::string getAssetPath() override;
    virtual std::string getLuaPath() override;

    virtual FILE* platformFopen(const char* fname, const char* mode) override;

    virtual bool getBoolForKey(const char *key, bool defaultValue) override;
    virtual int getIntegerForKey(const char *key, int defaultValue) override;
    virtual long getLongForKey(const char *key, long defaultValue) override;
    virtual float getFloatForKey(const char *key, float defaultValue) override;
    virtual double getDoubleForKey(const char *key, double defaultValue) override;
    virtual std::string getStringForKey(const char *key, const std::string& defaultValue) override;

    virtual void setBoolForKey(const char *key, bool value) override;
    virtual void setIntegerForKey(const char *key, int value) override;
    virtual void setLongForKey(const char *key, long value) override;
    virtual void setFloatForKey(const char *key, float value) override;
    virtual void setDoubleForKey(const char *key, double value) override;
    virtual void setStringForKey(const char* key, const std::string& value) override;

    virtual void removeKey(const char* key) override;
    virtual bool hasKey(const char* key) override;
    virtual void removeAllKeys() override;
    virtual void saveUserSettings() override;

    // platform services (JasperPlatform.java)
    virtual std::vector<std::string> getPreferredLocales() override;
    virtual std::string getRegionOverride() override;
    virtual std::string getAppVersion() override;
    virtual int64_t getAppBuild() override;
    virtual std::string getAppIdentifier() override;
    virtual std::string getAppName() override;
    virtual std::string getOperatingSystem() override;
    virtual std::string getOperatingSystemVersion() override;
    virtual int getAndroidApiLevel() override;
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

protected:
    virtual bool platformOpenURL(const std::string& url) override;
};

#endif /* JasperEngineAndroid_H_ */
