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

#ifndef BACKENDS_PLATFORM_ANDROID_VR_OPENXR_INPUT_H
#define BACKENDS_PLATFORM_ANDROID_VR_OPENXR_INPUT_H

#ifdef USE_OPENXR

#include "common/scummsys.h"

struct OpenXRInputImpl;

class OpenXRInput {
public:
	enum Hand {
		HAND_LEFT = 0,
		HAND_RIGHT = 1,
		HAND_COUNT = 2
	};

	OpenXRInput();
	~OpenXRInput();

	// instanceHandle and sessionHandle are opaque XrInstance/XrSession cast to void*
	bool init(void *instanceHandle, void *sessionHandle);
	void shutdown();
	// referenceSpaceHandle is opaque XrSpace cast to void*
	void sync(uint64_t predictedTime, void *referenceSpaceHandle);

	// Controller pose as floats: [0-3] = orientation quat (x,y,z,w), [4-6] = position (x,y,z)
	void getControllerPose(int hand, float *poseOut) const;
	float getTriggerValue(int hand) const;
	float getGripValue(int hand) const;
	void getThumbstick(int hand, float *x, float *y) const;
	bool isMenuPressed() const;

	bool isTriggerClicked(int hand) const;
	bool isTriggerReleased(int hand) const;
	bool isGripClicked(int hand) const;
	bool isGripReleased(int hand) const;
	bool isMenuClicked() const;

private:
	OpenXRInputImpl *_impl;
};

#endif // USE_OPENXR
#endif // BACKENDS_PLATFORM_ANDROID_VR_OPENXR_INPUT_H
