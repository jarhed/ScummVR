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

#ifndef BACKENDS_PLATFORM_ANDROID_VR_DIORAMA_DATA_H
#define BACKENDS_PLATFORM_ANDROID_VR_DIORAMA_DATA_H

#ifdef USE_OPENXR

#include <stdint.h>
#include <string.h>

// Diorama dimensions in meters
#define DIORAMA_WIDTH    2.0f
#define DIORAMA_HEIGHT   1.5f
#define DIORAMA_DEPTH    1.0f
#define DIORAMA_DISTANCE 1.5f
#define DIORAMA_CENTER_Y -0.3f

#define DIORAMA_MAX_BOXES 32
#define DIORAMA_MAX_ZPLANES 8
#define DIORAMA_MAX_SCREEN_W 640
#define DIORAMA_MAX_SCREEN_H 400

struct DioramaWalkBox {
	int16_t ulX, ulY, urX, urY;
	int16_t llX, llY, lrX, lrY;
	uint8_t scale; // 1=far, 255=near
};

struct DioramaSnapshot {
	// Room metadata
	uint16_t roomWidth, roomHeight;
	uint16_t screenWidth, screenHeight;
	int16_t cameraX;
	uint8_t currentRoom;
	uint32_t frameCounter;

	// Background: RGBA pixels (clean background without actors)
	uint8_t backgroundRGBA[DIORAMA_MAX_SCREEN_W * DIORAMA_MAX_SCREEN_H * 4];

	// Composite: RGBA pixels (with actors, for diff)
	uint8_t compositeRGBA[DIORAMA_MAX_SCREEN_W * DIORAMA_MAX_SCREEN_H * 4];

	// Walk boxes
	uint8_t numBoxes;
	DioramaWalkBox boxes[DIORAMA_MAX_BOXES];

	// Verb panel: RGBA pixels for the UI area below the game
	uint16_t verbWidth, verbHeight;
	uint8_t verbRGBA[DIORAMA_MAX_SCREEN_W * 80 * 4]; // verb panel is ~56px tall max

	// Average scale for depth mapping of the actor diff layer
	uint8_t avgFrontScale;

	// Z-plane masks: 1 byte per pixel, value = mask for that plane
	uint8_t numZPlanes;
	// For each z-plane, store an RGBA texture (background pixel where masked, else transparent)
	// These are generated on the ScummVM thread to avoid the VR thread needing palette access
	uint8_t zplaneRGBA[DIORAMA_MAX_ZPLANES][DIORAMA_MAX_SCREEN_W * DIORAMA_MAX_SCREEN_H * 4];

	bool valid;
};

// Double-buffered shared state between ScummVM and VR threads
struct DioramaSharedState {
	DioramaSnapshot snapshots[2];
	volatile int readIndex;
	volatile int writeIndex;
	volatile bool dirty;

	void init() {
		readIndex = 0;
		writeIndex = 1;
		dirty = false;
		snapshots[0].valid = false;
		snapshots[1].valid = false;
		snapshots[0].frameCounter = 0;
		snapshots[1].frameCounter = 0;
	}

	DioramaSnapshot *getWriteBuffer() { return &snapshots[writeIndex]; }
	const DioramaSnapshot *getReadBuffer() { return &snapshots[readIndex]; }

	void swapBuffers() {
		__sync_synchronize(); // memory barrier
		int w = writeIndex;
		int r = readIndex;
		writeIndex = r;
		readIndex = w;
		dirty = true;
		__sync_synchronize(); // memory barrier
	}
};

// Convert walk box scale to diorama Z depth
inline float scaleToDepth(uint8_t scale) {
	return -DIORAMA_DEPTH * (1.0f - ((float)(scale - 1)) / 254.0f);
}

// Convert game X to diorama X
inline float gameXToDiorama(int16_t gameX, uint16_t screenWidth) {
	return ((float)gameX / (float)screenWidth - 0.5f) * DIORAMA_WIDTH;
}

// Convert game Y to diorama Y (actors stand on the floor at Y=0, scaled by their depth)
inline float gameYToDioramaHeight(int16_t gameY, uint16_t screenHeight, uint8_t scale) {
	// In SCUMM, Y increases downward. Higher Y = closer to camera.
	// The actor height in the diorama should scale with their walk box scale.
	float normalizedScale = (float)scale / 255.0f;
	return normalizedScale * DIORAMA_HEIGHT * 0.5f; // half the back wall height
}

// Cursor position for drawing in diorama (set by VR input, read by extractor)
extern int g_dioramaCursorX;
extern int g_dioramaCursorY;

// Global shared state pointer (set by android_main, read by graphics manager)
extern DioramaSharedState *g_dioramaState;

#endif // USE_OPENXR
#endif // BACKENDS_PLATFORM_ANDROID_VR_DIORAMA_DATA_H
