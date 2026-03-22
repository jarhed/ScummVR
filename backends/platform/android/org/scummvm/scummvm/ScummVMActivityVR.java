package org.scummvm.scummvm;

import android.app.Activity;
import android.content.res.AssetManager;
import android.graphics.PixelFormat;
import android.os.Bundle;
import android.util.Log;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.KeyEvent;

import java.io.File;

/**
 * VR Activity for Meta Quest headsets.
 * Uses a small hidden SurfaceView to satisfy ScummVM's surface lifecycle,
 * while OpenXR manages the actual VR rendering.
 */
public class ScummVMActivityVR extends Activity {
	private static final String LOG_TAG = "ScummVM_VR";

	private ScummVMVR _scummvm;
	private Thread _scummvmThread;
	private SurfaceView _surfaceView;

	static {
		try {
			System.loadLibrary("openxr_loader");
			Log.i(LOG_TAG, "OpenXR loader loaded");
		} catch (UnsatisfiedLinkError e) {
			Log.e(LOG_TAG, "Failed to load OpenXR loader: " + e.getMessage());
		}

		try {
			System.loadLibrary("scummvm");
			Log.i(LOG_TAG, "ScummVM native library loaded");
		} catch (UnsatisfiedLinkError e) {
			Log.e(LOG_TAG, "Failed to load ScummVM: " + e.getMessage());
		}
	}

	@Override
	protected void onCreate(Bundle savedInstanceState) {
		super.onCreate(savedInstanceState);

		Log.i(LOG_TAG, "ScummVMActivityVR onCreate");

		// Create a 1x1 SurfaceView to satisfy ScummVM's surface lifecycle
		_surfaceView = new SurfaceView(this);
		setContentView(_surfaceView);

		SurfaceHolder holder = _surfaceView.getHolder();
		holder.setFormat(PixelFormat.RGBA_8888);

		AssetManager assetManager = getAssets();

		_scummvm = new ScummVMVR(this, assetManager, new MyScummVMDestroyedCallback() {
			@Override
			public void handle(int res) {
				Log.i(LOG_TAG, "ScummVM destroyed with result: " + res);
				runOnUiThread(() -> finish());
			}
		});

		_scummvm.setAssetsUpdated(true);

		String[] args = buildArgs();
		_scummvm.setArgs(args);

		// In VR mode, the Quest compositor doesn't render Android SurfaceViews,
		// so surfaceChanged may never fire naturally. We add a callback to trigger
		// it, and also post a fallback to manually fire it after a short delay.
		holder.addCallback(new SurfaceHolder.Callback() {
			@Override
			public void surfaceCreated(SurfaceHolder h) {
				Log.i(LOG_TAG, "VR SurfaceView surfaceCreated");
			}

			@Override
			public void surfaceChanged(SurfaceHolder h, int format, int width, int height) {
				Log.i(LOG_TAG, "VR SurfaceView surfaceChanged: " + width + "x" + height);
			}

			@Override
			public void surfaceDestroyed(SurfaceHolder h) {
				Log.i(LOG_TAG, "VR SurfaceView surfaceDestroyed");
			}
		});

		// Start ScummVM thread
		_scummvmThread = new Thread(_scummvm, "ScummVM_VR_Main");
		_scummvmThread.start();

		// Fallback: manually trigger surfaceChanged after a short delay
		// in case the SurfaceView doesn't get composited in VR mode
		_surfaceView.postDelayed(() -> {
			Log.i(LOG_TAG, "Forcing surfaceChanged for VR mode");
			_scummvm.surfaceChanged(holder, PixelFormat.RGBA_8888, 1920, 1080);
		}, 500);
	}

	@Override
	protected void onDestroy() {
		Log.i(LOG_TAG, "ScummVMActivityVR onDestroy");
		super.onDestroy();
	}

	@Override
	protected void onPause() {
		super.onPause();
		// Do NOT pause ScummVM in VR mode — we need to keep processing
		// OpenXR events for session state transitions (IDLE → READY → FOCUSED).
		// The OpenXR session state handles VR-specific pause/resume.
	}

	@Override
	protected void onResume() {
		super.onResume();
	}

	@Override
	public boolean onKeyDown(int keyCode, KeyEvent event) {
		if (_scummvm != null) {
			_scummvm.pushEvent(1 /* JE_KEY */, event.getAction(),
				event.getKeyCode(), event.getUnicodeChar(),
				event.getMetaState(), event.getRepeatCount(), 0);
			return true;
		}
		return super.onKeyDown(keyCode, event);
	}

	private String[] buildArgs() {
		File dataDir = getFilesDir();
		File configFile = new File(dataDir, "scummvm.ini");
		File saveDir = new File(dataDir, "saves");
		if (!saveDir.exists()) {
			saveDir.mkdirs();
		}

		return new String[]{
			"ScummVM",
			"--config=" + configFile.getAbsolutePath(),
			"--savepath=" + saveDir.getAbsolutePath()
		};
	}
}
