package com.jaspergameengine.jasperengine;

import android.app.Activity;
import android.content.Context;
import android.content.SharedPreferences;

public class UserSettings {
    private static final String PREFS_FILE_NAME = "UserSettings";
    private final SharedPreferences sharedPref;

    public UserSettings(final Activity activity) {
        sharedPref = activity.getSharedPreferences(PREFS_FILE_NAME, Context.MODE_PRIVATE);
    }

    public boolean getBoolForKey(String key, boolean defaultValue) {
        try {
            return sharedPref.getBoolean(key, defaultValue);
        } catch (Exception ex) {
            ex.printStackTrace();
        }

        return defaultValue;
    }

    public int getIntegerForKey(String key, int defaultValue) {
        try {
            return sharedPref.getInt(key, defaultValue);
        } catch (Exception ex) {
            ex.printStackTrace();
        }

        return defaultValue;
    }

    public long getLongForKey(String key, long defaultValue) {
        try {
            return sharedPref.getLong(key, defaultValue);
        } catch (Exception ex) {
            ex.printStackTrace();
        }

        return defaultValue;
    }

    public float getFloatForKey(String key, float defaultValue) {
        try {
            return sharedPref.getFloat(key, defaultValue);
        } catch (Exception ex) {
            ex.printStackTrace();
        }

        return defaultValue;
    }

    public double getDoubleForKey(String key, double defaultValue) {
        // SharedPreferences doesn't support double, using long to store
        try {
            return Double.longBitsToDouble(sharedPref.getLong(key, Double.doubleToRawLongBits(defaultValue)));
        } catch (Exception ex) {
            ex.printStackTrace();
        }

        return defaultValue;
    }

    public String getStringForKey(String key, String defaultValue) {
        try {
            return sharedPref.getString(key, defaultValue);
        } catch (Exception ex) {
            ex.printStackTrace();
        }

        return defaultValue;
    }

    public void setBoolForKey(String key, boolean value) {
        sharedPref.edit().putBoolean(key, value).apply();
    }

    public void setIntegerForKey(String key, int value) {
        sharedPref.edit().putInt(key, value).apply();
    }

    public void setLongForKey(String key, long value) {
        sharedPref.edit().putLong(key, value).apply();
    }

    public void setFloatForKey(String key, float value) {
        sharedPref.edit().putFloat(key, value).apply();
    }

    public void setDoubleForKey(String key, double value) {
        // SharedPreferences doesn't support double, using long to store
        sharedPref.edit().putLong(key, Double.doubleToRawLongBits(value)).apply();
    }

    public void setStringForKey(String key, String value) {
        sharedPref.edit().putString(key, value).apply();
    }

    public void removeKey(String key) {
        sharedPref.edit().remove(key).apply();
    }
}
