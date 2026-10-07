package com.jaspergameengine.jasperengine;

import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.os.Build.VERSION;
import android.os.Build.VERSION_CODES;
import android.os.Bundle;
import android.util.Log;
import android.view.View;
import android.view.WindowManager.LayoutParams;
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
	private AdMobWrapper admobWrapper;
	private ClipboardManager clipboard;

	private void hideSystemUI() {
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

	@Override
	protected void onCreate(Bundle savedInstanceState) {
		// When true, the app will fit inside any system UI windows.
		// When false, we render behind any system UI windows.
		WindowCompat.setDecorFitsSystemWindows(getWindow(), false);
		hideSystemUI();

		userSettings = new UserSettings(this);
		admobWrapper = new AdMobWrapper(this);
		clipboard = (ClipboardManager) getSystemService(Context.CLIPBOARD_SERVICE);

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

	public AdMobWrapper getAdMobWrapper() {
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
