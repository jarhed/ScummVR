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

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <android/log.h>
#include <cstring>

#include "backends/platform/android-vr/openxr-input.h"

#define LOG_TAG "ScummVM_OpenXR"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static constexpr float CLICK_THRESHOLD = 0.5f;

struct OpenXRInputImpl {
	XrInstance instance;
	XrSession session;

	XrActionSet actionSet;
	XrAction poseAction;
	XrAction triggerAction;
	XrAction gripAction;
	XrAction thumbstickAction;
	XrAction menuAction;

	XrSpace handSpaces[2];
	XrPath handPaths[2];

	XrPosef handPoses[2];
	float triggerValues[2];
	float gripValues[2];
	XrVector2f thumbstickValues[2];
	bool menuPressed;

	float prevTriggerValues[2];
	float prevGripValues[2];
	bool prevMenuPressed;

	bool triggerClicked[2];
	bool triggerReleased[2];
	bool gripClicked[2];
	bool gripReleased[2];
	bool menuClicked;

	OpenXRInputImpl() : instance(XR_NULL_HANDLE), session(XR_NULL_HANDLE),
		actionSet(XR_NULL_HANDLE), poseAction(XR_NULL_HANDLE),
		triggerAction(XR_NULL_HANDLE), gripAction(XR_NULL_HANDLE),
		thumbstickAction(XR_NULL_HANDLE), menuAction(XR_NULL_HANDLE),
		menuPressed(false), prevMenuPressed(false), menuClicked(false) {
		memset(handSpaces, 0, sizeof(handSpaces));
		memset(handPaths, 0, sizeof(handPaths));
		memset(triggerValues, 0, sizeof(triggerValues));
		memset(gripValues, 0, sizeof(gripValues));
		memset(thumbstickValues, 0, sizeof(thumbstickValues));
		memset(prevTriggerValues, 0, sizeof(prevTriggerValues));
		memset(prevGripValues, 0, sizeof(prevGripValues));
		memset(triggerClicked, 0, sizeof(triggerClicked));
		memset(triggerReleased, 0, sizeof(triggerReleased));
		memset(gripClicked, 0, sizeof(gripClicked));
		memset(gripReleased, 0, sizeof(gripReleased));

		for (int h = 0; h < 2; h++) {
			handPoses[h].orientation.w = 1.0f;
		}
	}
};

OpenXRInput::OpenXRInput() : _impl(new OpenXRInputImpl()) {
}

OpenXRInput::~OpenXRInput() {
	shutdown();
	delete _impl;
}

bool OpenXRInput::init(void *instanceHandle, void *sessionHandle) {
	_impl->instance = (XrInstance)instanceHandle;
	_impl->session = (XrSession)sessionHandle;

	XrActionSetCreateInfo actionSetInfo = {};
	actionSetInfo.type = XR_TYPE_ACTION_SET_CREATE_INFO;
	strncpy(actionSetInfo.actionSetName, "scummvm", XR_MAX_ACTION_SET_NAME_SIZE);
	strncpy(actionSetInfo.localizedActionSetName, "ScummVM Controls", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE);

	if (XR_FAILED(xrCreateActionSet(_impl->instance, &actionSetInfo, &_impl->actionSet))) {
		LOGE("Failed to create action set");
		return false;
	}

	xrStringToPath(_impl->instance, "/user/hand/left", &_impl->handPaths[0]);
	xrStringToPath(_impl->instance, "/user/hand/right", &_impl->handPaths[1]);

	auto createAction = [&](XrAction &action, const char *name, const char *localName,
	                        XrActionType type, bool bothHands) -> bool {
		XrActionCreateInfo info = {};
		info.type = XR_TYPE_ACTION_CREATE_INFO;
		info.actionType = type;
		strncpy(info.actionName, name, XR_MAX_ACTION_NAME_SIZE);
		strncpy(info.localizedActionName, localName, XR_MAX_LOCALIZED_ACTION_NAME_SIZE);
		if (bothHands) {
			info.countSubactionPaths = 2;
			info.subactionPaths = _impl->handPaths;
		}
		return !XR_FAILED(xrCreateAction(_impl->actionSet, &info, &action));
	};

	if (!createAction(_impl->poseAction, "aim_pose", "Aim Pose", XR_ACTION_TYPE_POSE_INPUT, true))
		return false;
	if (!createAction(_impl->triggerAction, "trigger", "Trigger", XR_ACTION_TYPE_FLOAT_INPUT, true))
		return false;
	if (!createAction(_impl->gripAction, "grip", "Grip", XR_ACTION_TYPE_FLOAT_INPUT, true))
		return false;
	if (!createAction(_impl->thumbstickAction, "thumbstick", "Thumbstick", XR_ACTION_TYPE_VECTOR2F_INPUT, true))
		return false;
	if (!createAction(_impl->menuAction, "menu", "Menu", XR_ACTION_TYPE_BOOLEAN_INPUT, false))
		return false;

	XrPath oculusTouchProfile;
	xrStringToPath(_impl->instance, "/interaction_profiles/oculus/touch_controller", &oculusTouchProfile);

	auto pathFromStr = [&](const char *str) -> XrPath {
		XrPath path;
		xrStringToPath(_impl->instance, str, &path);
		return path;
	};

	XrActionSuggestedBinding bindings[] = {
		{_impl->poseAction, pathFromStr("/user/hand/left/input/aim/pose")},
		{_impl->poseAction, pathFromStr("/user/hand/right/input/aim/pose")},
		{_impl->triggerAction, pathFromStr("/user/hand/left/input/trigger/value")},
		{_impl->triggerAction, pathFromStr("/user/hand/right/input/trigger/value")},
		{_impl->gripAction, pathFromStr("/user/hand/left/input/squeeze/value")},
		{_impl->gripAction, pathFromStr("/user/hand/right/input/squeeze/value")},
		{_impl->thumbstickAction, pathFromStr("/user/hand/left/input/thumbstick")},
		{_impl->thumbstickAction, pathFromStr("/user/hand/right/input/thumbstick")},
		{_impl->menuAction, pathFromStr("/user/hand/left/input/menu/click")},
	};

	XrInteractionProfileSuggestedBinding suggestedBindings = {};
	suggestedBindings.type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING;
	suggestedBindings.interactionProfile = oculusTouchProfile;
	suggestedBindings.suggestedBindings = bindings;
	suggestedBindings.countSuggestedBindings = sizeof(bindings) / sizeof(bindings[0]);

	if (XR_FAILED(xrSuggestInteractionProfileBindings(_impl->instance, &suggestedBindings))) {
		LOGE("Failed to suggest interaction profile bindings");
		return false;
	}

	for (int hand = 0; hand < 2; hand++) {
		XrActionSpaceCreateInfo spaceInfo = {};
		spaceInfo.type = XR_TYPE_ACTION_SPACE_CREATE_INFO;
		spaceInfo.action = _impl->poseAction;
		spaceInfo.subactionPath = _impl->handPaths[hand];
		spaceInfo.poseInActionSpace.orientation.w = 1.0f;

		if (XR_FAILED(xrCreateActionSpace(_impl->session, &spaceInfo, &_impl->handSpaces[hand]))) {
			LOGE("Failed to create hand space for hand %d", hand);
			return false;
		}
	}

	XrSessionActionSetsAttachInfo attachInfo = {};
	attachInfo.type = XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO;
	attachInfo.countActionSets = 1;
	attachInfo.actionSets = &_impl->actionSet;

	if (XR_FAILED(xrAttachSessionActionSets(_impl->session, &attachInfo))) {
		LOGE("Failed to attach action sets");
		return false;
	}

	LOGI("OpenXR input initialized");
	return true;
}

void OpenXRInput::shutdown() {
	for (int h = 0; h < 2; h++) {
		if (_impl->handSpaces[h] != XR_NULL_HANDLE) {
			xrDestroySpace(_impl->handSpaces[h]);
			_impl->handSpaces[h] = XR_NULL_HANDLE;
		}
	}
	if (_impl->actionSet != XR_NULL_HANDLE) {
		xrDestroyActionSet(_impl->actionSet);
		_impl->actionSet = XR_NULL_HANDLE;
	}
}

void OpenXRInput::sync(uint64_t predictedTime, void *referenceSpaceHandle) {
	XrSpace referenceSpace = (XrSpace)referenceSpaceHandle;

	memcpy(_impl->prevTriggerValues, _impl->triggerValues, sizeof(_impl->triggerValues));
	memcpy(_impl->prevGripValues, _impl->gripValues, sizeof(_impl->gripValues));
	_impl->prevMenuPressed = _impl->menuPressed;

	XrActiveActionSet activeSet = {};
	activeSet.actionSet = _impl->actionSet;

	XrActionsSyncInfo syncInfo = {};
	syncInfo.type = XR_TYPE_ACTIONS_SYNC_INFO;
	syncInfo.countActiveActionSets = 1;
	syncInfo.activeActionSets = &activeSet;

	if (XR_FAILED(xrSyncActions(_impl->session, &syncInfo)))
		return;

	for (int hand = 0; hand < 2; hand++) {
		XrSpaceLocation location = {};
		location.type = XR_TYPE_SPACE_LOCATION;

		if (!XR_FAILED(xrLocateSpace(_impl->handSpaces[hand], referenceSpace, (XrTime)predictedTime, &location))) {
			if (location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) {
				_impl->handPoses[hand] = location.pose;
			}
		}

		XrActionStateGetInfo getInfo = {};
		getInfo.type = XR_TYPE_ACTION_STATE_GET_INFO;
		getInfo.subactionPath = _impl->handPaths[hand];

		getInfo.action = _impl->triggerAction;
		XrActionStateFloat floatState = {};
		floatState.type = XR_TYPE_ACTION_STATE_FLOAT;
		if (!XR_FAILED(xrGetActionStateFloat(_impl->session, &getInfo, &floatState)) && floatState.isActive)
			_impl->triggerValues[hand] = floatState.currentState;

		getInfo.action = _impl->gripAction;
		floatState.type = XR_TYPE_ACTION_STATE_FLOAT;
		if (!XR_FAILED(xrGetActionStateFloat(_impl->session, &getInfo, &floatState)) && floatState.isActive)
			_impl->gripValues[hand] = floatState.currentState;

		getInfo.action = _impl->thumbstickAction;
		XrActionStateVector2f vec2State = {};
		vec2State.type = XR_TYPE_ACTION_STATE_VECTOR2F;
		if (!XR_FAILED(xrGetActionStateVector2f(_impl->session, &getInfo, &vec2State)) && vec2State.isActive)
			_impl->thumbstickValues[hand] = vec2State.currentState;
	}

	XrActionStateGetInfo menuGetInfo = {};
	menuGetInfo.type = XR_TYPE_ACTION_STATE_GET_INFO;
	menuGetInfo.action = _impl->menuAction;
	XrActionStateBoolean boolState = {};
	boolState.type = XR_TYPE_ACTION_STATE_BOOLEAN;
	if (!XR_FAILED(xrGetActionStateBoolean(_impl->session, &menuGetInfo, &boolState)) && boolState.isActive)
		_impl->menuPressed = boolState.currentState;

	for (int hand = 0; hand < 2; hand++) {
		bool wasTriggered = _impl->prevTriggerValues[hand] >= CLICK_THRESHOLD;
		bool isTriggered = _impl->triggerValues[hand] >= CLICK_THRESHOLD;
		_impl->triggerClicked[hand] = !wasTriggered && isTriggered;
		_impl->triggerReleased[hand] = wasTriggered && !isTriggered;

		bool wasGripped = _impl->prevGripValues[hand] >= CLICK_THRESHOLD;
		bool isGripped = _impl->gripValues[hand] >= CLICK_THRESHOLD;
		_impl->gripClicked[hand] = !wasGripped && isGripped;
		_impl->gripReleased[hand] = wasGripped && !isGripped;
	}

	_impl->menuClicked = !_impl->prevMenuPressed && _impl->menuPressed;
}

void OpenXRInput::getControllerPose(int hand, float *poseOut) const {
	poseOut[0] = _impl->handPoses[hand].orientation.x;
	poseOut[1] = _impl->handPoses[hand].orientation.y;
	poseOut[2] = _impl->handPoses[hand].orientation.z;
	poseOut[3] = _impl->handPoses[hand].orientation.w;
	poseOut[4] = _impl->handPoses[hand].position.x;
	poseOut[5] = _impl->handPoses[hand].position.y;
	poseOut[6] = _impl->handPoses[hand].position.z;
}

float OpenXRInput::getTriggerValue(int hand) const { return _impl->triggerValues[hand]; }
float OpenXRInput::getGripValue(int hand) const { return _impl->gripValues[hand]; }

void OpenXRInput::getThumbstick(int hand, float *x, float *y) const {
	*x = _impl->thumbstickValues[hand].x;
	*y = _impl->thumbstickValues[hand].y;
}

bool OpenXRInput::isMenuPressed() const { return _impl->menuPressed; }
bool OpenXRInput::isTriggerClicked(int hand) const { return _impl->triggerClicked[hand]; }
bool OpenXRInput::isTriggerReleased(int hand) const { return _impl->triggerReleased[hand]; }
bool OpenXRInput::isGripClicked(int hand) const { return _impl->gripClicked[hand]; }
bool OpenXRInput::isGripReleased(int hand) const { return _impl->gripReleased[hand]; }
bool OpenXRInput::isMenuClicked() const { return _impl->menuClicked; }

#endif // USE_OPENXR
