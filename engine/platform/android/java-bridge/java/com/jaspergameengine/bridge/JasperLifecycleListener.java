package com.jaspergameengine.bridge;

import android.content.Intent;
import android.content.res.Configuration;
import android.os.Bundle;

// The game activity's lifecycle, for plugin code (SDKs that need onResume, activity results,
// permission results...): JasperNative.addLifecycleListener(listener). Every method is called on
// the UI thread, after the activity's own handling; all are optional.
public interface JasperLifecycleListener {
    // once, right after the plugins' onJasperInit (a listener added there gets it)
    default void onCreate(Bundle savedInstanceState) {}
    default void onStart() {}
    default void onResume() {}
    default void onPause() {}
    default void onStop() {}
    default void onDestroy() {}
    default void onActivityResult(int requestCode, int resultCode, Intent data) {}
    default void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {}
    default void onNewIntent(Intent intent) {}
    default void onWindowFocusChanged(boolean hasFocus) {}
    default void onConfigurationChanged(Configuration newConfig) {}
    // true: consumed (an ad closing itself); the game does not get Escape
    default boolean onBackPressed() { return false; }
}
