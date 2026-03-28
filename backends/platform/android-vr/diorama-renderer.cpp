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

#include <GLES3/gl3.h>
#include <android/log.h>
#include <cstring>
#include <cmath>
#include <cstdio>

#define LOG_TAG "ScummVR_Diorama"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// Shaders
static const char *kDioramaVert =
	"#version 300 es\n"
	"layout(location=0) in vec3 pos;\n"
	"layout(location=1) in vec2 uv;\n"
	"uniform mat4 mvp;\n"
	"out vec2 vUV;\n"
	"void main() { gl_Position = mvp * vec4(pos, 1.0); vUV = uv; }\n";

static const char *kDioramaFrag =
	"#version 300 es\n"
	"precision mediump float;\n"
	"in vec2 vUV;\n"
	"uniform sampler2D tex;\n"
	"out vec4 color;\n"
	"void main() { color = texture(tex, vUV); }\n";

static const char *kDioramaAlphaFrag =
	"#version 300 es\n"
	"precision mediump float;\n"
	"in vec2 vUV;\n"
	"uniform sampler2D tex;\n"
	"out vec4 color;\n"
	"void main() {\n"
	"    vec4 c = texture(tex, vUV);\n"
	"    if (c.a < 0.5) discard;\n"
	"    color = c;\n"
	"}\n";

// Depth mesh grid resolution
#define DEPTH_GRID_W 64
#define DEPTH_GRID_H 32

struct DioramaRenderer {
	GLuint opaqueProgram;
	GLuint alphaProgram;
	GLint opaqueMVPLoc, opaqueTexLoc;
	GLint alphaMVPLoc, alphaTexLoc;

	GLuint backgroundTex;
	GLuint compositeTex;    // actor diff layer
	GLuint verbTex;         // verb/inventory panel
	GLuint zplaneTex[DIORAMA_MAX_ZPLANES];

	GLuint quadVBO; // generic unit quad, transformed via MVP
	GLuint floorVBO;
	int floorVertCount;

	// Depth displacement mesh
	GLuint depthMeshVBO;
	GLuint depthMeshIBO;
	int depthMeshIndexCount;
	bool hasDepthMap;
	uint16_t depthW, depthH;
	uint8_t depthData[800 * 200]; // depth map data (max room size)

	uint8_t lastRoom;
	uint32_t lastFrame;
	int16_t lastCameraX;
	bool initialized;
};

static DioramaRenderer g_diorama = {};

static GLuint compileShader(GLenum type, const char *src) {
	GLuint s = glCreateShader(type);
	glShaderSource(s, 1, &src, nullptr);
	glCompileShader(s);
	GLint ok;
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[256];
		glGetShaderInfoLog(s, 256, nullptr, log);
		LOGE("Shader: %s", log);
	}
	return s;
}

static GLuint createProgram(const char *vertSrc, const char *fragSrc) {
	GLuint vs = compileShader(GL_VERTEX_SHADER, vertSrc);
	GLuint fs = compileShader(GL_FRAGMENT_SHADER, fragSrc);
	GLuint prog = glCreateProgram();
	glAttachShader(prog, vs);
	glAttachShader(prog, fs);
	glLinkProgram(prog);
	glDeleteShader(vs);
	glDeleteShader(fs);
	return prog;
}

static GLuint createTexture() {
	GLuint tex;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST); // pixel art!
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	return tex;
}

static void mat4_identity(float *m) {
	memset(m, 0, 64);
	m[0] = m[5] = m[10] = m[15] = 1;
}

static void mat4_multiply(float *out, const float *a, const float *b) {
	float t[16];
	for (int i = 0; i < 4; i++)
		for (int j = 0; j < 4; j++) {
			t[i * 4 + j] = 0;
			for (int k = 0; k < 4; k++)
				t[i * 4 + j] += a[k * 4 + j] * b[i * 4 + k];
		}
	memcpy(out, t, 64);
}

// Translate a 4x4 matrix (column-major)
static void mat4_translate(float *m, float tx, float ty, float tz) {
	float t[16];
	mat4_identity(t);
	t[12] = tx;
	t[13] = ty;
	t[14] = tz;
	float r[16];
	mat4_multiply(r, m, t);
	memcpy(m, r, 64);
}

// Scale a 4x4 matrix
static void mat4_scale(float *m, float sx, float sy, float sz) {
	float t[16];
	mat4_identity(t);
	t[0] = sx;
	t[5] = sy;
	t[10] = sz;
	float r[16];
	mat4_multiply(r, m, t);
	memcpy(m, r, 64);
}

void dioramaRendererInit() {
	if (g_diorama.initialized) return;

	g_diorama.opaqueProgram = createProgram(kDioramaVert, kDioramaFrag);
	g_diorama.opaqueMVPLoc = glGetUniformLocation(g_diorama.opaqueProgram, "mvp");
	g_diorama.opaqueTexLoc = glGetUniformLocation(g_diorama.opaqueProgram, "tex");

	g_diorama.alphaProgram = createProgram(kDioramaVert, kDioramaAlphaFrag);
	g_diorama.alphaMVPLoc = glGetUniformLocation(g_diorama.alphaProgram, "mvp");
	g_diorama.alphaTexLoc = glGetUniformLocation(g_diorama.alphaProgram, "tex");

	// Unit quad: (0,0) to (1,1) in XY, textured
	float quad[] = {
		0, 0, 0, 0, 1,
		1, 0, 0, 1, 1,
		0, 1, 0, 0, 0,
		1, 1, 0, 1, 0,
	};
	glGenBuffers(1, &g_diorama.quadVBO);
	glBindBuffer(GL_ARRAY_BUFFER, g_diorama.quadVBO);
	glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
	glBindBuffer(GL_ARRAY_BUFFER, 0);

	glGenBuffers(1, &g_diorama.floorVBO);
	g_diorama.floorVertCount = 0;
	glGenBuffers(1, &g_diorama.depthMeshVBO);
	glGenBuffers(1, &g_diorama.depthMeshIBO);
	g_diorama.depthMeshIndexCount = 0;
	g_diorama.hasDepthMap = false;

	g_diorama.backgroundTex = createTexture();
	g_diorama.compositeTex = createTexture();
	g_diorama.verbTex = createTexture();
	for (int i = 0; i < DIORAMA_MAX_ZPLANES; i++)
		g_diorama.zplaneTex[i] = createTexture();

	g_diorama.lastRoom = 255;
	g_diorama.lastFrame = 0;
	g_diorama.initialized = true;

	LOGI("Diorama renderer initialized");
}

static void drawQuad(GLuint program, GLint mvpLoc, GLint texLoc,
                     const float *viewProj, const float *model, GLuint texture) {
	float mvp[16];
	mat4_multiply(mvp, viewProj, model);

	glUseProgram(program);
	glUniformMatrix4fv(mvpLoc, 1, GL_FALSE, mvp);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, texture);
	glUniform1i(texLoc, 0);

	glBindBuffer(GL_ARRAY_BUFFER, g_diorama.quadVBO);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 20, (void *)0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 20, (void *)12);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glDisableVertexAttribArray(0);
	glDisableVertexAttribArray(1);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glUseProgram(0);
}

bool dioramaHasData() {
	if (!g_dioramaState) return false;
	const DioramaSnapshot *snap = g_dioramaState->getReadBuffer();
	return snap->valid && snap->screenWidth > 0 && snap->screenHeight > 0;
}

// Load depth map from file and build displacement mesh
static void loadDepthMap(const char *path) {
	FILE *f = fopen(path, "rb");
	if (!f) {
		LOGI("No depth map at %s", path);
		g_diorama.hasDepthMap = false;
		return;
	}

	uint16_t dw, dh;
	fread(&dw, 2, 1, f);
	fread(&dh, 2, 1, f);
	if (dw == 0 || dh == 0 || dw > 800 || dh > 200 || (int)dw * dh > 800 * 200) {
		LOGE("Bad depth map size: %dx%d", dw, dh);
		fclose(f);
		g_diorama.hasDepthMap = false;
		return;
	}

	fread(g_diorama.depthData, dw * dh, 1, f);
	fclose(f);
	g_diorama.depthW = dw;
	g_diorama.depthH = dh;
	LOGI("Loaded depth map %dx%d from %s", dw, dh);

	// Build displacement mesh: a grid of vertices with Z displaced by depth
	int gw = DEPTH_GRID_W, gh = DEPTH_GRID_H;
	int numVerts = (gw + 1) * (gh + 1);
	int numIndices = gw * gh * 6;
	float *verts = new float[numVerts * 5]; // xyz + uv
	uint16_t *indices = new uint16_t[numIndices];

	float hw = DIORAMA_WIDTH / 2.0f;
	float aspect = (float)dw / (float)dh;
	float dioH = DIORAMA_WIDTH / aspect;
	float maxDisplace = DIORAMA_DEPTH * 1.2f; // strong forward displacement for dramatic 3D

	// Camera scroll offset — the depth map covers the full room width,
	// but the background texture only shows the visible viewport.
	// We need to sample the depth map at the camera-scrolled position.
	float scrollU = 0.0f;
	float viewportFrac = 1.0f; // what fraction of the full room is visible
	if (g_dioramaState) {
		const DioramaSnapshot *s = g_dioramaState->getReadBuffer();
		if (s->valid && s->roomWidth > s->screenWidth) {
			scrollU = (float)s->cameraX / (float)s->roomWidth;
			viewportFrac = (float)s->screenWidth / (float)s->roomWidth;
			// Adjust scroll — cameraX is the center of the viewport
			scrollU = ((float)s->cameraX - s->screenWidth / 2.0f) / (float)s->roomWidth;
			if (scrollU < 0) scrollU = 0;
		}
	}

	int vi = 0;
	for (int y = 0; y <= gh; y++) {
		for (int x = 0; x <= gw; x++) {
			float u = (float)x / (float)gw; // 0-1 across the viewport
			float v = (float)y / (float)gh;

			// Sample depth map at the scrolled position
			// Map viewport u (0-1) to full room depth map coordinate
			float depthU = scrollU + u * viewportFrac;
			int dx = (int)(depthU * (dw - 1));
			int dy = (int)(v * (dh - 1));
			if (dx < 0) dx = 0;
			if (dx >= dw) dx = dw - 1;
			if (dy >= dh) dy = dh - 1;
			float depth = (float)g_diorama.depthData[dy * dw + dx] / 255.0f;

			// World position: back wall + forward displacement based on depth
			// depth 0 = far (black in depth map) = stays on back wall
			// depth 1 = near (white) = pushed forward
			float wx = -hw + u * DIORAMA_WIDTH;
			float wy = DIORAMA_CENTER_Y + (1.0f - v) * dioH;
			float wz = -DIORAMA_DISTANCE - DIORAMA_DEPTH + depth * maxDisplace;

			verts[vi++] = wx;
			verts[vi++] = wy;
			verts[vi++] = wz;
			verts[vi++] = u;
			verts[vi++] = v;
		}
	}

	int ii = 0;
	for (int y = 0; y < gh; y++) {
		for (int x = 0; x < gw; x++) {
			int tl = y * (gw + 1) + x;
			int tr = tl + 1;
			int bl = (y + 1) * (gw + 1) + x;
			int br = bl + 1;
			indices[ii++] = tl; indices[ii++] = bl; indices[ii++] = tr;
			indices[ii++] = tr; indices[ii++] = bl; indices[ii++] = br;
		}
	}

	glBindBuffer(GL_ARRAY_BUFFER, g_diorama.depthMeshVBO);
	glBufferData(GL_ARRAY_BUFFER, numVerts * 5 * sizeof(float), verts, GL_STATIC_DRAW);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_diorama.depthMeshIBO);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, numIndices * sizeof(uint16_t), indices, GL_STATIC_DRAW);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

	g_diorama.depthMeshIndexCount = numIndices;
	g_diorama.hasDepthMap = true;

	delete[] verts;
	delete[] indices;

	LOGI("Built depth mesh: %d verts, %d indices", numVerts, numIndices);
}

// Build 3D floor geometry from walk boxes
static void buildFloorGeometry(const DioramaSnapshot *snap) {
	if (snap->numBoxes == 0) {
		g_diorama.floorVertCount = 0;
		return;
	}

	float w = (float)snap->screenWidth;
	float h = (float)snap->screenHeight;
	float hw = DIORAMA_WIDTH / 2.0f;
	float aspect = w / h;
	float dioHeight = DIORAMA_WIDTH / aspect;

	// Find Y range of walk boxes for depth mapping
	float minY = 9999, maxY = -9999;
	for (int i = 0; i < snap->numBoxes; i++) {
		float ys[] = { (float)snap->boxes[i].ulY, (float)snap->boxes[i].urY,
		               (float)snap->boxes[i].llY, (float)snap->boxes[i].lrY };
		for (int j = 0; j < 4; j++) {
			if (ys[j] < minY) minY = ys[j];
			if (ys[j] > maxY) maxY = ys[j];
		}
	}
	float yRange = maxY - minY;
	if (yRange < 1.0f) yRange = 1.0f;

	// Each walk box = 2 triangles = 6 vertices, 5 floats each (pos xyz + uv)
	int maxVerts = snap->numBoxes * 6;
	float *verts = new float[maxVerts * 5];
	int vi = 0;

	auto addVert = [&](float gameX, float gameY) {
		// X: map game X to diorama X
		float x = (gameX / w - 0.5f) * DIORAMA_WIDTH;
		// Z: map game Y to depth (higher game Y = closer = less negative Z)
		float zNorm = (gameY - minY) / yRange; // 0=back, 1=front
		float z = -DIORAMA_DISTANCE - DIORAMA_DEPTH + zNorm * DIORAMA_DEPTH;
		// Y: floor level
		float y = DIORAMA_CENTER_Y;
		// UV: project background texture
		float u = gameX / w;
		float v = gameY / h;

		verts[vi++] = x;
		verts[vi++] = y;
		verts[vi++] = z;
		verts[vi++] = u;
		verts[vi++] = v;
	};

	for (int i = 0; i < snap->numBoxes; i++) {
		const DioramaWalkBox &box = snap->boxes[i];
		// Triangle 1: ul, ur, ll
		addVert(box.ulX, box.ulY);
		addVert(box.urX, box.urY);
		addVert(box.llX, box.llY);
		// Triangle 2: ur, lr, ll
		addVert(box.urX, box.urY);
		addVert(box.lrX, box.lrY);
		addVert(box.llX, box.llY);
	}

	g_diorama.floorVertCount = vi / 5;
	glBindBuffer(GL_ARRAY_BUFFER, g_diorama.floorVBO);
	glBufferData(GL_ARRAY_BUFFER, vi * sizeof(float), verts, GL_DYNAMIC_DRAW);
	glBindBuffer(GL_ARRAY_BUFFER, 0);

	delete[] verts;
}

void dioramaRendererDraw(const float *viewProj) {
	if (!g_diorama.initialized || !g_dioramaState)
		return;

	const DioramaSnapshot *snap = g_dioramaState->getReadBuffer();
	if (!snap->valid || snap->screenWidth == 0 || snap->screenHeight == 0)
		return;

	// Update textures if new frame
	if (snap->frameCounter != g_diorama.lastFrame) {
		g_diorama.lastFrame = snap->frameCounter;
		int w = snap->screenWidth;
		int h = snap->screenHeight;

		// Background texture
		glBindTexture(GL_TEXTURE_2D, g_diorama.backgroundTex);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
			snap->backgroundRGBA);

		// Actor diff texture (composite with alpha where actors are)
		glBindTexture(GL_TEXTURE_2D, g_diorama.compositeTex);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
			snap->compositeRGBA);

		// Verb panel texture
		if (snap->verbWidth > 0 && snap->verbHeight > 0) {
			glBindTexture(GL_TEXTURE_2D, g_diorama.verbTex);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, snap->verbWidth, snap->verbHeight,
				0, GL_RGBA, GL_UNSIGNED_BYTE, snap->verbRGBA);
		}

		// Z-plane textures
		for (int z = 1; z < snap->numZPlanes && z < DIORAMA_MAX_ZPLANES; z++) {
			glBindTexture(GL_TEXTURE_2D, g_diorama.zplaneTex[z]);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
				snap->zplaneRGBA[z]);
		}

		// Rebuild depth mesh when camera scrolls (depth map stays loaded, just rebuild mesh)
		if (g_diorama.hasDepthMap && snap->cameraX != g_diorama.lastCameraX) {
			g_diorama.lastCameraX = snap->cameraX;
			// Rebuild mesh with same depth data but new camera offset
			// Reuse loadDepthMap's mesh building by calling it with a dummy rebuild
			uint16_t dw = g_diorama.depthW, dh = g_diorama.depthH;
			if (dw > 0 && dh > 0) {
				// Re-run mesh generation (the scroll offset is read from g_dioramaState inside)
				int gw = DEPTH_GRID_W, gh = DEPTH_GRID_H;
				int numVerts = (gw + 1) * (gh + 1);
				int numIndices = gw * gh * 6;
				float *verts = new float[numVerts * 5];
				uint16_t *indices = new uint16_t[numIndices];

				float hw2 = DIORAMA_WIDTH / 2.0f;
				float aspect2 = (float)dw / (float)dh;
				float dioH2 = DIORAMA_WIDTH / aspect2;
				float maxDisp = DIORAMA_DEPTH * 1.2f;

				float scrollU2 = 0, vpFrac2 = 1;
				if (snap->roomWidth > snap->screenWidth) {
					scrollU2 = ((float)snap->cameraX - snap->screenWidth / 2.0f) / (float)snap->roomWidth;
					if (scrollU2 < 0) scrollU2 = 0;
					vpFrac2 = (float)snap->screenWidth / (float)snap->roomWidth;
				}

				int vi2 = 0;
				for (int y = 0; y <= gh; y++) {
					for (int x = 0; x <= gw; x++) {
						float u = (float)x / (float)gw;
						float v = (float)y / (float)gh;
						float dU = scrollU2 + u * vpFrac2;
						int ddx = (int)(dU * (dw - 1));
						int ddy = (int)(v * (dh - 1));
						if (ddx < 0) ddx = 0;
						if (ddx >= dw) ddx = dw - 1;
						if (ddy >= dh) ddy = dh - 1;
						float d = (float)g_diorama.depthData[ddy * dw + ddx] / 255.0f;
						verts[vi2++] = -hw2 + u * DIORAMA_WIDTH;
						verts[vi2++] = DIORAMA_CENTER_Y + (1.0f - v) * dioH2;
						verts[vi2++] = -DIORAMA_DISTANCE - DIORAMA_DEPTH + d * maxDisp;
						verts[vi2++] = u;
						verts[vi2++] = v;
					}
				}
				int ii2 = 0;
				for (int y = 0; y < gh; y++) {
					for (int x = 0; x < gw; x++) {
						int tl = y * (gw + 1) + x;
						indices[ii2++] = tl; indices[ii2++] = tl + gw + 1; indices[ii2++] = tl + 1;
						indices[ii2++] = tl + 1; indices[ii2++] = tl + gw + 1; indices[ii2++] = tl + gw + 2;
					}
				}
				glBindBuffer(GL_ARRAY_BUFFER, g_diorama.depthMeshVBO);
				glBufferData(GL_ARRAY_BUFFER, numVerts * 5 * sizeof(float), verts, GL_DYNAMIC_DRAW);
				glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_diorama.depthMeshIBO);
				glBufferData(GL_ELEMENT_ARRAY_BUFFER, numIndices * sizeof(uint16_t), indices, GL_DYNAMIC_DRAW);
				glBindBuffer(GL_ARRAY_BUFFER, 0);
				glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
				g_diorama.depthMeshIndexCount = numIndices;
				delete[] verts;
				delete[] indices;
			}
		}

		if (snap->currentRoom != g_diorama.lastRoom) {
			g_diorama.lastRoom = snap->currentRoom;
			g_diorama.lastCameraX = snap->cameraX;
			buildFloorGeometry(snap);
			// Try to load a depth map for this room
			// Check app data dir first, then sdcard
			const char *dirs[] = {
				"/data/user/0/org.scummvm.scummvm.vr.debug/files",
				"/sdcard/scummvm_games/dott",
				nullptr
			};
			g_diorama.hasDepthMap = false;
			for (const char **dir = dirs; *dir && !g_diorama.hasDepthMap; dir++) {
				char depthPath[256];
				snprintf(depthPath, sizeof(depthPath), "%s/dott_room%d_depth.bin",
					*dir, snap->currentRoom);
				loadDepthMap(depthPath);
			}
			LOGI("Diorama room %d: %d boxes, depth=%s",
				snap->currentRoom, snap->numBoxes,
				g_diorama.hasDepthMap ? "YES" : "no");
		}
	}

	// Calculate aspect-correct diorama dimensions from actual game area
	float aspect = (float)snap->screenWidth / (float)snap->screenHeight;
	float dioWidth = DIORAMA_WIDTH;
	float dioHeight = dioWidth / aspect;
	float hw = dioWidth / 2.0f;

	// Auto-orient: compute viewing angle from head (origin) to diorama center
	float dcY = DIORAMA_CENTER_Y + dioHeight * 0.5f;
	float dcZ = -DIORAMA_DISTANCE - DIORAMA_DEPTH * 0.5f;
	float distXZ = sqrtf(dcZ * dcZ); // horizontal distance
	float autoTiltX = atan2f(-dcY, distXZ); // look down at the diorama

	float dioBase[16];
	mat4_identity(dioBase);
	float pivotY = dcY;
	float pivotZ = dcZ;
	mat4_translate(dioBase, 0, pivotY, pivotZ);
	// X tilt: auto + manual grab adjustment
	{
		float angleX = autoTiltX + g_dioramaRotX;
		float c = cosf(angleX), s = sinf(angleX);
		float rot[16];
		mat4_identity(rot);
		rot[5] = c; rot[6] = s;
		rot[9] = -s; rot[10] = c;
		float tmp[16];
		mat4_multiply(tmp, dioBase, rot);
		memcpy(dioBase, tmp, 64);
	}
	// Y rotation: manual grab only
	if (g_dioramaRotY != 0.0f) {
		float c = cosf(g_dioramaRotY), s = sinf(g_dioramaRotY);
		float rot[16];
		mat4_identity(rot);
		rot[0] = c; rot[2] = -s;
		rot[8] = s; rot[10] = c;
		float tmp[16];
		mat4_multiply(tmp, dioBase, rot);
		memcpy(dioBase, tmp, 64);
	}
	mat4_translate(dioBase, 0, -pivotY, -pivotZ);

	float dioVP[16];
	mat4_multiply(dioVP, viewProj, dioBase);

	// === Background: depth-displaced mesh or flat back wall ===
	if (g_diorama.hasDepthMap && g_diorama.depthMeshIndexCount > 0) {
		// Render depth-displaced mesh
		glUseProgram(g_diorama.opaqueProgram);
		glUniformMatrix4fv(g_diorama.opaqueMVPLoc, 1, GL_FALSE, dioVP);
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, g_diorama.backgroundTex);
		glUniform1i(g_diorama.opaqueTexLoc, 0);

		glBindBuffer(GL_ARRAY_BUFFER, g_diorama.depthMeshVBO);
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_diorama.depthMeshIBO);
		glEnableVertexAttribArray(0);
		glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 20, (void *)0);
		glEnableVertexAttribArray(1);
		glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 20, (void *)12);
		glDrawElements(GL_TRIANGLES, g_diorama.depthMeshIndexCount, GL_UNSIGNED_SHORT, 0);
		glDisableVertexAttribArray(0);
		glDisableVertexAttribArray(1);
		glBindBuffer(GL_ARRAY_BUFFER, 0);
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
		glUseProgram(0);
	} else {
		// Fallback: flat back wall
		float model[16];
		mat4_identity(model);
		mat4_translate(model, -hw, DIORAMA_CENTER_Y, -DIORAMA_DISTANCE - DIORAMA_DEPTH);
		mat4_scale(model, dioWidth, dioHeight, 1.0f);
		drawQuad(g_diorama.opaqueProgram, g_diorama.opaqueMVPLoc, g_diorama.opaqueTexLoc,
			dioVP, model, g_diorama.backgroundTex);
	}

	// (Walk box floor geometry disabled — replaced by angled floor above)
	if (false && g_diorama.floorVertCount > 0) {
		glUseProgram(g_diorama.opaqueProgram);
		glUniformMatrix4fv(g_diorama.opaqueMVPLoc, 1, GL_FALSE, dioVP);
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, g_diorama.backgroundTex);
		glUniform1i(g_diorama.opaqueTexLoc, 0);

		glBindBuffer(GL_ARRAY_BUFFER, g_diorama.floorVBO);
		glEnableVertexAttribArray(0);
		glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 20, (void *)0);
		glEnableVertexAttribArray(1);
		glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 20, (void *)12);
		glDrawArrays(GL_TRIANGLES, 0, g_diorama.floorVertCount);
		glDisableVertexAttribArray(0);
		glDisableVertexAttribArray(1);
		glBindBuffer(GL_ARRAY_BUFFER, 0);
		glUseProgram(0);
	}

	// === Z-plane foreground layers ===
	// TODO: Z-planes cause artifacts — disabled until mask extraction is fixed
	// (doubled elements, black triangles from garbage mask data)

	// === Actor/foreground diff layer ===
	if (g_diorama.hasDepthMap && g_diorama.depthMeshIndexCount > 0) {
		// Render actors ON the depth mesh — second pass with alpha testing
		// This places actors at the correct 3D depth within the room
		glUseProgram(g_diorama.alphaProgram);
		glUniformMatrix4fv(g_diorama.alphaMVPLoc, 1, GL_FALSE, dioVP);
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, g_diorama.compositeTex);
		glUniform1i(g_diorama.alphaTexLoc, 0);

		// Slight Z offset to prevent z-fighting with the background mesh
		glEnable(GL_POLYGON_OFFSET_FILL);
		glPolygonOffset(-1.0f, -1.0f);

		glBindBuffer(GL_ARRAY_BUFFER, g_diorama.depthMeshVBO);
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_diorama.depthMeshIBO);
		glEnableVertexAttribArray(0);
		glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 20, (void *)0);
		glEnableVertexAttribArray(1);
		glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 20, (void *)12);
		glDrawElements(GL_TRIANGLES, g_diorama.depthMeshIndexCount, GL_UNSIGNED_SHORT, 0);
		glDisableVertexAttribArray(0);
		glDisableVertexAttribArray(1);
		glBindBuffer(GL_ARRAY_BUFFER, 0);
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

		glDisable(GL_POLYGON_OFFSET_FILL);
		glUseProgram(0);
	} else {
		// Fallback: flat actor layer
		float zPos = -DIORAMA_DISTANCE - DIORAMA_DEPTH * 0.15f;
		float model[16];
		mat4_identity(model);
		mat4_translate(model, -hw, DIORAMA_CENTER_Y, zPos);
		mat4_scale(model, dioWidth, dioHeight, 1.0f);
		drawQuad(g_diorama.alphaProgram, g_diorama.alphaMVPLoc, g_diorama.alphaTexLoc,
			dioVP, model, g_diorama.compositeTex);
	}

	// === Verb/inventory panel ===
	// Render below the diorama, at the front so it's not occluded by depth mesh
	if (snap->verbWidth > 0 && snap->verbHeight > 0) {
		float verbAspect = (float)snap->verbWidth / (float)snap->verbHeight;
		float verbW = dioWidth * 0.8f;
		float verbH = verbW / verbAspect;
		float model[16];
		mat4_identity(model);
		mat4_translate(model, -verbW / 2.0f, DIORAMA_CENTER_Y - verbH - 0.05f,
			-DIORAMA_DISTANCE - DIORAMA_DEPTH * 0.1f); // near the front
		mat4_scale(model, verbW, verbH, 1.0f);
		drawQuad(g_diorama.opaqueProgram, g_diorama.opaqueMVPLoc, g_diorama.opaqueTexLoc,
			dioVP, model, g_diorama.verbTex);
	}
}

#endif // USE_OPENXR
