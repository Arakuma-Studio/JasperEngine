package com.jaspergameengine.jasperengine;

import android.app.Activity;
import android.app.ActivityManager;
import android.content.ActivityNotFoundException;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.hardware.display.DisplayManager;
import android.net.ConnectivityManager;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.NetworkInfo;
import android.net.Uri;
import android.os.Build;
import android.os.Handler;
import android.os.LocaleList;
import android.os.Looper;
import android.os.PowerManager;
import android.os.VibrationEffect;
import android.os.Vibrator;
import android.os.VibratorManager;
import android.util.DisplayMetrics;
import android.util.Log;
import android.graphics.Rect;
import android.view.Display;
import android.view.DisplayCutout;
import android.view.HapticFeedbackConstants;
import android.view.OrientationEventListener;
import android.view.Surface;
import android.view.View;
import android.view.ViewTreeObserver;
import android.view.WindowInsets;

import java.util.ArrayList;
import java.util.Locale;

// Platform services of the engine (System.openURL, the locale, app and device information,
// vibration, Screen.orientation, Input.deviceOrientation), called from the native game thread
// through JNI (JasperEngineAndroid.cpp). Static, so MainActivity stays small: it calls
// onCreate/onResume/onPause/onDestroy for the listeners.
public final class JasperPlatform {

	private static final String TAG = "JasperEngine";

	// Display.getRotation() in degrees, kept by a DisplayListener (a half turn changes no
	// configuration and no surface size, so the game thread reads this every frame)
	public static volatile int displayRotation = 0;
	// OrientationEventListener degrees (0..359), -1 when the device lies flat or nothing reports
	public static volatile int deviceDegrees = -1;

	// Screen.safeArea (safe-area spec 6.5): the native side reads GameActivity's insets itself and
	// this for the rest, kept on the UI thread by a layout listener: where the game's view lies in
	// the window (insets are the window's) and the display cutout's rectangles (API 28+), as
	// {left, top, right, bottom distances of the view from the window's edges, rectangle count,
	// then left, top, right, bottom of each rectangle in window pixels}. The revision goes up
	// with every change, so the game thread can compare one int a frame.
	public static volatile int safeAreaRevision = 0;
	public static volatile int[] safeAreaInfo = new int[]{0, 0, 0, 0, 0};

	private static View.OnLayoutChangeListener safeAreaListener;
	// every layout pass of the window (a half turn re-lays the window out with the cutout on the
	// other side but leaves the content's bounds as they were)
	private static ViewTreeObserver.OnGlobalLayoutListener safeAreaGlobalListener;
	private static OrientationEventListener orientationListener;
	private static DisplayManager.DisplayListener displayListener;
	private static boolean warnedNetworkPermission = false;

	// ---- thermal and memory state (Device.thermalState, Device.onLowMemory; spec S56)

	// PowerManager's thermal status as Device's ThermalState (0 Nominal, 1 Fair, 2 Serious,
	// 3 Critical): kept by a listener from API 29, read by the game thread every frame
	public static volatile int thermalState = 0;
	// onTrimMemory calls at TRIM_MEMORY_RUNNING_LOW and above (MainActivity), counted for the
	// game thread, which handles each new one as a low-memory warning
	public static volatile int trimMemoryCount = 0;
	// a PowerManager.OnThermalStatusChangedListener (API 29), registered while the activity lives
	private static Object thermalListener;

	private JasperPlatform() {
	}

	// THERMAL_STATUS_NONE, LIGHT, MODERATE, then SEVERE, CRITICAL, EMERGENCY and SHUTDOWN
	static int thermalStateOf(int status) {
		if (status <= 0) return 0;
		if (status == 1) return 1;
		if (status == 2) return 2;
		return 3;
	}

	// The API 29+ calls, only reached behind an SDK_INT check
	@android.annotation.TargetApi(29)
	private static final class Thermal {
		static void register(Activity activity) {
			PowerManager power = (PowerManager) activity.getSystemService(Context.POWER_SERVICE);
			if (power == null) return;
			thermalState = thermalStateOf(power.getCurrentThermalStatus());
			PowerManager.OnThermalStatusChangedListener listener = new PowerManager.OnThermalStatusChangedListener() {
				@Override
				public void onThermalStatusChanged(int status) {
					thermalState = thermalStateOf(status);
				}
			};
			power.addThermalStatusListener(activity.getMainExecutor(), listener);
			thermalListener = listener;
		}

		static void unregister(Activity activity) {
			PowerManager power = (PowerManager) activity.getSystemService(Context.POWER_SERVICE);
			if (power != null && thermalListener != null) {
				power.removeThermalStatusListener((PowerManager.OnThermalStatusChangedListener) thermalListener);
			}
			thermalListener = null;
		}

		// the status now and, from API 30, the forecast of 10 s ahead (1 is where SEVERE
		// throttling starts): the game lowers its cost before the OS throttles it
		@android.annotation.TargetApi(30)
		static int poll(Activity activity) {
			PowerManager power = (PowerManager) activity.getSystemService(Context.POWER_SERVICE);
			if (power == null) return thermalState;
			int state = thermalStateOf(power.getCurrentThermalStatus());
			if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
				float headroom = power.getThermalHeadroom(10);
				if (!Float.isNaN(headroom)) {
					if (headroom >= 1.0f) state = Math.max(state, 2);
					else if (headroom >= 0.85f) state = Math.max(state, 1);
				}
			}
			return state;
		}
	}

	// Polled by the game thread every 2 s (getThermalHeadroom answers once a second at most): the
	// fallback where the listener's status alone is all there is (API 29), the forecast from API
	// 30. Nominal below API 29, which reports nothing.
	public static int getThermalState(Activity activity) {
		if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) return 0;
		try {
			thermalState = Thermal.poll(activity);
		} catch (RuntimeException e) {
			Log.w(TAG, "thermal state: " + e);
		}
		return thermalState;
	}

	// ---- lifecycle (MainActivity)

	public static void onCreate(final Activity activity) {
		updateDisplayRotation(activity);
		if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
			try {
				Thermal.register(activity);
			} catch (RuntimeException e) {
				Log.w(TAG, "thermal listener: " + e);
			}
		}
		final View content = activity.findViewById(android.R.id.content);
		if (content != null) {
			safeAreaListener = new View.OnLayoutChangeListener() {
				@Override
				public void onLayoutChange(View v, int left, int top, int right, int bottom,
				                           int oldLeft, int oldTop, int oldRight, int oldBottom) {
					updateSafeAreaInfo(activity);
				}
			};
			content.addOnLayoutChangeListener(safeAreaListener);
			safeAreaGlobalListener = new ViewTreeObserver.OnGlobalLayoutListener() {
				@Override
				public void onGlobalLayout() {
					updateSafeAreaInfo(activity);
				}
			};
			activity.getWindow().getDecorView().getViewTreeObserver().addOnGlobalLayoutListener(safeAreaGlobalListener);
		}
		orientationListener = new OrientationEventListener(activity) {
			@Override
			public void onOrientationChanged(int degrees) {
				deviceDegrees = degrees == OrientationEventListener.ORIENTATION_UNKNOWN ? -1 : degrees;
			}
		};
		DisplayManager displays = (DisplayManager) activity.getSystemService(Context.DISPLAY_SERVICE);
		if (displays != null) {
			displayListener = new DisplayManager.DisplayListener() {
				@Override public void onDisplayAdded(int displayId) {}
				@Override public void onDisplayRemoved(int displayId) {}
				@Override public void onDisplayChanged(int displayId) {
					updateDisplayRotation(activity);
					// the cutout moves with a rotation: read again once the window has its new insets
					View decor = activity.getWindow().getDecorView();
					if (decor != null) {
						decor.post(new Runnable() {
							@Override public void run() {
								updateSafeAreaInfo(activity);
							}
						});
					}
				}
			};
			displays.registerDisplayListener(displayListener, new Handler(Looper.getMainLooper()));
		}
	}

	public static void onResume(Activity activity) {
		updateDisplayRotation(activity);
		if (orientationListener != null && orientationListener.canDetectOrientation()) {
			orientationListener.enable();
		}
	}

	public static void onPause(Activity activity) {
		if (orientationListener != null) {
			orientationListener.disable();
		}
		deviceDegrees = -1;
	}

	public static void onDestroy(Activity activity) {
		if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) Thermal.unregister(activity);
		View content = activity.findViewById(android.R.id.content);
		if (content != null && safeAreaListener != null) {
			content.removeOnLayoutChangeListener(safeAreaListener);
		}
		safeAreaListener = null;
		if (safeAreaGlobalListener != null) {
			ViewTreeObserver observer = activity.getWindow().getDecorView().getViewTreeObserver();
			if (observer.isAlive()) observer.removeOnGlobalLayoutListener(safeAreaGlobalListener);
		}
		safeAreaGlobalListener = null;
		if (orientationListener != null) {
			orientationListener.disable();
			orientationListener = null;
		}
		DisplayManager displays = (DisplayManager) activity.getSystemService(Context.DISPLAY_SERVICE);
		if (displays != null && displayListener != null) {
			displays.unregisterDisplayListener(displayListener);
		}
		displayListener = null;
	}

	// UI thread (the layout listener): see safeAreaInfo
	private static void updateSafeAreaInfo(Activity activity) {
		try {
			View decor = activity.getWindow().getDecorView();
			View content = activity.findViewById(android.R.id.content);
			if (decor == null || content == null) return;
			int[] location = new int[2];
			content.getLocationInWindow(location);
			ArrayList<Integer> values = new ArrayList<>();
			values.add(Math.max(0, location[0]));
			values.add(Math.max(0, location[1]));
			values.add(Math.max(0, decor.getWidth() - location[0] - content.getWidth()));
			values.add(Math.max(0, decor.getHeight() - location[1] - content.getHeight()));
			ArrayList<Rect> rects = new ArrayList<>();
			if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
				WindowInsets insets = decor.getRootWindowInsets();
				DisplayCutout cutout = insets != null ? insets.getDisplayCutout() : null;
				if (cutout != null) rects.addAll(cutout.getBoundingRects());
			}
			values.add(rects.size());
			for (Rect rect : rects) {
				values.add(rect.left);
				values.add(rect.top);
				values.add(rect.right);
				values.add(rect.bottom);
			}
			int[] info = new int[values.size()];
			for (int i = 0; i < info.length; i++) info[i] = values.get(i);
			if (!java.util.Arrays.equals(info, safeAreaInfo)) {
				safeAreaInfo = info;
				safeAreaRevision++;
			}
		} catch (RuntimeException e) {
			Log.w(TAG, "Screen.safeArea: could not read the window's layout", e);
		}
	}

	@SuppressWarnings("deprecation")
	private static Display display(Activity activity) {
		if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
			Display display = activity.getDisplay();
			if (display != null) return display;
		}
		return activity.getWindowManager().getDefaultDisplay();
	}

	private static void updateDisplayRotation(Activity activity) {
		try {
			switch (display(activity).getRotation()) {
				case Surface.ROTATION_90: displayRotation = 90; break;
				case Surface.ROTATION_180: displayRotation = 180; break;
				case Surface.ROTATION_270: displayRotation = 270; break;
				default: displayRotation = 0; break;
			}
		} catch (RuntimeException e) {
			Log.e(TAG, "Failed to read the display rotation", e);
		}
	}

	// ---- URLs

	// System.openURL: an ACTION_VIEW intent for any app that handles the URL (browser, store,
	// mail); false when none does
	public static boolean openURL(Activity activity, String url) {
		try {
			Intent intent = new Intent(Intent.ACTION_VIEW, Uri.parse(url));
			intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
			activity.startActivity(intent);
			return true;
		} catch (ActivityNotFoundException e) {
			Log.w(TAG, "No app opens " + url);
			return false;
		} catch (RuntimeException e) {
			Log.e(TAG, "Failed to open " + url, e);
			return false;
		}
	}

	// ---- locale

	// BCP 47 tags, most preferred first
	public static String[] getPreferredLocales() {
		ArrayList<String> tags = new ArrayList<>();
		if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) {
			LocaleList list = LocaleList.getDefault();
			for (int i = 0; i < list.size(); i++) {
				tags.add(list.get(i).toLanguageTag());
			}
		}
		if (tags.isEmpty()) {
			tags.add(Locale.getDefault().toLanguageTag());
		}
		return tags.toArray(new String[0]);
	}

	// the country of the default locale (the user's region), "" when it has none
	public static String getRegion() {
		String country = Locale.getDefault().getCountry();
		return country != null ? country : "";
	}

	// ---- application

	@SuppressWarnings("deprecation")
	private static PackageInfo packageInfo(Activity activity) {
		try {
			PackageManager manager = activity.getPackageManager();
			if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
				return manager.getPackageInfo(activity.getPackageName(), PackageManager.PackageInfoFlags.of(0));
			}
			return manager.getPackageInfo(activity.getPackageName(), 0);
		} catch (PackageManager.NameNotFoundException e) {
			return null;
		}
	}

	public static String getAppVersion(Activity activity) {
		PackageInfo info = packageInfo(activity);
		return (info != null && info.versionName != null) ? info.versionName : "";
	}

	@SuppressWarnings("deprecation")
	public static long getAppBuild(Activity activity) {
		PackageInfo info = packageInfo(activity);
		if (info == null) return 0;
		if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) return info.getLongVersionCode();
		return info.versionCode;
	}

	public static String getAppIdentifier(Activity activity) {
		return activity.getPackageName();
	}

	public static String getAppName(Activity activity) {
		ApplicationInfo info = activity.getApplicationInfo();
		CharSequence label = activity.getPackageManager().getApplicationLabel(info);
		return label != null ? label.toString() : "";
	}

	// ---- device

	public static String getOsRelease() {
		return Build.VERSION.RELEASE != null ? Build.VERSION.RELEASE : "";
	}

	public static int getApiLevel() {
		return Build.VERSION.SDK_INT;
	}

	public static String getBuildDisplay() {
		return Build.DISPLAY != null ? Build.DISPLAY : "";
	}

	public static String getBuildIncremental() {
		return Build.VERSION.INCREMENTAL != null ? Build.VERSION.INCREMENTAL : "";
	}

	public static String getDeviceModel() {
		String manufacturer = Build.MANUFACTURER != null ? Build.MANUFACTURER : "";
		String model = Build.MODEL != null ? Build.MODEL : "";
		return (manufacturer + " " + model).trim();
	}

	// MiB
	public static int getSystemMemorySize(Activity activity) {
		ActivityManager manager = (ActivityManager) activity.getSystemService(Context.ACTIVITY_SERVICE);
		if (manager == null) return 0;
		ActivityManager.MemoryInfo info = new ActivityManager.MemoryInfo();
		manager.getMemoryInfo(info);
		return (int) (info.totalMem / (1024L * 1024L));
	}

	// NetworkReachability: 0 not reachable, 1 carrier data, 2 local area network (Wi-Fi,
	// Ethernet). Needs ACCESS_NETWORK_STATE (the export adds it when a script asks).
	@SuppressWarnings("deprecation")
	public static int getInternetReachability(Activity activity) {
		try {
			ConnectivityManager manager = (ConnectivityManager) activity.getSystemService(Context.CONNECTIVITY_SERVICE);
			if (manager == null) return 0;
			if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
				Network network = manager.getActiveNetwork();
				if (network == null) return 0;
				NetworkCapabilities capabilities = manager.getNetworkCapabilities(network);
				if (capabilities == null) return 0;
				if (capabilities.hasTransport(NetworkCapabilities.TRANSPORT_CELLULAR)) return 1;
				return 2;
			}
			NetworkInfo info = manager.getActiveNetworkInfo();
			if (info == null || !info.isConnected()) return 0;
			return info.getType() == ConnectivityManager.TYPE_MOBILE ? 1 : 2;
		} catch (SecurityException e) {
			if (!warnedNetworkPermission) {
				warnedNetworkPermission = true;
				Log.w(TAG, "System.getInternetReachability needs the ACCESS_NETWORK_STATE permission");
			}
			return 0;
		}
	}

	// Handheld.Vibrate; false without a vibrator or the VIBRATE permission
	@SuppressWarnings("deprecation")
	public static boolean vibrate(Activity activity, int milliseconds) {
		try {
			Vibrator vibrator;
			if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
				VibratorManager manager = (VibratorManager) activity.getSystemService(Context.VIBRATOR_MANAGER_SERVICE);
				vibrator = manager != null ? manager.getDefaultVibrator() : null;
			} else {
				vibrator = (Vibrator) activity.getSystemService(Context.VIBRATOR_SERVICE);
			}
			if (vibrator == null || !vibrator.hasVibrator()) return false;
			long length = Math.max(1, milliseconds);
			if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
				vibrator.vibrate(VibrationEffect.createOneShot(length, VibrationEffect.DEFAULT_AMPLITUDE));
			} else {
				vibrator.vibrate(length);
			}
			return true;
		} catch (SecurityException e) {
			Log.w(TAG, "System.vibrate needs the VIBRATE permission");
			return false;
		}
	}

	// HapticFeedback: Light, Medium, Heavy, Selection, Success, Warning, Error
	public static boolean hapticFeedback(final Activity activity, int type) {
		final int constant;
		switch (type) {
			case 0: case 3: constant = HapticFeedbackConstants.CLOCK_TICK; break;
			case 1: constant = HapticFeedbackConstants.VIRTUAL_KEY; break;
			case 4: constant = Build.VERSION.SDK_INT >= Build.VERSION_CODES.R
					? HapticFeedbackConstants.CONFIRM : HapticFeedbackConstants.VIRTUAL_KEY; break;
			case 5: case 6: constant = Build.VERSION.SDK_INT >= Build.VERSION_CODES.R
					? HapticFeedbackConstants.REJECT : HapticFeedbackConstants.LONG_PRESS; break;
			default: constant = HapticFeedbackConstants.LONG_PRESS; break;
		}
		final View view = activity.getWindow() != null ? activity.getWindow().getDecorView() : null;
		if (view == null) return false;
		activity.runOnUiThread(new Runnable() {
			@Override public void run() {
				view.performHapticFeedback(constant);
			}
		});
		return true;
	}

	// ---- display

	public static int getScreenDpi(Activity activity) {
		DisplayMetrics metrics = activity.getResources().getDisplayMetrics();
		return metrics.densityDpi;
	}

	public static float getRefreshRate(Activity activity) {
		try {
			return display(activity).getRefreshRate();
		} catch (RuntimeException e) {
			return 60.0f;
		}
	}

	// whether the device's rotation 0 is a portrait one (phones) rather than a landscape one
	// (most tablets)
	@SuppressWarnings("deprecation")
	public static boolean isNaturalPortrait(Activity activity) {
		try {
			Display display = display(activity);
			DisplayMetrics metrics = new DisplayMetrics();
			display.getRealMetrics(metrics);
			int rotation = display.getRotation();
			boolean quarter = rotation == Surface.ROTATION_90 || rotation == Surface.ROTATION_270;
			boolean tall = metrics.heightPixels >= metrics.widthPixels;
			return quarter ? !tall : tall;
		} catch (RuntimeException e) {
			return true;
		}
	}

	// Screen.orientation / autorotateTo*: an ActivityInfo.SCREEN_ORIENTATION_* constant
	public static void setOrientationPolicy(final Activity activity, final int orientation) {
		activity.runOnUiThread(new Runnable() {
			@Override public void run() {
				try {
					if (activity.getRequestedOrientation() != orientation) {
						activity.setRequestedOrientation(orientation);
					}
				} catch (RuntimeException e) {
					// Android 8.0 refuses fixed orientations for translucent activities
					Log.e(TAG, "Failed to set the screen orientation", e);
				}
			}
		});
	}
}
