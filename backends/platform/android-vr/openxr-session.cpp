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
#include "graphics/opengl/system_headers.h"

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <vector>
#include <cstring>
#include <android/log.h>

#include "backends/platform/android-vr/openxr-session.h"

#define LOG_TAG "ScummVM_OpenXR"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#define XR_CHECK(cmd) \
	do { \
		XrResult result = (cmd); \
		if (XR_FAILED(result)) { \
			LOGE("OpenXR error: %s returned %d at %s:%d", #cmd, result, __FILE__, __LINE__); \
			return false; \
		} \
	} while (false)

static const int MAX_SWAPCHAIN_IMAGES = 8;

struct OpenXRSessionImpl {
	XrInstance instance;
	XrSystemId systemId;
	XrSession session;
	XrSpace localSpace;
	XrSwapchain swapchains[2];
	XrSwapchainImageOpenGLESKHR swapchainImages[2][MAX_SWAPCHAIN_IMAGES];
	uint32_t swapchainImageCount[2];
	GLuint swapchainFBOs[2][MAX_SWAPCHAIN_IMAGES];
	XrViewConfigurationView viewConfigs[2];
	XrSessionState sessionState;
	bool sessionRunning;
	XrFrameState xrFrameState;

	OpenXRSessionImpl() : instance(XR_NULL_HANDLE), systemId(XR_NULL_SYSTEM_ID),
		session(XR_NULL_HANDLE), localSpace(XR_NULL_HANDLE),
		sessionState(XR_SESSION_STATE_UNKNOWN), sessionRunning(false) {
		memset(&xrFrameState, 0, sizeof(xrFrameState));
		xrFrameState.type = XR_TYPE_FRAME_STATE;
		swapchains[0] = XR_NULL_HANDLE;
		swapchains[1] = XR_NULL_HANDLE;
		memset(swapchainImageCount, 0, sizeof(swapchainImageCount));
		memset(swapchainFBOs, 0, sizeof(swapchainFBOs));
		memset(viewConfigs, 0, sizeof(viewConfigs));
	}
};

OpenXRSession::OpenXRSession() : _impl(new OpenXRSessionImpl()) {
}

OpenXRSession::~OpenXRSession() {
	shutdown();
	delete _impl;
}

bool OpenXRSession::isSessionRunning() const {
	return _impl->sessionRunning;
}

uint32_t OpenXRSession::getSwapchainWidth() const {
	return _impl->viewConfigs[0].recommendedImageRectWidth;
}

uint32_t OpenXRSession::getSwapchainHeight() const {
	return _impl->viewConfigs[0].recommendedImageRectHeight;
}

void *OpenXRSession::getInstanceHandle() const {
	return (void *)_impl->instance;
}

void *OpenXRSession::getSessionHandle() const {
	return (void *)_impl->session;
}

void *OpenXRSession::getLocalSpaceHandle() const {
	return (void *)_impl->localSpace;
}

static bool createInstance(OpenXRSessionImpl *impl, JavaVM *vm, jobject activity) {
	// Step 1: Initialize the OpenXR loader (REQUIRED on Android before any other XR calls)
	PFN_xrInitializeLoaderKHR loaderInitFunc = nullptr;
	XrResult loaderResult = xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR",
		(PFN_xrVoidFunction *)&loaderInitFunc);
	if (XR_SUCCEEDED(loaderResult) && loaderInitFunc != nullptr) {
		XrLoaderInitInfoAndroidKHR loaderInitInfo = {};
		loaderInitInfo.type = XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR;
		loaderInitInfo.applicationVM = vm;
		loaderInitInfo.applicationContext = activity;
		XR_CHECK(loaderInitFunc((XrLoaderInitInfoBaseHeaderKHR *)&loaderInitInfo));
		LOGI("OpenXR loader initialized");
	} else {
		LOGE("Failed to get xrInitializeLoaderKHR");
		return false;
	}

	// Step 2: Create instance (no need for XR_KHR_android_create_instance
	// since we already initialized the loader with the Android context)
	const char *extensions[] = {
		XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME
	};

	XrInstanceCreateInfo createInfo = {};
	createInfo.type = XR_TYPE_INSTANCE_CREATE_INFO;
	createInfo.next = nullptr;
	strncpy(createInfo.applicationInfo.applicationName, "ScummVM VR", XR_MAX_APPLICATION_NAME_SIZE);
	createInfo.applicationInfo.applicationVersion = 1;
	strncpy(createInfo.applicationInfo.engineName, "ScummVM", XR_MAX_ENGINE_NAME_SIZE);
	createInfo.applicationInfo.engineVersion = 1;
	createInfo.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
	createInfo.enabledExtensionCount = 1;
	createInfo.enabledExtensionNames = extensions;

	XR_CHECK(xrCreateInstance(&createInfo, &impl->instance));

	XrSystemGetInfo systemInfo = {};
	systemInfo.type = XR_TYPE_SYSTEM_GET_INFO;
	systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;

	XR_CHECK(xrGetSystem(impl->instance, &systemInfo, &impl->systemId));

	uint32_t viewCount = 0;
	XR_CHECK(xrEnumerateViewConfigurationViews(impl->instance, impl->systemId,
		XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount, nullptr));

	if (viewCount != 2) {
		LOGE("Expected 2 views for stereo, got %u", viewCount);
		return false;
	}

	impl->viewConfigs[0].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
	impl->viewConfigs[1].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
	XR_CHECK(xrEnumerateViewConfigurationViews(impl->instance, impl->systemId,
		XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &viewCount, impl->viewConfigs));

	LOGI("OpenXR view: %ux%u per eye", impl->viewConfigs[0].recommendedImageRectWidth,
		impl->viewConfigs[0].recommendedImageRectHeight);

	return true;
}

static bool createSession(OpenXRSessionImpl *impl, EGLDisplay eglDisplay, EGLConfig eglConfig, EGLContext eglContext) {
	XrGraphicsRequirementsOpenGLESKHR gfxRequirements = {};
	gfxRequirements.type = XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR;

	PFN_xrGetOpenGLESGraphicsRequirementsKHR pfnGetRequirements = nullptr;
	XR_CHECK(xrGetInstanceProcAddr(impl->instance, "xrGetOpenGLESGraphicsRequirementsKHR",
		(PFN_xrVoidFunction *)&pfnGetRequirements));
	XR_CHECK(pfnGetRequirements(impl->instance, impl->systemId, &gfxRequirements));

	XrGraphicsBindingOpenGLESAndroidKHR gfxBinding = {};
	gfxBinding.type = XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR;
	gfxBinding.display = eglDisplay;
	gfxBinding.config = eglConfig;
	gfxBinding.context = eglContext;

	XrSessionCreateInfo sessionInfo = {};
	sessionInfo.type = XR_TYPE_SESSION_CREATE_INFO;
	sessionInfo.next = &gfxBinding;
	sessionInfo.systemId = impl->systemId;

	XR_CHECK(xrCreateSession(impl->instance, &sessionInfo, &impl->session));

	LOGI("OpenXR session created");
	return true;
}

static bool createReferenceSpace(OpenXRSessionImpl *impl) {
	XrReferenceSpaceCreateInfo spaceInfo = {};
	spaceInfo.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO;
	spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	spaceInfo.poseInReferenceSpace.orientation.w = 1.0f;

	XR_CHECK(xrCreateReferenceSpace(impl->session, &spaceInfo, &impl->localSpace));
	return true;
}

static bool createSwapchains(OpenXRSessionImpl *impl) {
	for (int eye = 0; eye < 2; eye++) {
		XrSwapchainCreateInfo swapchainInfo = {};
		swapchainInfo.type = XR_TYPE_SWAPCHAIN_CREATE_INFO;
		swapchainInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
		swapchainInfo.format = GL_SRGB8_ALPHA8;
		swapchainInfo.sampleCount = 1;
		swapchainInfo.width = impl->viewConfigs[eye].recommendedImageRectWidth;
		swapchainInfo.height = impl->viewConfigs[eye].recommendedImageRectHeight;
		swapchainInfo.faceCount = 1;
		swapchainInfo.arraySize = 1;
		swapchainInfo.mipCount = 1;

		XR_CHECK(xrCreateSwapchain(impl->session, &swapchainInfo, &impl->swapchains[eye]));

		uint32_t imageCount = 0;
		XR_CHECK(xrEnumerateSwapchainImages(impl->swapchains[eye], 0, &imageCount, nullptr));

		if (imageCount > MAX_SWAPCHAIN_IMAGES) {
			LOGE("Too many swapchain images: %u (max %d)", imageCount, MAX_SWAPCHAIN_IMAGES);
			return false;
		}
		impl->swapchainImageCount[eye] = imageCount;

		for (uint32_t i = 0; i < imageCount; i++) {
			impl->swapchainImages[eye][i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
			impl->swapchainImages[eye][i].next = nullptr;
		}
		XR_CHECK(xrEnumerateSwapchainImages(impl->swapchains[eye], imageCount, &imageCount,
			(XrSwapchainImageBaseHeader *)impl->swapchainImages[eye]));

		glGenFramebuffers(imageCount, impl->swapchainFBOs[eye]);

		for (uint32_t i = 0; i < imageCount; i++) {
			glBindFramebuffer(GL_FRAMEBUFFER, impl->swapchainFBOs[eye][i]);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
				impl->swapchainImages[eye][i].image, 0);

			GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
			if (status != GL_FRAMEBUFFER_COMPLETE) {
				LOGE("Swapchain FBO %d for eye %d not complete: 0x%x", i, eye, status);
				return false;
			}
		}

		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		LOGI("Created swapchain for eye %d: %u images", eye, imageCount);
	}

	return true;
}

bool OpenXRSession::init(JavaVM *vm, jobject activity, EGLDisplay eglDisplay, EGLConfig eglConfig, EGLContext eglContext) {
	if (!createInstance(_impl, vm, activity))
		return false;
	if (!createSession(_impl, eglDisplay, eglConfig, eglContext))
		return false;
	if (!createReferenceSpace(_impl))
		return false;
	if (!createSwapchains(_impl))
		return false;

	LOGI("OpenXR session initialized successfully");
	return true;
}

void OpenXRSession::shutdown() {
	for (int eye = 0; eye < 2; eye++) {
		if (_impl->swapchainImageCount[eye] > 0) {
			glDeleteFramebuffers(_impl->swapchainImageCount[eye], _impl->swapchainFBOs[eye]);
			_impl->swapchainImageCount[eye] = 0;
		}
		if (_impl->swapchains[eye] != XR_NULL_HANDLE) {
			xrDestroySwapchain(_impl->swapchains[eye]);
			_impl->swapchains[eye] = XR_NULL_HANDLE;
		}
	}

	if (_impl->localSpace != XR_NULL_HANDLE) {
		xrDestroySpace(_impl->localSpace);
		_impl->localSpace = XR_NULL_HANDLE;
	}

	if (_impl->session != XR_NULL_HANDLE) {
		xrDestroySession(_impl->session);
		_impl->session = XR_NULL_HANDLE;
	}

	if (_impl->instance != XR_NULL_HANDLE) {
		xrDestroyInstance(_impl->instance);
		_impl->instance = XR_NULL_HANDLE;
	}

	_impl->sessionRunning = false;
}

void OpenXRSession::handleSessionStateChanges() {
	XrEventDataBuffer eventData = {};
	eventData.type = XR_TYPE_EVENT_DATA_BUFFER;

	while (xrPollEvent(_impl->instance, &eventData) == XR_SUCCESS) {
		if (eventData.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
			XrEventDataSessionStateChanged *stateChanged =
				(XrEventDataSessionStateChanged *)&eventData;
			_impl->sessionState = stateChanged->state;

			LOGI("OpenXR session state changed to %d", _impl->sessionState);

			if (_impl->sessionState == XR_SESSION_STATE_READY) {
				XrSessionBeginInfo beginInfo = {};
				beginInfo.type = XR_TYPE_SESSION_BEGIN_INFO;
				beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
				xrBeginSession(_impl->session, &beginInfo);
				_impl->sessionRunning = true;
			} else if (_impl->sessionState == XR_SESSION_STATE_STOPPING) {
				xrEndSession(_impl->session);
				_impl->sessionRunning = false;
			}
		}

		eventData.type = XR_TYPE_EVENT_DATA_BUFFER;
	}
}

bool OpenXRSession::beginFrame(FrameState &state) {
	handleSessionStateChanges();

	if (!_impl->sessionRunning) {
		state.shouldRender = false;
		return false;
	}

	_impl->xrFrameState.type = XR_TYPE_FRAME_STATE;
	XrFrameWaitInfo waitInfo = {};
	waitInfo.type = XR_TYPE_FRAME_WAIT_INFO;

	XrResult result = xrWaitFrame(_impl->session, &waitInfo, &_impl->xrFrameState);
	if (XR_FAILED(result)) {
		LOGE("xrWaitFrame failed: %d", result);
		state.shouldRender = false;
		return false;
	}

	XrFrameBeginInfo beginInfo = {};
	beginInfo.type = XR_TYPE_FRAME_BEGIN_INFO;
	result = xrBeginFrame(_impl->session, &beginInfo);
	if (XR_FAILED(result)) {
		LOGE("xrBeginFrame failed: %d", result);
		state.shouldRender = false;
		return false;
	}

	state.shouldRender = _impl->xrFrameState.shouldRender;
	state.predictedDisplayTime = _impl->xrFrameState.predictedDisplayTime;

	if (state.shouldRender) {
		XrViewLocateInfo viewInfo = {};
		viewInfo.type = XR_TYPE_VIEW_LOCATE_INFO;
		viewInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
		viewInfo.displayTime = state.predictedDisplayTime;
		viewInfo.space = _impl->localSpace;

		XrViewState viewState = {};
		viewState.type = XR_TYPE_VIEW_STATE;
		uint32_t viewCount = 2;

		XrView views[2] = {};
		views[0].type = XR_TYPE_VIEW;
		views[1].type = XR_TYPE_VIEW;

		result = xrLocateViews(_impl->session, &viewInfo, &viewState, 2, &viewCount, views);
		if (XR_FAILED(result)) {
			LOGE("xrLocateViews failed: %d", result);
			state.shouldRender = false;
		} else {
			for (int eye = 0; eye < 2; eye++) {
				state.eyePose[eye][0] = views[eye].pose.orientation.x;
				state.eyePose[eye][1] = views[eye].pose.orientation.y;
				state.eyePose[eye][2] = views[eye].pose.orientation.z;
				state.eyePose[eye][3] = views[eye].pose.orientation.w;
				state.eyePose[eye][4] = views[eye].pose.position.x;
				state.eyePose[eye][5] = views[eye].pose.position.y;
				state.eyePose[eye][6] = views[eye].pose.position.z;
				state.eyeFov[eye][0] = views[eye].fov.angleLeft;
				state.eyeFov[eye][1] = views[eye].fov.angleRight;
				state.eyeFov[eye][2] = views[eye].fov.angleUp;
				state.eyeFov[eye][3] = views[eye].fov.angleDown;
			}
		}
	}

	return true;
}

GLuint OpenXRSession::acquireSwapchainImage(int eye) {
	XrSwapchainImageAcquireInfo acquireInfo = {};
	acquireInfo.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO;
	uint32_t imageIndex = 0;
	xrAcquireSwapchainImage(_impl->swapchains[eye], &acquireInfo, &imageIndex);

	XrSwapchainImageWaitInfo waitInfo = {};
	waitInfo.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO;
	waitInfo.timeout = XR_INFINITE_DURATION;
	xrWaitSwapchainImage(_impl->swapchains[eye], &waitInfo);

	return _impl->swapchainFBOs[eye][imageIndex];
}

void OpenXRSession::releaseSwapchainImage(int eye) {
	XrSwapchainImageReleaseInfo releaseInfo = {};
	releaseInfo.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO;
	xrReleaseSwapchainImage(_impl->swapchains[eye], &releaseInfo);
}

void OpenXRSession::endFrame(const FrameState &state) {
	// Reconstruct XrView data from our FrameState
	XrCompositionLayerProjectionView projViews[2] = {};
	XrCompositionLayerProjection layer = {};

	if (state.shouldRender) {
		for (int eye = 0; eye < 2; eye++) {
			projViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
			projViews[eye].pose.orientation.x = state.eyePose[eye][0];
			projViews[eye].pose.orientation.y = state.eyePose[eye][1];
			projViews[eye].pose.orientation.z = state.eyePose[eye][2];
			projViews[eye].pose.orientation.w = state.eyePose[eye][3];
			projViews[eye].pose.position.x = state.eyePose[eye][4];
			projViews[eye].pose.position.y = state.eyePose[eye][5];
			projViews[eye].pose.position.z = state.eyePose[eye][6];
			projViews[eye].fov.angleLeft = state.eyeFov[eye][0];
			projViews[eye].fov.angleRight = state.eyeFov[eye][1];
			projViews[eye].fov.angleUp = state.eyeFov[eye][2];
			projViews[eye].fov.angleDown = state.eyeFov[eye][3];
			projViews[eye].subImage.swapchain = _impl->swapchains[eye];
			projViews[eye].subImage.imageRect.offset = {0, 0};
			projViews[eye].subImage.imageRect.extent = {
				(int32_t)_impl->viewConfigs[eye].recommendedImageRectWidth,
				(int32_t)_impl->viewConfigs[eye].recommendedImageRectHeight
			};
			projViews[eye].subImage.imageArrayIndex = 0;
		}

		layer.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION;
		layer.space = _impl->localSpace;
		layer.viewCount = 2;
		layer.views = projViews;
	}

	const XrCompositionLayerBaseHeader *layers[] = {
		(const XrCompositionLayerBaseHeader *)&layer
	};

	XrFrameEndInfo endInfo = {};
	endInfo.type = XR_TYPE_FRAME_END_INFO;
	endInfo.displayTime = state.predictedDisplayTime;
	endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	endInfo.layerCount = state.shouldRender ? 1 : 0;
	endInfo.layers = state.shouldRender ? layers : nullptr;

	xrEndFrame(_impl->session, &endInfo);
}

#endif // USE_OPENXR
