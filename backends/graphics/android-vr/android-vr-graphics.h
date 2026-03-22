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

#ifndef BACKENDS_GRAPHICS_ANDROID_VR_GRAPHICS_H
#define BACKENDS_GRAPHICS_ANDROID_VR_GRAPHICS_H

#ifdef USE_OPENXR

#include "backends/graphics/android/android-graphics.h"
#include "backends/platform/android-vr/openxr-session.h"
#include "backends/platform/android-vr/openxr-input.h"
#include "backends/platform/android-vr/vr-screen-geometry.h"

class AndroidVRGraphicsManager : public AndroidGraphicsManager {
public:
	AndroidVRGraphicsManager(OpenXRSession *xrSession);
	virtual ~AndroidVRGraphicsManager();

	void initSurface();
	void deinitSurface();
	void resizeSurface();
	void updateScreen() override;

	// Override touch control methods to be no-ops in VR mode
	void touchControlInitSurface(const Graphics::ManagedSurface &surf) override {}
	void touchControlNotifyChanged() override {}
	void touchControlDraw(uint8 alpha, int16 x, int16 y, int16 w, int16 h, const Common::Rect &clip) override {}

protected:
	bool loadVideoMode(uint requestedWidth, uint requestedHeight,
	                   bool resizable, int antialiasing) override;
	void refreshScreen() override;

private:
	void renderEye(int eye, const float *eyePose, const float *eyeFov);
	void drawVirtualScreen(const float *viewProjMatrix);
	void processVRInput();

	void initVRShaders();
	void destroyVRShaders();

	OpenXRSession *_xrSession;
	OpenXRInput *_xrInput;
	VRScreenGeometry _screenGeometry;

	GLuint _sceneTexture;
	GLuint _sceneFBO;
	GLuint _sceneDepthBuffer;
	uint _sceneWidth;
	uint _sceneHeight;

	GLuint _vrQuadProgram;
	GLuint _vrQuadVBO;
	GLint _vrUniformMVP;
	GLint _vrUniformTexture;

	int _vrMouseX;
	int _vrMouseY;
	bool _vrMouseValid;
};

#endif // USE_OPENXR
#endif // BACKENDS_GRAPHICS_ANDROID_VR_GRAPHICS_H
