package com.jaspergameengine.jasperengine;

import android.app.Activity;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.os.Build.VERSION;
import android.os.Build.VERSION_CODES;
import android.os.Bundle;
import android.util.Log;
import android.view.View;
import android.view.WindowManager.LayoutParams;
import androidx.activity.OnBackPressedCallback;
import androidx.core.view.WindowCompat;
import androidx.core.view.WindowInsetsCompat;
import androidx.core.view.WindowInsetsControllerCompat;

import com.google.androidgamesdk.GameActivity;

public class MainActivity extends GameActivity {

	// Load our native library:
	static {
		System.loadLibrary("jasperengine-android");
	}

	private UserSettings userSettings;
	// Null when the export leaves AdMob out (its wrapper source is compiled only with it)
	private Object admobWrapper;
	private ClipboardManager clipboard;

	private native void onBackPressedNative();

	// A prebuilt player's Project Settings (docs/specs/S30-shipping.md R13): <meta-data> of the
	// activity its package writes, since its code is not generated per game. A Compile build has
	// none and keeps the defaults (its source is patched instead).
	private boolean metaFlag(String name, boolean fallback) {
		try {
			android.content.pm.ActivityInfo info = getPackageManager().getActivityInfo(getComponentName(),
					android.content.pm.PackageManager.GET_META_DATA);
			if (info.metaData != null && info.metaData.containsKey(name)) return info.metaData.getBoolean(name, fallback);
		} catch (android.content.pm.PackageManager.NameNotFoundException e) {
			Log.w("JasperEngine", "no activity info: " + e);
		}
		return fallback;
	}

	private void hideSystemUI() {
		if (!metaFlag("jasperengine.fullscreen", true)) {
			// a windowed player fits inside the system bars
			WindowCompat.setDecorFitsSystemWindows(getWindow(), true);
			return;
		}
		// This will put the game behind any cutouts and waterfalls on devices which have
		// them, so the corresponding insets will be non-zero.
		if (VERSION.SDK_INT >= VERSION_CODES.P) {
			getWindow().getAttributes().layoutInDisplayCutoutMode
					= LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS;
		}
		// From API 30 onwards, this is the recommended way to hide the system UI, rather than
		// using View.setSystemUiVisibility.
		View decorView = getWindow().getDecorView();
		WindowInsetsControllerCompat controller = new WindowInsetsControllerCompat(getWindow(),
				decorView);
		controller.hide(WindowInsetsCompat.Type.systemBars());
		controller.hide(WindowInsetsCompat.Type.displayCutout());
		controller.setSystemBarsBehavior(
				WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
	}

	// The wrapper sources are compiled only when the export enables their service
	private Object createOptionalWrapper(String className) {
		try {
			return Class.forName(className).getConstructor(Activity.class).newInstance(this);
		} catch (ReflectiveOperationException | LinkageError e) {
			return null;
		}
	}

	@Override
	protected void onCreate(Bundle savedInstanceState) {
		if (metaFlag("jasperengine.keepScreenOn", false)) getWindow().addFlags(LayoutParams.FLAG_KEEP_SCREEN_ON);
		// When true, the app will fit inside any system UI windows.
		// When false, we render behind any system UI windows.
		WindowCompat.setDecorFitsSystemWindows(getWindow(), false);
		hideSystemUI();

		userSettings = new UserSettings(this);
		admobWrapper = createOptionalWrapper("com.jaspergameengine.jasperengine.AdMobWrapper");
		clipboard = (ClipboardManager) getSystemService(Context.CLIPBOARD_SERVICE);

		// Games get Back as Escape. A callback, since Android 16 no longer sends it as a key
		getOnBackPressedDispatcher().addCallback(this, new OnBackPressedCallback(true) {
			@Override
			public void handleOnBackPressed() {
				onBackPressedNative();
			}
		});

		super.onCreate(savedInstanceState);
	}

	@Override
	protected void onResume() {
		super.onResume();
		hideSystemUI();
	}

	public UserSettings getUserSettings() {
		return userSettings;
	}

	public Object getAdMobWrapper() {
		return admobWrapper;
	}

	// Called from the native game thread (HTML UI copy/cut). The clip is set on the UI thread.
	public void setClipboardText(String text) {
		final String clipText = (text != null) ? text : "";
		runOnUiThread(new Runnable() {
			@Override public void run() {
				if (clipboard == null) return;
				try {
					clipboard.setPrimaryClip(ClipData.newPlainText("text", clipText));
				} catch (RuntimeException e) {
					Log.e("JasperEngine", "Failed to set clipboard text", e);
				}
			}
		});
	}

	// Called from the native game thread (HTML UI paste). Since Android 10 only the focused
	// app can read the clipboard; otherwise there is no clip and this returns "".
	public String getClipboardText() {
		if (clipboard == null) return "";
		try {
			ClipData clip = clipboard.getPrimaryClip();
			if (clip == null || clip.getItemCount() == 0) return "";
			CharSequence text = clip.getItemAt(0).coerceToText(this);
			return (text != null) ? text.toString() : "";
		} catch (RuntimeException e) {
			Log.e("JasperEngine", "Failed to get clipboard text", e);
			return "";
		}
	}
}
