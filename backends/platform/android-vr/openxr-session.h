/* ScummVM - Graphic Adventure Engine
 *
 * ScummVM is the legal property of its developers, whose names
 * are too numerous to list here. Please refer to the COPYRIGHT
 * file distributed with this source distribution.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#ifndef BACKENDS_PLATFORM_ANDROID_VR_OPENXR_SESSION_H
#define BACKENDS_PLATFORM_ANDROID_VR_OPENXR_SESSION_H

#ifdef USE_OPENXR

#include "common/scummsys.h"

// Forward declare types to avoid pulling in OpenXR/EGL/GL headers
// (which conflict with ScummVM's forbidden.h when included transitively).
// The full headers are only included in the .cpp file.
typedef uint32_t GLuint;
typedef void *EGLDisplay;
typedef void *EGLConfig;
typedef void *EGLContext;

struct _JavaVM;
typedef _JavaVM JavaVM;
class _jobject;
typedef _jobject *jobject;

// Forward declare the internal implementation
struct OpenXRSessionImpl;

class OpenXRSession {
public:
	struct FrameState {
		bool shouldRender;
		int64_t predictedDisplayTime;
		// Eye poses stored as raw floats to avoid OpenXR header dependency
		// [eye][0-3] = orientation quat (x,y,z,w), [eye][4-6] = position (x,y,z)
		float eyePose[2][7];
		// FOV angles in radians: [eye][0-3] = left, right, up, down
		float eyeFov[2][4];
	};

	OpenXRSession();
	~OpenXRSession();

	bool init(JavaVM *vm, jobject activity, EGLDisplay eglDisplay, EGLConfig eglConfig, EGLContext eglContext);
	void shutdown();

	bool beginFrame(FrameState &state);
	GLuint acquireSwapchainImage(int eye);
	void releaseSwapchainImage(int eye);
	void endFrame(const FrameState &state);

	bool isSessionRunning() const;
	void handleSessionStateChanges();

	uint32_t getSwapchainWidth() const;
	uint32_t getSwapchainHeight() const;

	// These return opaque handles for use by OpenXRInput
	void *getInstanceHandle() const;
	void *getSessionHandle() const;
	void *getLocalSpaceHandle() const;

private:
	OpenXRSessionImpl *_impl;
};

#endif // USE_OPENXR
#endif // BACKENDS_PLATFORM_ANDROID_VR_OPENXR_SESSION_H
