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

#define FORBIDDEN_SYMBOL_ALLOW_ALL
#include "common/scummsys.h"

#ifdef USE_OPENXR

#include <jni.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <android/log.h>
#include <android/native_activity.h>
#include <android_native_app_glue.h>
#include <cstring>
#include <cmath>
#include <pthread.h>
#include <unistd.h>

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

// ScummVM headers for event pushing
#include "common/system.h"
#include "common/events.h"
#include "backends/platform/android/android.h"

// Diorama mode
#include "backends/platform/android-vr/diorama-data.h"
extern void dioramaRendererInit();
extern void dioramaRendererDraw(const float *viewProj);
extern bool dioramaHasData();

#define LOG_TAG "ScummVM_VR_Main"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#define MAX_VIEWS 4
#define MAX_SWAPCHAIN_LENGTH 4

// Virtual screen quad shader
static const char *kScreenVert =
	"#version 300 es\n"
	"layout(location=0) in vec3 pos;\n"
	"layout(location=1) in vec2 uv;\n"
	"uniform mat4 mvp;\n"
	"out vec2 vUV;\n"
	"void main() { gl_Position = mvp * vec4(pos,1); vUV = uv; }\n";

static const char *kScreenFrag =
	"#version 300 es\n"
	"precision mediump float;\n"
	"in vec2 vUV;\n"
	"uniform sampler2D tex;\n"
	"out vec4 color;\n"
	"void main() { color = texture(tex, vUV); }\n";

struct VRApp {
	android_app *app;
	bool windowInit;

	EGLDisplay eglDisplay;
	EGLConfig eglConfig;
	EGLContext eglContext;
	EGLSurface eglSurface;

	// ScummVM thread and shared texture
	EGLContext scummvmContext;  // shared GL context for ScummVM thread
	EGLSurface scummvmSurface; // PBuffer for ScummVM thread
	GLuint scummvmFBO;         // FBO ScummVM renders to
	GLuint scummvmTexture;     // shared texture (written by ScummVM, read by VR)
	pthread_t scummvmThread;
	bool scummvmRunning;

	XrInstance instance;
	XrSystemId system;
	XrSession session;
	XrSpace localSpace;
	XrSessionState sessionState;
	bool sessionRunning;
	bool running;

	uint32_t viewCount;
	XrViewConfigurationView viewConfigs[MAX_VIEWS];
	XrSwapchain swapchains[MAX_VIEWS];
	XrSwapchainImageOpenGLESKHR swapchainImages[MAX_VIEWS][MAX_SWAPCHAIN_LENGTH];
	uint32_t swapchainLengths[MAX_VIEWS];
	GLuint swapchainFBOs[MAX_VIEWS][MAX_SWAPCHAIN_LENGTH];
	GLuint depthBuffers[MAX_VIEWS];

	XrFrameState frameState;

	// Virtual screen rendering
	GLuint screenProgram;
	GLuint screenVBO;
	GLint screenMVPLoc;
	GLint screenTexLoc;
	GLuint testTexture;

	// Controller input
	XrActionSet actionSet;
	XrAction poseAction;
	XrAction triggerAction;
	XrAction gripAction;
	XrAction thumbstickAction;
	XrAction menuAction;
	XrAction bButtonAction;
	XrPath handPaths[2];
	XrSpace handSpaces[2];

	// Input state
	XrPosef handPoses[2];
	bool bButtonPressed;
	bool prevBButtonPressed;
	float triggerValues[2];
	float prevTriggerValues[2];
	float gripValues[2];
	float prevGripValues[2];
	XrVector2f thumbstickValues[2];
	int mouseX, mouseY;
	bool mouseValid;

	// Laser pointer data (world space)
	float laserStartX, laserStartY, laserStartZ;
	float laserEndX, laserEndY, laserEndZ;
	bool laserActive;

	// Scene rotation (trigger+grip to rotate)
	float sceneRotX, sceneRotY;
	bool grabbing;
	float grabStartX, grabStartY;
};

// ---- Matrix math ----
static void mat4_identity(float *m) {
	memset(m, 0, 64);
	m[0]=m[5]=m[10]=m[15]=1;
}

static void mat4_multiply(float *out, const float *a, const float *b) {
	float t[16];
	for (int i=0;i<4;i++) for (int j=0;j<4;j++) {
		t[i*4+j]=0;
		for (int k=0;k<4;k++) t[i*4+j] += a[k*4+j]*b[i*4+k];
	}
	memcpy(out,t,64);
}

static void mat4_from_pose(float *m, const XrPosef &p) {
	float x=p.orientation.x, y=p.orientation.y, z=p.orientation.z, w=p.orientation.w;

	// Build rotation matrix from quaternion
	float r[16];
	r[0]=1-2*(y*y+z*z); r[1]=2*(x*y+w*z);   r[2]=2*(x*z-w*y);   r[3]=0;
	r[4]=2*(x*y-w*z);   r[5]=1-2*(x*x+z*z); r[6]=2*(y*z+w*x);   r[7]=0;
	r[8]=2*(x*z+w*y);   r[9]=2*(y*z-w*x);   r[10]=1-2*(x*x+y*y);r[11]=0;
	r[12]=0; r[13]=0; r[14]=0; r[15]=1;

	// Invert: transpose the 3x3 rotation part
	m[0]=r[0]; m[1]=r[4]; m[2]=r[8];  m[3]=0;
	m[4]=r[1]; m[5]=r[5]; m[6]=r[9];  m[7]=0;
	m[8]=r[2]; m[9]=r[6]; m[10]=r[10]; m[11]=0;
	m[12]=0;   m[13]=0;   m[14]=0;     m[15]=1;

	// Apply negative position translation (matches working Quest examples)
	float nx=-p.position.x, ny=-p.position.y, nz=-p.position.z;
	m[12] = m[0]*nx + m[4]*ny + m[8]*nz;
	m[13] = m[1]*nx + m[5]*ny + m[9]*nz;
	m[14] = m[2]*nx + m[6]*ny + m[10]*nz;
}

static void mat4_projection(float *m, const XrFovf &fov, float n, float f) {
	float tl=tanf(fov.angleLeft), tr=tanf(fov.angleRight);
	float tu=tanf(fov.angleUp), td=tanf(fov.angleDown);
	float w=tr-tl, h=tu-td;
	memset(m,0,64);
	m[0]=2/w; m[5]=2/h;
	m[8]=(tr+tl)/w; m[9]=(tu+td)/h;
	m[10]=-(f+n)/(f-n); m[11]=-1;
	m[14]=-2*f*n/(f-n);
}

// ---- OpenXR/EGL setup (same as before, condensed) ----
static void handleCmd(android_app *app, int32_t cmd) {
	VRApp *a = (VRApp *)app->userData;
	if (cmd == APP_CMD_INIT_WINDOW) { a->windowInit = true; LOGI("Window ready"); }
	else if (cmd == APP_CMD_DESTROY) a->running = false;
}

static bool initEGL(VRApp *a) {
	a->eglDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	EGLint maj, min;
	eglInitialize(a->eglDisplay, &maj, &min);
	EGLint cfg[] = {EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,
		EGL_DEPTH_SIZE,16,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES3_BIT,EGL_NONE};
	EGLint nc;
	eglChooseConfig(a->eglDisplay, cfg, &a->eglConfig, 1, &nc);
	EGLint ctx[] = {EGL_CONTEXT_CLIENT_VERSION,3,EGL_NONE};
	a->eglContext = eglCreateContext(a->eglDisplay, a->eglConfig, EGL_NO_CONTEXT, ctx);
	EGLint sa[] = {EGL_NONE};
	a->eglSurface = eglCreateWindowSurface(a->eglDisplay, a->eglConfig, a->app->window, sa);
	eglMakeCurrent(a->eglDisplay, a->eglSurface, a->eglSurface, a->eglContext);
	LOGI("EGL %d.%d, GL: %s", maj, min, glGetString(GL_RENDERER));
	return true;
}

static bool initOpenXR(VRApp *a) {
	XrResult r;
	PFN_xrInitializeLoaderKHR loaderInit;
	xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR", (PFN_xrVoidFunction *)&loaderInit);
	XrLoaderInitInfoAndroidKHR li = {XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
	li.applicationVM = a->app->activity->vm;
	li.applicationContext = a->app->activity->clazz;
	if (XR_FAILED(loaderInit((XrLoaderInitInfoBaseHeaderKHR *)&li))) { LOGE("Loader fail"); return false; }

	const char *exts[] = {XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME};
	XrInstanceCreateInfo ii = {XR_TYPE_INSTANCE_CREATE_INFO};
	strcpy(ii.applicationInfo.applicationName, "ScummVM VR");
	ii.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
	ii.enabledExtensionCount = 1; ii.enabledExtensionNames = exts;
	if (XR_FAILED(xrCreateInstance(&ii, &a->instance))) { LOGE("Instance fail"); return false; }

	XrSystemGetInfo si = {XR_TYPE_SYSTEM_GET_INFO};
	si.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	xrGetSystem(a->instance, &si, &a->system);

	xrEnumerateViewConfigurationViews(a->instance, a->system,
		XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &a->viewCount, nullptr);
	for (uint32_t i=0;i<a->viewCount;i++) {
		a->viewConfigs[i].type=XR_TYPE_VIEW_CONFIGURATION_VIEW; a->viewConfigs[i].next=nullptr;
	}
	xrEnumerateViewConfigurationViews(a->instance, a->system,
		XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, a->viewCount, &a->viewCount, a->viewConfigs);
	LOGI("Views: %u @ %ux%u", a->viewCount,
		a->viewConfigs[0].recommendedImageRectWidth, a->viewConfigs[0].recommendedImageRectHeight);

	PFN_xrGetOpenGLESGraphicsRequirementsKHR getReqs;
	xrGetInstanceProcAddr(a->instance, "xrGetOpenGLESGraphicsRequirementsKHR", (PFN_xrVoidFunction *)&getReqs);
	XrGraphicsRequirementsOpenGLESKHR reqs = {XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
	getReqs(a->instance, a->system, &reqs);

	XrGraphicsBindingOpenGLESAndroidKHR bind = {XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};
	bind.display=a->eglDisplay; bind.config=a->eglConfig; bind.context=a->eglContext;
	XrSessionCreateInfo sci = {XR_TYPE_SESSION_CREATE_INFO};
	sci.next=&bind; sci.systemId=a->system;
	if (XR_FAILED(xrCreateSession(a->instance, &sci, &a->session))) { LOGE("Session fail"); return false; }

	XrReferenceSpaceCreateInfo rsi = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
	rsi.referenceSpaceType=XR_REFERENCE_SPACE_TYPE_LOCAL;
	rsi.poseInReferenceSpace.orientation.w=1;
	xrCreateReferenceSpace(a->session, &rsi, &a->localSpace);

	for (uint32_t v=0; v<a->viewCount; v++) {
		XrSwapchainCreateInfo swi = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
		swi.usageFlags=XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
		swi.format=GL_SRGB8_ALPHA8; swi.sampleCount=1;
		swi.width=a->viewConfigs[v].recommendedImageRectWidth;
		swi.height=a->viewConfigs[v].recommendedImageRectHeight;
		swi.faceCount=1; swi.arraySize=1; swi.mipCount=1;
		xrCreateSwapchain(a->session, &swi, &a->swapchains[v]);

		uint32_t ic;
		xrEnumerateSwapchainImages(a->swapchains[v], 0, &ic, nullptr);
		a->swapchainLengths[v]=ic;
		for (uint32_t i=0;i<ic;i++) {
			a->swapchainImages[v][i].type=XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
			a->swapchainImages[v][i].next=nullptr;
		}
		xrEnumerateSwapchainImages(a->swapchains[v], ic, &ic,
			(XrSwapchainImageBaseHeader *)a->swapchainImages[v]);
		glGenFramebuffers(ic, a->swapchainFBOs[v]);
		for (uint32_t i=0;i<ic;i++) {
			glBindFramebuffer(GL_FRAMEBUFFER, a->swapchainFBOs[v][i]);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
				a->swapchainImages[v][i].image, 0);
		}
		glGenTextures(1, &a->depthBuffers[v]);
		glBindTexture(GL_TEXTURE_2D, a->depthBuffers[v]);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24,
			swi.width, swi.height, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, nullptr);
	}
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	LOGI("OpenXR ready");
	return true;
}

static bool initInput(VRApp *a) {
	XrResult r;

	// Action set
	XrActionSetCreateInfo asi = {XR_TYPE_ACTION_SET_CREATE_INFO};
	strcpy(asi.actionSetName, "scummvm");
	strcpy(asi.localizedActionSetName, "ScummVM");
	r = xrCreateActionSet(a->instance, &asi, &a->actionSet);
	if (XR_FAILED(r)) { LOGE("ActionSet fail: %d", r); return false; }

	// Hand paths
	xrStringToPath(a->instance, "/user/hand/left", &a->handPaths[0]);
	xrStringToPath(a->instance, "/user/hand/right", &a->handPaths[1]);

	// Actions
	auto makeAction = [&](XrAction &action, const char *name, const char *loc, XrActionType type) {
		XrActionCreateInfo ci = {XR_TYPE_ACTION_CREATE_INFO};
		ci.actionType = type;
		strcpy(ci.actionName, name);
		strcpy(ci.localizedActionName, loc);
		ci.countSubactionPaths = 2;
		ci.subactionPaths = a->handPaths;
		xrCreateAction(a->actionSet, &ci, &action);
	};

	makeAction(a->poseAction, "aim_pose", "Aim Pose", XR_ACTION_TYPE_POSE_INPUT);
	makeAction(a->triggerAction, "trigger", "Trigger", XR_ACTION_TYPE_FLOAT_INPUT);
	makeAction(a->gripAction, "grip", "Grip", XR_ACTION_TYPE_FLOAT_INPUT);
	makeAction(a->thumbstickAction, "thumbstick", "Thumbstick", XR_ACTION_TYPE_VECTOR2F_INPUT);

	// Menu (left hand only)
	XrActionCreateInfo mci = {XR_TYPE_ACTION_CREATE_INFO};
	mci.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
	strcpy(mci.actionName, "menu");
	strcpy(mci.localizedActionName, "Menu");
	mci.countSubactionPaths = 0;
	xrCreateAction(a->actionSet, &mci, &a->menuAction);

	// B button (right hand) → ESC
	XrActionCreateInfo bci = {XR_TYPE_ACTION_CREATE_INFO};
	bci.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
	strcpy(bci.actionName, "b_button");
	strcpy(bci.localizedActionName, "B Button");
	bci.countSubactionPaths = 0;
	xrCreateAction(a->actionSet, &bci, &a->bButtonAction);

	// Suggest bindings for Oculus Touch
	XrPath profilePath;
	xrStringToPath(a->instance, "/interaction_profiles/oculus/touch_controller", &profilePath);

	auto p = [&](const char *s) -> XrPath { XrPath p; xrStringToPath(a->instance, s, &p); return p; };

	XrActionSuggestedBinding bindings[] = {
		{a->poseAction, p("/user/hand/left/input/aim/pose")},
		{a->poseAction, p("/user/hand/right/input/aim/pose")},
		{a->triggerAction, p("/user/hand/left/input/trigger/value")},
		{a->triggerAction, p("/user/hand/right/input/trigger/value")},
		{a->gripAction, p("/user/hand/left/input/squeeze/value")},
		{a->gripAction, p("/user/hand/right/input/squeeze/value")},
		{a->thumbstickAction, p("/user/hand/left/input/thumbstick")},
		{a->thumbstickAction, p("/user/hand/right/input/thumbstick")},
		{a->menuAction, p("/user/hand/left/input/menu/click")},
		{a->bButtonAction, p("/user/hand/right/input/b/click")},
	};
	XrInteractionProfileSuggestedBinding sb = {XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
	sb.interactionProfile = profilePath;
	sb.suggestedBindings = bindings;
	sb.countSuggestedBindings = sizeof(bindings) / sizeof(bindings[0]);
	r = xrSuggestInteractionProfileBindings(a->instance, &sb);
	if (XR_FAILED(r)) { LOGE("Bindings fail: %d", r); return false; }

	// Hand spaces
	for (int h = 0; h < 2; h++) {
		XrActionSpaceCreateInfo asci = {XR_TYPE_ACTION_SPACE_CREATE_INFO};
		asci.action = a->poseAction;
		asci.subactionPath = a->handPaths[h];
		asci.poseInActionSpace.orientation.w = 1;
		xrCreateActionSpace(a->session, &asci, &a->handSpaces[h]);
	}

	// Attach to session
	XrSessionActionSetsAttachInfo attach = {XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
	attach.countActionSets = 1;
	attach.actionSets = &a->actionSet;
	r = xrAttachSessionActionSets(a->session, &attach);
	if (XR_FAILED(r)) { LOGE("Attach fail: %d", r); return false; }

	LOGI("Input initialized");
	return true;
}

// ---- Virtual screen setup ----
static GLuint compileShader(GLenum type, const char *src) {
	GLuint s = glCreateShader(type);
	glShaderSource(s, 1, &src, nullptr);
	glCompileShader(s);
	GLint ok; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) { char log[256]; glGetShaderInfoLog(s,256,nullptr,log); LOGE("Shader: %s",log); }
	return s;
}

static void initScreenQuad(VRApp *a) {
	GLuint vs = compileShader(GL_VERTEX_SHADER, kScreenVert);
	GLuint fs = compileShader(GL_FRAGMENT_SHADER, kScreenFrag);
	a->screenProgram = glCreateProgram();
	glAttachShader(a->screenProgram, vs);
	glAttachShader(a->screenProgram, fs);
	glLinkProgram(a->screenProgram);
	glDeleteShader(vs); glDeleteShader(fs);
	a->screenMVPLoc = glGetUniformLocation(a->screenProgram, "mvp");
	a->screenTexLoc = glGetUniformLocation(a->screenProgram, "tex");

	// Virtual screen: 3.2m wide, 1.8m tall, 2.5m away, centered at eye height
	float hw = 1.6f, hh = 0.9f, d = -2.5f;
	float verts[] = {
		// pos (x,y,z)          uv
		-hw, -hh, d,   0, 1,   // bottom-left
		 hw, -hh, d,   1, 1,   // bottom-right
		-hw,  hh, d,   0, 0,   // top-left
		 hw,  hh, d,   1, 0,   // top-right
	};
	glGenBuffers(1, &a->screenVBO);
	glBindBuffer(GL_ARRAY_BUFFER, a->screenVBO);
	glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
	glBindBuffer(GL_ARRAY_BUFFER, 0);

	// No test texture needed — the shared texture gets ScummVM's content
	a->testTexture = a->scummvmTexture;

	LOGI("Virtual screen quad initialized (using shared texture)");
}

static void drawScreen(VRApp *a, const float *viewProj) {
	glUseProgram(a->screenProgram);
	// MVP = viewProj * identity (screen is already in world coords)
	glUniformMatrix4fv(a->screenMVPLoc, 1, GL_FALSE, viewProj);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, a->testTexture);
	glUniform1i(a->screenTexLoc, 0);

	glBindBuffer(GL_ARRAY_BUFFER, a->screenVBO);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 20, (void*)0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 20, (void*)12);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glDisableVertexAttribArray(0);
	glDisableVertexAttribArray(1);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glUseProgram(0);
}

// ---- Event loop and rendering ----
static void pumpEvents(VRApp *a) {
	int events;
	struct android_poll_source *source;
	while (ALooper_pollAll(0, nullptr, &events, (void **)&source) >= 0) {
		if (source) source->process(a->app, source);
	}
	XrEventDataBuffer ev = {XR_TYPE_EVENT_DATA_BUFFER};
	while (xrPollEvent(a->instance, &ev) == XR_SUCCESS) {
		if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
			auto *sc = (XrEventDataSessionStateChanged *)&ev;
			a->sessionState = sc->state;
			LOGI("Session state: %d", a->sessionState);
			if (a->sessionState == XR_SESSION_STATE_READY) {
				XrSessionBeginInfo b = {XR_TYPE_SESSION_BEGIN_INFO};
				b.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
				xrBeginSession(a->session, &b);
				a->sessionRunning = true;
				LOGI("Session started!");
			} else if (a->sessionState == XR_SESSION_STATE_STOPPING) {
				xrEndSession(a->session);
				a->sessionRunning = false;
			} else if (a->sessionState == XR_SESSION_STATE_EXITING) {
				a->running = false;
			}
		}
		ev.type = XR_TYPE_EVENT_DATA_BUFFER;
	}
}

static bool g_dioramaMode = true;

static void processInput(VRApp *a) {
	if (!a->sessionRunning) return;

	// Sync actions
	XrActiveActionSet active = {};
	active.actionSet = a->actionSet;
	XrActionsSyncInfo syncInfo = {XR_TYPE_ACTIONS_SYNC_INFO};
	syncInfo.countActiveActionSets = 1;
	syncInfo.activeActionSets = &active;
	xrSyncActions(a->session, &syncInfo);

	// Save previous trigger/grip state for edge detection
	a->prevTriggerValues[0] = a->triggerValues[0];
	a->prevTriggerValues[1] = a->triggerValues[1];
	a->prevGripValues[0] = a->gripValues[0];
	a->prevGripValues[1] = a->gripValues[1];

	// Get hand poses and button states
	for (int h = 0; h < 2; h++) {
		XrSpaceLocation loc = {XR_TYPE_SPACE_LOCATION};
		xrLocateSpace(a->handSpaces[h], a->localSpace,
			a->frameState.predictedDisplayTime, &loc);
		if (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)
			a->handPoses[h] = loc.pose;

		XrActionStateGetInfo gi = {XR_TYPE_ACTION_STATE_GET_INFO};
		gi.subactionPath = a->handPaths[h];

		gi.action = a->triggerAction;
		XrActionStateFloat tf = {XR_TYPE_ACTION_STATE_FLOAT};
		if (XR_SUCCEEDED(xrGetActionStateFloat(a->session, &gi, &tf)) && tf.isActive)
			a->triggerValues[h] = tf.currentState;

		gi.action = a->gripAction;
		XrActionStateFloat gf = {XR_TYPE_ACTION_STATE_FLOAT};
		if (XR_SUCCEEDED(xrGetActionStateFloat(a->session, &gi, &gf)) && gf.isActive)
			a->gripValues[h] = gf.currentState;

		gi.action = a->thumbstickAction;
		XrActionStateVector2f tv = {XR_TYPE_ACTION_STATE_VECTOR2F};
		if (XR_SUCCEEDED(xrGetActionStateVector2f(a->session, &gi, &tv)) && tv.isActive)
			a->thumbstickValues[h] = tv.currentState;
	}

	// Ray-screen intersection for right controller (hand index 1)
	XrPosef &pose = a->handPoses[1];
	float qx = pose.orientation.x, qy = pose.orientation.y;
	float qz = pose.orientation.z, qw = pose.orientation.w;
	float dirX = -2.0f * (qx * qz - qw * qy);
	float dirY = -2.0f * (qy * qz + qw * qx);
	float dirZ = -(1.0f - 2.0f * (qx * qx + qy * qy));

	// Store laser start (controller position)
	a->laserStartX = pose.position.x;
	a->laserStartY = pose.position.y;
	a->laserStartZ = pose.position.z;
	a->laserActive = false;

	// Screen Z and dimensions depend on mode
	float screenZ, hw, hh, screenCenterY;
	if (g_dioramaMode && dioramaHasData()) {
		screenZ = -DIORAMA_DISTANCE - DIORAMA_DEPTH; // back wall
		hw = DIORAMA_WIDTH / 2.0f;
		// Get aspect from diorama data
		const DioramaSnapshot *snap = g_dioramaState ? g_dioramaState->getReadBuffer() : nullptr;
		float aspect = (snap && snap->screenHeight > 0) ?
			(float)snap->screenWidth / (float)snap->screenHeight : 2.2f;
		hh = (DIORAMA_WIDTH / aspect) / 2.0f;
		screenCenterY = DIORAMA_CENTER_Y + hh; // center of the back wall
	} else {
		screenZ = -2.5f;
		hw = 1.6f;
		hh = 0.9f;
		screenCenterY = 0.0f;
	}

	if (fabsf(dirZ) > 1e-6f) {
		float t = (screenZ - pose.position.z) / dirZ;
		if (t > 0) {
			float hitX = pose.position.x + t * dirX;
			float hitY = pose.position.y + t * dirY;

			// Check if hit is within the screen bounds
			float relY = hitY - screenCenterY + hh;
			if (hitX >= -hw && hitX <= hw && relY >= 0 && relY <= 2 * hh) {
				// Compute UV on the screen quad
				float u = 1.0f - (hitX + hw) / (2 * hw); // mirrored X
				float v = relY / (2 * hh);
				int mx = (int)(u * 1920);
				int my = (int)(v * 1080);

				if (mx != a->mouseX || my != a->mouseY) {
					a->mouseX = mx;
					a->mouseY = my;
					a->mouseValid = true;

					// Store cursor UV for diorama cursor rendering
					g_dioramaCursorX = (int)(u * 320);
					g_dioramaCursorY = (int)(v * 200);

					// Push mouse move event to ScummVM
					if (g_system) {
						OSystem_Android *sys = dynamic_cast<OSystem_Android *>(g_system);
						if (sys) {
							Common::Event ev;
							ev.type = Common::EVENT_MOUSEMOVE;
							ev.mouse = Common::Point(mx, my);
							sys->pushEvent(ev);
						}
					}
				}
			}
		}
	}

	// Right trigger → left click (edge detection at 0.5 threshold)
	if (g_system) {
		OSystem_Android *sys = dynamic_cast<OSystem_Android *>(g_system);
		if (sys && a->mouseValid) {
			const float THRESH = 0.5f;

			// Trigger → left click
			if (a->prevTriggerValues[1] < THRESH && a->triggerValues[1] >= THRESH) {
				Common::Event ev;
				ev.type = Common::EVENT_LBUTTONDOWN;
				ev.mouse = Common::Point(a->mouseX, a->mouseY);
				sys->pushEvent(ev);
			}
			if (a->prevTriggerValues[1] >= THRESH && a->triggerValues[1] < THRESH) {
				Common::Event ev;
				ev.type = Common::EVENT_LBUTTONUP;
				ev.mouse = Common::Point(a->mouseX, a->mouseY);
				sys->pushEvent(ev);
			}

			// Grip → right click
			if (a->prevGripValues[1] < THRESH && a->gripValues[1] >= THRESH) {
				Common::Event ev;
				ev.type = Common::EVENT_RBUTTONDOWN;
				ev.mouse = Common::Point(a->mouseX, a->mouseY);
				sys->pushEvent(ev);
			}
			if (a->prevGripValues[1] >= THRESH && a->gripValues[1] < THRESH) {
				Common::Event ev;
				ev.type = Common::EVENT_RBUTTONUP;
				ev.mouse = Common::Point(a->mouseX, a->mouseY);
				sys->pushEvent(ev);
			}

			// Left thumbstick Y → scroll
			if (a->thumbstickValues[0].y > 0.5f) {
				Common::Event ev;
				ev.type = Common::EVENT_WHEELUP;
				ev.mouse = Common::Point(a->mouseX, a->mouseY);
				sys->pushEvent(ev);
			} else if (a->thumbstickValues[0].y < -0.5f) {
				Common::Event ev;
				ev.type = Common::EVENT_WHEELDOWN;
				ev.mouse = Common::Point(a->mouseX, a->mouseY);
				sys->pushEvent(ev);
			}
		}
	}

	// B button → ESC (edge-detected, doesn't need mouse position)
	a->prevBButtonPressed = a->bButtonPressed;
	XrActionStateGetInfo bgi = {XR_TYPE_ACTION_STATE_GET_INFO};
	bgi.action = a->bButtonAction;
	XrActionStateBoolean bbs = {XR_TYPE_ACTION_STATE_BOOLEAN};
	if (XR_SUCCEEDED(xrGetActionStateBoolean(a->session, &bgi, &bbs)) && bbs.isActive)
		a->bButtonPressed = bbs.currentState;

	if (a->bButtonPressed && !a->prevBButtonPressed && g_system) {
		OSystem_Android *sys = dynamic_cast<OSystem_Android *>(g_system);
		if (sys) {
			Common::Event ev;
			ev.type = Common::EVENT_KEYDOWN;
			ev.kbd.keycode = Common::KEYCODE_ESCAPE;
			ev.kbd.ascii = 27;
			ev.kbd.flags = 0;
			sys->pushEvent(ev);

			Common::Event evUp;
			evUp.type = Common::EVENT_KEYUP;
			evUp.kbd.keycode = Common::KEYCODE_ESCAPE;
			evUp.kbd.ascii = 27;
			evUp.kbd.flags = 0;
			sys->pushEvent(evUp);
		}
	}

	// Scene rotation: hold trigger+grip on right controller to grab and rotate
	bool isGrabbing = a->triggerValues[1] > 0.5f && a->gripValues[1] > 0.5f;
	XrPosef &rPose = a->handPoses[1];
	float handX = rPose.position.x;
	float handY = rPose.position.y;

	if (isGrabbing && !a->grabbing) {
		// Start grab
		a->grabbing = true;
		a->grabStartX = handX;
		a->grabStartY = handY;
	} else if (isGrabbing && a->grabbing) {
		// Dragging — rotate scene
		float dx = handX - a->grabStartX;
		float dy = handY - a->grabStartY;
		a->sceneRotY += dx * 3.0f;
		a->sceneRotX += dy * 3.0f;
		g_dioramaRotX = a->sceneRotX;
		g_dioramaRotY = a->sceneRotY;
		a->grabStartX = handX;
		a->grabStartY = handY;
	} else {
		a->grabbing = false;
	}
}

// Laser pointer shader (simple colored line, no texture)
static const char *kLaserVert =
	"#version 300 es\n"
	"layout(location=0) in vec3 pos;\n"
	"uniform mat4 mvp;\n"
	"void main() { gl_Position = mvp * vec4(pos, 1.0); }\n";

static const char *kLaserFrag =
	"#version 300 es\n"
	"precision mediump float;\n"
	"uniform vec4 uColor;\n"
	"out vec4 color;\n"
	"void main() { color = uColor; }\n";

static GLuint g_laserProgram = 0;
static GLint g_laserMVPLoc = -1;
static GLint g_laserColorLoc = -1;
static GLuint g_laserVBO = 0;

static void initLaser() {
	GLuint vs = compileShader(GL_VERTEX_SHADER, kLaserVert);
	GLuint fs = compileShader(GL_FRAGMENT_SHADER, kLaserFrag);
	g_laserProgram = glCreateProgram();
	glAttachShader(g_laserProgram, vs);
	glAttachShader(g_laserProgram, fs);
	glLinkProgram(g_laserProgram);
	glDeleteShader(vs);
	glDeleteShader(fs);
	g_laserMVPLoc = glGetUniformLocation(g_laserProgram, "mvp");
	g_laserColorLoc = glGetUniformLocation(g_laserProgram, "uColor");
	glGenBuffers(1, &g_laserVBO);
}

static void drawLaser(VRApp *a, const float *viewProj) {
	if (!a->laserActive || !g_laserProgram)
		return;

	// Line from controller to hit point
	float verts[] = {
		a->laserStartX, a->laserStartY, a->laserStartZ,
		a->laserEndX,   a->laserEndY,   a->laserEndZ,
	};

	glUseProgram(g_laserProgram);

	// Identity model matrix — vertices are already in world space
	glUniformMatrix4fv(g_laserMVPLoc, 1, GL_FALSE, viewProj);
	glUniform4f(g_laserColorLoc, 0.2f, 0.8f, 1.0f, 0.8f); // cyan laser

	glBindBuffer(GL_ARRAY_BUFFER, g_laserVBO);
	glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 12, (void *)0);

	glLineWidth(3.0f);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE); // additive blend for glow
	glDrawArrays(GL_LINES, 0, 2);

	// Draw a dot at the hit point
	float dot[] = {
		a->laserEndX, a->laserEndY, a->laserEndZ + 0.001f,
	};
	glBufferData(GL_ARRAY_BUFFER, sizeof(dot), dot, GL_DYNAMIC_DRAW);
	glUniform4f(g_laserColorLoc, 1.0f, 1.0f, 1.0f, 1.0f); // white dot
	glDrawArrays(GL_POINTS, 0, 1);

	glDisable(GL_BLEND);
	glDisableVertexAttribArray(0);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glUseProgram(0);
}

static void renderFrame(VRApp *a) {
	if (!a->sessionRunning) return;

	a->frameState.type = XR_TYPE_FRAME_STATE;
	XrFrameWaitInfo wi = {XR_TYPE_FRAME_WAIT_INFO};
	xrWaitFrame(a->session, &wi, &a->frameState);
	XrFrameBeginInfo bi = {XR_TYPE_FRAME_BEGIN_INFO};
	xrBeginFrame(a->session, &bi);

	// Process controller input after xrWaitFrame (so predictedDisplayTime is valid)
	processInput(a);

	XrCompositionLayerProjectionView projViews[MAX_VIEWS] = {};
	XrCompositionLayerProjection layer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
	bool render = a->frameState.shouldRender;

	if (render) {
		XrViewLocateInfo vli = {XR_TYPE_VIEW_LOCATE_INFO};
		vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
		vli.displayTime = a->frameState.predictedDisplayTime;
		vli.space = a->localSpace;
		XrViewState vs = {XR_TYPE_VIEW_STATE};
		XrView views[MAX_VIEWS];
		for (uint32_t i=0;i<a->viewCount;i++) views[i].type=XR_TYPE_VIEW;
		uint32_t vc;
		xrLocateViews(a->session, &vli, &vs, a->viewCount, &vc, views);

		for (uint32_t v=0; v<vc; v++) {
			XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
			uint32_t idx;
			xrAcquireSwapchainImage(a->swapchains[v], &ai, &idx);
			XrSwapchainImageWaitInfo swi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
			swi.timeout = XR_INFINITE_DURATION;
			xrWaitSwapchainImage(a->swapchains[v], &swi);

			uint32_t w = a->viewConfigs[v].recommendedImageRectWidth;
			uint32_t h = a->viewConfigs[v].recommendedImageRectHeight;

			glBindFramebuffer(GL_FRAMEBUFFER, a->swapchainFBOs[v][idx]);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, a->depthBuffers[v], 0);
			glViewport(0, 0, w, h);
			glEnable(GL_DEPTH_TEST);
			glClearColor(0.03f, 0.03f, 0.06f, 1.0f);
			glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

			// Build view-projection matrix
			float viewMat[16], projMat[16], vpMat[16];
			mat4_from_pose(viewMat, views[v].pose);
			mat4_projection(projMat, views[v].fov, 0.1f, 100.0f);
			mat4_multiply(vpMat, projMat, viewMat);

			// Draw the virtual screen
			if (g_dioramaMode && dioramaHasData()) {
				dioramaRendererDraw(vpMat);
			} else {
				drawScreen(a, vpMat);
			}

			// Cursor crosshair at hit point
			if (a->laserActive && g_laserProgram) {
				float cx = a->laserEndX, cy = a->laserEndY, cz = a->laserEndZ + 0.005f;
				float s = 0.015f; // crosshair size in meters
				float cross[] = {
					cx - s, cy, cz,   cx + s, cy, cz,     // horizontal
					cx, cy - s, cz,   cx, cy + s, cz,     // vertical
				};
				glUseProgram(g_laserProgram);
				glUniformMatrix4fv(g_laserMVPLoc, 1, GL_FALSE, vpMat);
				glUniform4f(g_laserColorLoc, 1.0f, 1.0f, 1.0f, 1.0f);
				glBindBuffer(GL_ARRAY_BUFFER, g_laserVBO);
				glBufferData(GL_ARRAY_BUFFER, sizeof(cross), cross, GL_DYNAMIC_DRAW);
				glEnableVertexAttribArray(0);
				glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 12, (void *)0);
				glLineWidth(2.0f);
				glDrawArrays(GL_LINES, 0, 4);
				glDisableVertexAttribArray(0);
				glBindBuffer(GL_ARRAY_BUFFER, 0);
				glUseProgram(0);
			}

			XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
			xrReleaseSwapchainImage(a->swapchains[v], &ri);

			projViews[v].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
			projViews[v].pose = views[v].pose;
			projViews[v].fov = views[v].fov;
			projViews[v].subImage.swapchain = a->swapchains[v];
			projViews[v].subImage.imageRect.offset = {0,0};
			projViews[v].subImage.imageRect.extent = {(int32_t)w,(int32_t)h};
		}
		layer.layerFlags = XR_COMPOSITION_LAYER_CORRECT_CHROMATIC_ABERRATION_BIT |
		                   XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
		layer.space = a->localSpace;
		layer.viewCount = vc;
		layer.views = projViews;
	}

	const XrCompositionLayerBaseHeader *layers[] = {(XrCompositionLayerBaseHeader *)&layer};
	XrFrameEndInfo ei = {XR_TYPE_FRAME_END_INFO};
	ei.displayTime = a->frameState.predictedDisplayTime;
	ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	ei.layerCount = render ? 1 : 0;
	ei.layers = render ? layers : nullptr;
	xrEndFrame(a->session, &ei);
}

// ---- ScummVM integration ----

// Forward declares
extern "C" int scummvm_main(int argc, const char * const argv[]);

#include "backends/platform/android/jni-android.h"

// Global pointer for ScummVM thread to find the shared texture
static VRApp *g_vrApp = nullptr;

// Shared texture ID — read by AndroidVRGraphicsManager on ScummVM thread
uint32_t g_vrSharedTexture = 0;

static DioramaSharedState s_dioramaState;
DioramaSharedState *g_dioramaState = nullptr;
int g_dioramaCursorX = 0;
int g_dioramaCursorY = 0;
float g_dioramaRotX = 0;
float g_dioramaRotY = 0;

// Diorama shared state — written by ScummVM thread, read by VR thread
// (Diorama globals are earlier in the file, before renderFrame)

// Cached class loader and method for finding app classes from native threads
static jobject g_classLoader = nullptr;
static jmethodID g_loadClassMethod = nullptr;

// Cache the application class loader from the main thread (which can FindClass)
static void cacheClassLoader(JNIEnv *env, jobject activity) {
	jclass actClass = env->GetObjectClass(activity);
	jmethodID getClassLoader = env->GetMethodID(actClass, "getClassLoader",
		"()Ljava/lang/ClassLoader;");
	jobject loader = env->CallObjectMethod(activity, getClassLoader);
	g_classLoader = env->NewGlobalRef(loader);

	jclass loaderClass = env->FindClass("java/lang/ClassLoader");
	g_loadClassMethod = env->GetMethodID(loaderClass, "loadClass",
		"(Ljava/lang/String;)Ljava/lang/Class;");
}

// Find an application class from any thread using the cached class loader
static jclass findAppClass(JNIEnv *env, const char *name) {
	jstring className = env->NewStringUTF(name);
	jclass cls = (jclass)env->CallObjectMethod(g_classLoader, g_loadClassMethod, className);
	env->DeleteLocalRef(className);
	if (env->ExceptionCheck()) {
		env->ExceptionDescribe();
		env->ExceptionClear();
		return nullptr;
	}
	return cls;
}

static void initSharedTexture(VRApp *a) {
	const int tw = 1920, th = 1080;

	// Create texture on the VR (main) context — it will be shared
	glGenTextures(1, &a->scummvmTexture);
	glBindTexture(GL_TEXTURE_2D, a->scummvmTexture);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tw, th, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glBindTexture(GL_TEXTURE_2D, 0);

	// Create a shared EGL context for the ScummVM thread
	EGLint ctxAttribs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
	a->scummvmContext = eglCreateContext(a->eglDisplay, a->eglConfig, a->eglContext, ctxAttribs);
	if (a->scummvmContext == EGL_NO_CONTEXT) {
		LOGE("Failed to create shared context for ScummVM");
		return;
	}

	// Create PBuffer surface for ScummVM thread — must be large enough
	// for ScummVM to render to (it renders to FBO 0 = PBuffer backbuffer)
	EGLint pbAttribs[] = { EGL_WIDTH, tw, EGL_HEIGHT, th, EGL_NONE };
	a->scummvmSurface = eglCreatePbufferSurface(a->eglDisplay, a->eglConfig, pbAttribs);

	g_vrSharedTexture = a->scummvmTexture;
	LOGI("Shared ScummVM context created, texture %u (%dx%d)", a->scummvmTexture, tw, th);
}

static void *scummvmThreadFunc(void *arg) {
	VRApp *a = (VRApp *)arg;

	LOGI("ScummVM thread starting");

	// Make the shared context current on this thread
	if (!eglMakeCurrent(a->eglDisplay, a->scummvmSurface, a->scummvmSurface, a->scummvmContext)) {
		LOGE("eglMakeCurrent failed on ScummVM thread");
		return nullptr;
	}

	// Attach this thread to the JavaVM
	JavaVM *vm = a->app->activity->vm;
	JNIEnv *env = nullptr;
	vm->AttachCurrentThread(&env, nullptr);

	// Create ScummVMVR Java instance for JNI callbacks
	// Must use cached class loader since this is a native thread
	jclass vrClass = findAppClass(env, "org.scummvm.scummvm.ScummVMVR");
	if (!vrClass) {
		LOGE("Cannot find ScummVMVR class");
		vm->DetachCurrentThread();
		return nullptr;
	}

	// Get AssetManager from the NativeActivity
	jobject activity = a->app->activity->clazz;
	jclass activityClass = env->GetObjectClass(activity);
	jmethodID getAssets = env->GetMethodID(activityClass, "getAssets",
		"()Landroid/content/res/AssetManager;");
	jobject assetManager = env->CallObjectMethod(activity, getAssets);

	// Construct ScummVMVR(Activity, AssetManager, callback)
	jmethodID ctor = env->GetMethodID(vrClass, "<init>",
		"(Landroid/app/Activity;Landroid/content/res/AssetManager;"
		"Lorg/scummvm/scummvm/MyScummVMDestroyedCallback;)V");
	if (!ctor) {
		LOGE("Cannot find ScummVMVR constructor");
		vm->DetachCurrentThread();
		return nullptr;
	}

	jobject scummvmObj = env->NewObject(vrClass, ctor, activity, assetManager, nullptr);
	if (!scummvmObj || env->ExceptionCheck()) {
		LOGE("Failed to create ScummVMVR instance");
		if (env->ExceptionCheck()) {
			env->ExceptionDescribe();
			env->ExceptionClear();
		}
		vm->DetachCurrentThread();
		return nullptr;
	}
	LOGI("ScummVMVR Java instance created");

	// Bootstrap ScummVM native side
	if (!JNI::createVR(env, scummvmObj, assetManager)) {
		LOGE("JNI::createVR failed");
		vm->DetachCurrentThread();
		return nullptr;
	}

	LOGI("Calling scummvm_main");

	// Build args
	const char *argv[] = { "scummvm", "--config=/data/user/0/org.scummvm.scummvm.vr.debug/files/scummvm.ini" };
	int argc = 2;

	int result = scummvm_main(argc, argv);

	LOGI("scummvm_main returned %d", result);

	a->scummvmRunning = false;
	vm->DetachCurrentThread();
	return nullptr;
}

extern "C" void android_main(android_app *app) {
	LOGI("ScummVM VR starting");
	VRApp a = {};
	a.app = app; a.running = true; a.scummvmRunning = true;
	app->userData = &a;
	app->onAppCmd = handleCmd;
	g_vrApp = &a;

	while (!a.windowInit) {
		int events;
		struct android_poll_source *source;
		if (ALooper_pollAll(0, nullptr, &events, (void **)&source) >= 0)
			if (source) source->process(app, source);
	}

	initEGL(&a);
	if (!initOpenXR(&a)) { LOGE("OpenXR init failed"); return; }
	if (!initInput(&a)) { LOGE("Input init failed (non-fatal)"); }

	// Cache the class loader from the main thread for use on native threads
	{
		JavaVM *vm = a.app->activity->vm;
		JNIEnv *env;
		vm->AttachCurrentThread(&env, nullptr);
		cacheClassLoader(env, a.app->activity->clazz);
		vm->DetachCurrentThread();
	}

	initSharedTexture(&a);
	initScreenQuad(&a);

	// Initialize diorama mode
	s_dioramaState.init();
	g_dioramaState = &s_dioramaState;
	dioramaRendererInit();
	initLaser();

	// Start ScummVM on a background thread
	pthread_create(&a.scummvmThread, nullptr, scummvmThreadFunc, &a);

	LOGI("Entering VR main loop");
	while (a.running) {
		pumpEvents(&a);
		renderFrame(&a);
	}

	// Signal ScummVM thread to stop
	a.scummvmRunning = false;
	pthread_join(a.scummvmThread, nullptr);

	LOGI("Exiting");
}

#endif // USE_OPENXR
