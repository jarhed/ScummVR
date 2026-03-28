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

#include "backends/platform/android-vr/diorama-data.h"
#include "common/system.h"
#include "graphics/paletteman.h"
#include "engines/engine.h"
#include "engines/scumm/scumm.h"
#include "engines/scumm/gfx.h"
#include "engines/scumm/boxes.h"
#include "engines/scumm/actor.h"

#include <android/log.h>
#include <cstdio>

#define LOG_TAG "ScummVM_VR"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

// g_dioramaState is defined in android-vr-main.cpp, declared extern in diorama-data.h
static uint32_t s_frameCounter = 0;
static uint8_t s_lastRoom = 255;

void dioramaExtract() {
	if (!g_dioramaState || !g_engine)
		return;

	// Check if we're running a SCUMM engine
	Scumm::ScummEngine *engine = dynamic_cast<Scumm::ScummEngine *>(g_engine);
	if (!engine)
		return;

	DioramaSnapshot *snap = g_dioramaState->getWriteBuffer();

	// Get VirtScreen — this is just the GAME area, not the verb panel
	Scumm::VirtScreen &vs = engine->_virtscr[Scumm::kMainVirtScreen];
	if (!vs.hasTwoBuffers)
		return;

	// Use the VirtScreen dimensions (game area only, excludes verb panel)
	uint16_t w = vs.w;
	uint16_t h = vs.h;
	if (w > DIORAMA_MAX_SCREEN_W) w = DIORAMA_MAX_SCREEN_W;
	if (h > DIORAMA_MAX_SCREEN_H) h = DIORAMA_MAX_SCREEN_H;
	if (w == 0 || h == 0)
		return;

	snap->screenWidth = w;
	snap->screenHeight = h;
	snap->roomWidth = engine->_roomWidth;
	snap->roomHeight = engine->_roomHeight;
	snap->currentRoom = engine->_currentRoom;
	snap->cameraX = engine->camera._cur.x;

	// Log on room change
	if (snap->currentRoom != s_lastRoom) {
		s_lastRoom = snap->currentRoom;
		int numBoxes = engine->getNumBoxes();
		LOGI("Diorama room %d: %dx%d game area, %d walk boxes, %d z-planes",
			snap->currentRoom, w, h, numBoxes, engine->_gdi->_numZBuffer);
	}

	// Get palette
	byte palette[3 * 256];
	g_system->getPaletteManager()->grabPalette(palette, 0, 256);

	// Extract background and composite (game area only)
	for (int y = 0; y < h; y++) {
		const byte *bgRow = (const byte *)vs.getBackPixels(0, y);
		const byte *fgRow = (const byte *)vs.getPixels(0, y);
		uint8_t *bgOut = &snap->backgroundRGBA[(y * w) * 4];
		uint8_t *fgOut = &snap->compositeRGBA[(y * w) * 4];
		for (int x = 0; x < w; x++) {
			byte bgIdx = bgRow[x];
			bgOut[x * 4 + 0] = palette[bgIdx * 3 + 0];
			bgOut[x * 4 + 1] = palette[bgIdx * 3 + 1];
			bgOut[x * 4 + 2] = palette[bgIdx * 3 + 2];
			bgOut[x * 4 + 3] = 255;

			byte fgIdx = fgRow[x];
			fgOut[x * 4 + 0] = palette[fgIdx * 3 + 0];
			fgOut[x * 4 + 1] = palette[fgIdx * 3 + 1];
			fgOut[x * 4 + 2] = palette[fgIdx * 3 + 2];
			// Mark actor pixels: alpha=255 where foreground differs from background
			fgOut[x * 4 + 3] = (bgIdx != fgIdx) ? 255 : 0;
		}
	}

	// Extract verb panel from the full composite screen (below the game area)
	{
		Scumm::VirtScreen &verbVs = engine->_virtscr[Scumm::kVerbVirtScreen];
		uint16_t vw = verbVs.w;
		uint16_t vh = verbVs.h;
		if (vw > DIORAMA_MAX_SCREEN_W) vw = DIORAMA_MAX_SCREEN_W;
		if (vh > 80) vh = 80;
		snap->verbWidth = vw;
		snap->verbHeight = vh;
		if (vw > 0 && vh > 0) {
			for (int y = 0; y < vh; y++) {
				const byte *row = (const byte *)verbVs.getPixels(0, y);
				uint8_t *out = &snap->verbRGBA[(y * vw) * 4];
				for (int x = 0; x < vw; x++) {
					byte idx = row[x];
					out[x * 4 + 0] = palette[idx * 3 + 0];
					out[x * 4 + 1] = palette[idx * 3 + 1];
					out[x * 4 + 2] = palette[idx * 3 + 2];
					out[x * 4 + 3] = 255;
				}
			}
		}
	}

	// Extract per-actor positions for billboard rendering
	// Use getObjectOrActorXY (public) to get actor positions
	{
		int actorCount = 0;
		// Actor IDs typically start at 1, max ~25 for SCUMM v6
		for (int id = 1; id < 30 && actorCount < 16; id++) {
			int ax, ay;
			if (engine->getObjectOrActorXY(id, ax, ay) == 0) {
				// Valid actor with position
				if (ax > 0 && ay > 0 && ax < snap->roomWidth && ay < snap->roomHeight) {
					snap->actors[actorCount].x = ax;
					snap->actors[actorCount].y = ay;
					// Approximate scale from Y position (higher Y = closer = bigger)
					float yFrac = (float)ay / (float)h;
					snap->actors[actorCount].scale = (uint8_t)(128 + yFrac * 127);
					snap->actors[actorCount].visible = true;
					actorCount++;
				}
			}
		}
		snap->numActors = actorCount;
	}

	// Walk boxes
	int numBoxes = engine->getNumBoxes();
	if (numBoxes > DIORAMA_MAX_BOXES) numBoxes = DIORAMA_MAX_BOXES;
	snap->numBoxes = numBoxes;
	uint8_t maxScale = 1;
	for (int i = 0; i < numBoxes; i++) {
		Scumm::BoxCoords bc = engine->getBoxCoordinates(i);
		snap->boxes[i].ulX = bc.ul.x; snap->boxes[i].ulY = bc.ul.y;
		snap->boxes[i].urX = bc.ur.x; snap->boxes[i].urY = bc.ur.y;
		snap->boxes[i].llX = bc.ll.x; snap->boxes[i].llY = bc.ll.y;
		snap->boxes[i].lrX = bc.lr.x; snap->boxes[i].lrY = bc.lr.y;
		snap->boxes[i].scale = engine->getBoxScale(i);
		if (snap->boxes[i].scale > maxScale)
			maxScale = snap->boxes[i].scale;
	}
	snap->avgFrontScale = maxScale;

	// Z-plane masked layers (skip for now if causing artifacts — just background + actors)
	int numZPlanes = engine->_gdi->_numZBuffer;
	if (numZPlanes > DIORAMA_MAX_ZPLANES) numZPlanes = DIORAMA_MAX_ZPLANES;
	snap->numZPlanes = numZPlanes;

	for (int z = 1; z < numZPlanes; z++) {
		uint8_t *out = snap->zplaneRGBA[z];
		bool hasAnyMasked = false;
		for (int y = 0; y < h; y++) {
			const byte *bgRow = (const byte *)vs.getBackPixels(0, y);
			for (int x = 0; x < w; x++) {
				// getMaskBuffer returns the byte containing the mask bit for pixel (x, y) at z-plane z
				byte *maskPtr = engine->getMaskBuffer(x, y, z);
				bool masked = false;
				if (maskPtr) {
					// The bit for pixel x within the byte is at position (x % 8), MSB first
					masked = (*maskPtr & (0x80 >> (x & 7))) != 0;
				}
				int idx = (y * w + x) * 4;
				if (masked) {
					byte bgIdx = bgRow[x];
					out[idx + 0] = palette[bgIdx * 3 + 0];
					out[idx + 1] = palette[bgIdx * 3 + 1];
					out[idx + 2] = palette[bgIdx * 3 + 2];
					out[idx + 3] = 255;
					hasAnyMasked = true;
				} else {
					out[idx + 0] = 0;
					out[idx + 1] = 0;
					out[idx + 2] = 0;
					out[idx + 3] = 0;
				}
			}
		}
	}

	// Draw cursor into composite texture
	{
		int cx = g_dioramaCursorX;
		int cy = g_dioramaCursorY;
		int size = 3; // cursor radius in pixels
		for (int dy = -size; dy <= size; dy++) {
			for (int dx = -size; dx <= size; dx++) {
				// Draw a crosshair shape
				if (dx != 0 && dy != 0) continue;
				int px = cx + dx;
				int py = cy + dy;
				if (px >= 0 && px < w && py >= 0 && py < h) {
					int idx = (py * w + px) * 4;
					snap->compositeRGBA[idx + 0] = 255;
					snap->compositeRGBA[idx + 1] = 255;
					snap->compositeRGBA[idx + 2] = 255;
					snap->compositeRGBA[idx + 3] = 255;
				}
			}
		}
	}

	snap->frameCounter = ++s_frameCounter;
	snap->valid = true;
	g_dioramaState->swapBuffers();

	// Debug: dump textures on first frame of each new room
	static uint8_t s_dumpedRoom = 255;
	if (snap->currentRoom != s_dumpedRoom) {
		s_dumpedRoom = snap->currentRoom;
		char path[256];

		// Write raw RGBA files that can be converted with:
		// convert -size WxH -depth 8 rgba:file.raw file.png
		snprintf(path, sizeof(path), "/sdcard/diorama_bg_room%d_%dx%d.raw",
			snap->currentRoom, w, h);
		FILE *f = fopen(path, "wb");
		if (f) {
			fwrite(snap->backgroundRGBA, w * h * 4, 1, f);
			fclose(f);
			LOGI("Dumped background to %s", path);
		}

		snprintf(path, sizeof(path), "/sdcard/diorama_actors_room%d_%dx%d.raw",
			snap->currentRoom, w, h);
		f = fopen(path, "wb");
		if (f) {
			fwrite(snap->compositeRGBA, w * h * 4, 1, f);
			fclose(f);
			LOGI("Dumped actors to %s", path);
		}

		for (int z = 1; z < numZPlanes; z++) {
			snprintf(path, sizeof(path), "/sdcard/diorama_zplane%d_room%d_%dx%d.raw",
				z, snap->currentRoom, w, h);
			f = fopen(path, "wb");
			if (f) {
				fwrite(snap->zplaneRGBA[z], w * h * 4, 1, f);
				fclose(f);
				LOGI("Dumped z-plane %d to %s", z, path);
			}
		}
	}
}

#endif // USE_OPENXR
