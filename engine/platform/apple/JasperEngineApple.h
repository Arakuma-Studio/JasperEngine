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

    virtual void quit() override;

    virtual std::string getAssetPath() override;
    virtual std::string getUserDataPath() override;
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

    virtual void initializeAdMob(bool tagForChildDirectedTreatment, bool tagForUnderAgeOfConsent) override;
    virtual void setMaxAdContentRating(jasperengine::AdMobRating rating) override;
    virtual void loadInterstitialAd(const std::string& adUnitID) override;
    virtual bool isInterstitialAdLoaded() override;
    virtual void showInterstitialAd() override;
};


#endif /* JasperEngineApple_h */
