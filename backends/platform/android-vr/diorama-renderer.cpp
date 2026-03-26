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

	uint8_t lastRoom;
	uint32_t lastFrame;
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

		if (snap->currentRoom != g_diorama.lastRoom) {
			g_diorama.lastRoom = snap->currentRoom;
			LOGI("Diorama room %d: %d boxes, %d z-planes",
				snap->currentRoom, snap->numBoxes, snap->numZPlanes);
		}
	}

	// Calculate aspect-correct diorama dimensions from actual game area
	float aspect = (float)snap->screenWidth / (float)snap->screenHeight;
	float dioWidth = DIORAMA_WIDTH;
	float dioHeight = dioWidth / aspect;
	float hw = dioWidth / 2.0f;

	// === Back wall ===
	{
		float model[16];
		mat4_identity(model);
		mat4_translate(model, -hw, DIORAMA_CENTER_Y, -DIORAMA_DISTANCE - DIORAMA_DEPTH);
		mat4_scale(model, dioWidth, dioHeight, 1.0f);
		drawQuad(g_diorama.opaqueProgram, g_diorama.opaqueMVPLoc, g_diorama.opaqueTexLoc,
			viewProj, model, g_diorama.backgroundTex);
	}

	// === Z-plane foreground layers ===
	// TODO: Z-planes cause artifacts — disabled until mask extraction is fixed
	// (doubled elements, black triangles from garbage mask data)

	// === Actor/foreground diff layer ===
	// Place actors about 40% forward from the back wall
	{
		float zPos = -DIORAMA_DISTANCE - DIORAMA_DEPTH * 0.6f;
		float model[16];
		mat4_identity(model);
		mat4_translate(model, -hw, DIORAMA_CENTER_Y, zPos);
		mat4_scale(model, dioWidth, dioHeight, 1.0f);
		drawQuad(g_diorama.alphaProgram, g_diorama.alphaMVPLoc, g_diorama.alphaTexLoc,
			viewProj, model, g_diorama.compositeTex);
	}

	// === Verb/inventory panel ===
	// Render as a flat panel below the diorama, at the back wall depth
	if (snap->verbWidth > 0 && snap->verbHeight > 0) {
		float verbAspect = (float)snap->verbWidth / (float)snap->verbHeight;
		float verbW = dioWidth * 0.8f; // slightly narrower than diorama
		float verbH = verbW / verbAspect;
		float model[16];
		mat4_identity(model);
		mat4_translate(model, -verbW / 2.0f, DIORAMA_CENTER_Y - verbH - 0.05f,
			-DIORAMA_DISTANCE - DIORAMA_DEPTH);
		mat4_scale(model, verbW, verbH, 1.0f);
		drawQuad(g_diorama.opaqueProgram, g_diorama.opaqueMVPLoc, g_diorama.opaqueTexLoc,
			viewProj, model, g_diorama.verbTex);
	}
}

#endif // USE_OPENXR
