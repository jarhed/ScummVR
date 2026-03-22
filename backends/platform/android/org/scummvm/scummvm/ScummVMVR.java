package org.scummvm.scummvm;

import android.app.Activity;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.res.AssetManager;
import android.net.ConnectivityManager;
import android.net.NetworkInfo;
import android.util.Log;
import android.view.SurfaceHolder;
import android.widget.Toast;

import javax.microedition.khronos.egl.EGL10;
import javax.microedition.khronos.egl.EGLConfig;
import javax.microedition.khronos.egl.EGLContext;
import javax.microedition.khronos.egl.EGLDisplay;
import javax.microedition.khronos.egl.EGLSurface;

import java.io.File;

/**
 * VR variant of the ScummVM native bridge.
 * Uses the parent ScummVM class but the VR Activity manages the lifecycle
 * differently (PBuffer EGL, no SurfaceView).
 */
public class ScummVMVR extends ScummVM {
	private static final String LOG_TAG = "ScummVM_VR";

	private final Activity _activity;

	public ScummVMVR(Activity activity, AssetManager assetManager,
	                 MyScummVMDestroyedCallback callback) {
		// Pass null SurfaceHolder - VR mode doesn't use window surfaces
		super(assetManager, null, callback);
		_activity = activity;
	}

	// The parent's run() waits for surfaceChanged which will never come in VR mode.
	// Instead, the VR Activity will call setAssetsUpdated/setArgs from the parent,
	// and the parent's run() lifecycle needs to be adapted.
	// For now, we signal the surface semaphore by calling surfaceChanged directly.

	// Abstract method implementations required by ScummVM base class
	@Override
	protected void getDPI(float[] values) {
		values[0] = 460.0f;
		values[1] = 460.0f;
		values[2] = 2.0f;
	}

	@Override
	protected void displayMessageOnOSD(final String msg) {
		_activity.runOnUiThread(() -> {
			Toast.makeText(_activity, msg, Toast.LENGTH_SHORT).show();
		});
	}

	@Override
	protected void openUrl(String url) {
	}

	@Override
	protected boolean hasTextInClipboard() {
		ClipboardManager cm = (ClipboardManager) _activity.getSystemService(Context.CLIPBOARD_SERVICE);
		return cm != null && cm.hasPrimaryClip();
	}

	@Override
	protected String getTextFromClipboard() {
		ClipboardManager cm = (ClipboardManager) _activity.getSystemService(Context.CLIPBOARD_SERVICE);
		if (cm != null && cm.hasPrimaryClip()) {
			ClipData clip = cm.getPrimaryClip();
			if (clip != null && clip.getItemCount() > 0) {
				CharSequence text = clip.getItemAt(0).getText();
				return text != null ? text.toString() : "";
			}
		}
		return "";
	}

	@Override
	protected boolean setTextInClipboard(String text) {
		ClipboardManager cm = (ClipboardManager) _activity.getSystemService(Context.CLIPBOARD_SERVICE);
		if (cm != null) {
			cm.setPrimaryClip(ClipData.newPlainText("ScummVM", text));
			return true;
		}
		return false;
	}

	@Override
	protected boolean isConnectionLimited() {
		ConnectivityManager cm = (ConnectivityManager) _activity.getSystemService(Context.CONNECTIVITY_SERVICE);
		if (cm != null) {
			NetworkInfo info = cm.getActiveNetworkInfo();
			return info != null && info.isConnected() &&
				(info.getType() == ConnectivityManager.TYPE_MOBILE);
		}
		return false;
	}

	@Override
	protected void setWindowCaption(String caption) {
	}

	@Override
	protected void showVirtualKeyboard(boolean enable) {
	}

	@Override
	protected void showOnScreenControls(int enableMask) {
	}

	@Override
	protected void setTouchMode(int touchMode) {
	}

	@Override
	protected int getTouchMode() {
		return 1;
	}

	@Override
	protected void setOrientation(int orientation) {
	}

	@Override
	protected String getScummVMBasePath() {
		return _activity.getFilesDir().getAbsolutePath();
	}

	@Override
	protected String getScummVMConfigPath() {
		return new File(_activity.getFilesDir(), "scummvm.ini").getAbsolutePath();
	}

	@Override
	protected String getScummVMLogPath() {
		return new File(_activity.getFilesDir(), "scummvm.log").getAbsolutePath();
	}

	@Override
	protected void setCurrentGame(String target) {
	}

	@Override
	protected String[] getSysArchives() {
		// Assets are accessed via AssetManager (AssetArchive), not filesystem paths.
		// Return empty — addSysArchivesToSearchSet handles the asset archive separately.
		return new String[0];
	}

	@Override
	protected String[] getAllStorageLocations() {
		// Storage locations are returned as pairs: [label, path, label, path, ...]
		return new String[]{ "Internal", _activity.getFilesDir().getAbsolutePath() };
	}

	@Override
	protected String[] getAllStorageLocationsNoPermissionRequest() {
		return getAllStorageLocations();
	}

	@Override
	protected SAFFSTree getNewSAFTree(boolean write, String initialURI, String prompt) {
		return null;
	}

	@Override
	protected SAFFSTree[] getSAFTrees() {
		return new SAFFSTree[0];
	}

	@Override
	protected SAFFSTree findSAFTree(String name) {
		return null;
	}

	@Override
	protected int exportBackup(String prompt) {
		return -1;
	}

	@Override
	protected int importBackup(String prompt, String path) {
		return -1;
	}
}
