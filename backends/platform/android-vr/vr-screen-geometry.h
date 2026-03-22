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

#ifndef BACKENDS_PLATFORM_ANDROID_VR_SCREEN_GEOMETRY_H
#define BACKENDS_PLATFORM_ANDROID_VR_SCREEN_GEOMETRY_H

#ifdef USE_OPENXR

#include "common/scummsys.h"
#include "common/rect.h"

class VRScreenGeometry {
public:
	VRScreenGeometry();

	void setScreenSize(uint width, uint height);
	void setDistance(float distance);
	void setPhysicalSize(float widthMeters, float heightMeters);

	// pose is 7 floats: [0-3] = orientation quat (x,y,z,w), [4-6] = position (x,y,z)
	bool rayIntersect(const float *pose, Common::Point &screenPos) const;

	void getQuadVertices(float *vertices) const;
	void getModelMatrix(float *matrix) const;

	float getDistance() const { return _distance; }
	float getWidth() const { return _widthMeters; }
	float getHeight() const { return _heightMeters; }

	// pose is 7 floats: [0-3] = orientation quat (x,y,z,w), [4-6] = position (x,y,z)
	static void poseToViewMatrix(const float *pose, float *matrix);
	// fov is 4 floats: left, right, up, down angles in radians
	static void fovToProjectionMatrix(const float *fov, float nearZ, float farZ, float *matrix);

private:
	float _distance;
	float _widthMeters;
	float _heightMeters;
	uint _screenWidth;
	uint _screenHeight;

	float _centerX, _centerY, _centerZ;
};

#endif // USE_OPENXR
#endif // BACKENDS_PLATFORM_ANDROID_VR_SCREEN_GEOMETRY_H
