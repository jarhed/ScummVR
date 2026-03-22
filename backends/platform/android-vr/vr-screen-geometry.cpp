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

#include "backends/platform/android-vr/vr-screen-geometry.h"
#include <cmath>
#include <cstring>

VRScreenGeometry::VRScreenGeometry()
	: _distance(2.5f),
	  _widthMeters(3.2f),
	  _heightMeters(1.8f),
	  _screenWidth(1920),
	  _screenHeight(1080),
	  _centerX(0.0f),
	  _centerY(0.0f),
	  _centerZ(-2.5f) {
}

void VRScreenGeometry::setScreenSize(uint width, uint height) {
	_screenWidth = width;
	_screenHeight = height;
}

void VRScreenGeometry::setDistance(float distance) {
	_distance = distance;
	_centerZ = -_distance;
}

void VRScreenGeometry::setPhysicalSize(float widthMeters, float heightMeters) {
	_widthMeters = widthMeters;
	_heightMeters = heightMeters;
}

bool VRScreenGeometry::rayIntersect(const float *pose, Common::Point &screenPos) const {
	// pose: [0-3] = quat (x,y,z,w), [4-6] = position (x,y,z)
	float qx = pose[0], qy = pose[1], qz = pose[2], qw = pose[3];
	float ox = pose[4], oy = pose[5], oz = pose[6];

	// Forward direction (negative Z in OpenXR convention)
	float dirX = 2.0f * (qx * qz + qw * qy);
	float dirY = 2.0f * (qy * qz - qw * qx);
	float dirZ = -(1.0f - 2.0f * (qx * qx + qy * qy));

	if (fabsf(dirZ) < 1e-6f)
		return false;

	float t = (_centerZ - oz) / dirZ;
	if (t < 0.0f)
		return false;

	float hitX = ox + t * dirX;
	float hitY = oy + t * dirY;

	float halfW = _widthMeters * 0.5f;
	float halfH = _heightMeters * 0.5f;

	float localX = hitX - _centerX;
	float localY = hitY - _centerY;

	if (localX < -halfW || localX > halfW || localY < -halfH || localY > halfH)
		return false;

	float u = (localX + halfW) / _widthMeters;
	float v = 1.0f - (localY + halfH) / _heightMeters;

	screenPos.x = (int)(u * _screenWidth);
	screenPos.y = (int)(v * _screenHeight);

	return true;
}

void VRScreenGeometry::getQuadVertices(float *vertices) const {
	float halfW = _widthMeters * 0.5f;
	float halfH = _heightMeters * 0.5f;

	// 4 vertices: position (x,y,z) + texcoord (u,v) = 5 floats each
	// Bottom-left
	vertices[0] = _centerX - halfW; vertices[1] = _centerY - halfH; vertices[2] = _centerZ;
	vertices[3] = 0.0f; vertices[4] = 1.0f;
	// Bottom-right
	vertices[5] = _centerX + halfW; vertices[6] = _centerY - halfH; vertices[7] = _centerZ;
	vertices[8] = 1.0f; vertices[9] = 1.0f;
	// Top-left
	vertices[10] = _centerX - halfW; vertices[11] = _centerY + halfH; vertices[12] = _centerZ;
	vertices[13] = 0.0f; vertices[14] = 0.0f;
	// Top-right
	vertices[15] = _centerX + halfW; vertices[16] = _centerY + halfH; vertices[17] = _centerZ;
	vertices[18] = 1.0f; vertices[19] = 0.0f;
}

void VRScreenGeometry::getModelMatrix(float *matrix) const {
	memset(matrix, 0, sizeof(float) * 16);
	matrix[0] = 1.0f;
	matrix[5] = 1.0f;
	matrix[10] = 1.0f;
	matrix[15] = 1.0f;
}

void VRScreenGeometry::poseToViewMatrix(const float *pose, float *matrix) {
	float qx = pose[0], qy = pose[1], qz = pose[2], qw = pose[3];
	float px = pose[4], py = pose[5], pz = pose[6];

	// Rotation matrix from quaternion (transposed = inverse rotation for view matrix)
	float r00 = 1.0f - 2.0f * (qy * qy + qz * qz);
	float r01 = 2.0f * (qx * qy + qw * qz);
	float r02 = 2.0f * (qx * qz - qw * qy);
	float r10 = 2.0f * (qx * qy - qw * qz);
	float r11 = 1.0f - 2.0f * (qx * qx + qz * qz);
	float r12 = 2.0f * (qy * qz + qw * qx);
	float r20 = 2.0f * (qx * qz + qw * qy);
	float r21 = 2.0f * (qy * qz - qw * qx);
	float r22 = 1.0f - 2.0f * (qx * qx + qy * qy);

	// Column-major for OpenGL
	matrix[0] = r00; matrix[4] = r10; matrix[8]  = r20; matrix[12] = -(r00 * px + r10 * py + r20 * pz);
	matrix[1] = r01; matrix[5] = r11; matrix[9]  = r21; matrix[13] = -(r01 * px + r11 * py + r21 * pz);
	matrix[2] = r02; matrix[6] = r12; matrix[10] = r22; matrix[14] = -(r02 * px + r12 * py + r22 * pz);
	matrix[3] = 0.0f; matrix[7] = 0.0f; matrix[11] = 0.0f; matrix[15] = 1.0f;
}

void VRScreenGeometry::fovToProjectionMatrix(const float *fov, float nearZ, float farZ, float *matrix) {
	float tanLeft = tanf(fov[0]);
	float tanRight = tanf(fov[1]);
	float tanUp = tanf(fov[2]);
	float tanDown = tanf(fov[3]);

	float tanWidth = tanRight - tanLeft;
	float tanHeight = tanUp - tanDown;

	memset(matrix, 0, sizeof(float) * 16);

	matrix[0] = 2.0f / tanWidth;
	matrix[5] = 2.0f / tanHeight;
	matrix[8] = (tanRight + tanLeft) / tanWidth;
	matrix[9] = (tanUp + tanDown) / tanHeight;
	matrix[10] = -(farZ + nearZ) / (farZ - nearZ);
	matrix[11] = -1.0f;
	matrix[14] = -(2.0f * farZ * nearZ) / (farZ - nearZ);
}

#endif // USE_OPENXR
