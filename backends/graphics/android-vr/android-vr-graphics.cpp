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

#include <EGL/egl.h>
#include <android/log.h>
#include <cstring>

// Must come after FORBIDDEN_SYMBOL_ALLOW_ALL and system headers
#include "backends/graphics/android-vr/android-vr-graphics.h"

// GL functions are available via GLAD (included through the graphics headers)
#include "backends/platform/android/android.h"
#include "backends/platform/android/jni-android.h"
#include "backends/graphics/opengl/framebuffer.h"
#include "backends/graphics/opengl/texture.h"

#include "common/events.h"

// From android-vr-main.cpp — shared texture ID for VR rendering
extern uint32_t g_vrSharedTexture;

// Diorama data extraction
extern void dioramaExtract();

#define LOG_TAG "ScummVM_VR"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static const char *kVRVertexShader =
	"#version 300 es\n"
	"layout(location = 0) in vec3 aPosition;\n"
	"layout(location = 1) in vec2 aTexCoord;\n"
	"uniform mat4 uMVP;\n"
	"out vec2 vTexCoord;\n"
	"void main() {\n"
	"    gl_Position = uMVP * vec4(aPosition, 1.0);\n"
	"    vTexCoord = aTexCoord;\n"
	"}\n";

static const char *kVRFragmentShader =
	"#version 300 es\n"
	"precision mediump float;\n"
	"in vec2 vTexCoord;\n"
	"uniform sampler2D uTexture;\n"
	"out vec4 fragColor;\n"
	"void main() {\n"
	"    fragColor = texture(uTexture, vTexCoord);\n"
	"}\n";

static GLuint compileShader(GLenum type, const char *source) {
	GLuint shader = glCreateShader(type);
	glShaderSource(shader, 1, &source, nullptr);
	glCompileShader(shader);

	GLint compiled = 0;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
	if (!compiled) {
		char log[512];
		glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
		__android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "Shader compile error: %s", log);
		glDeleteShader(shader);
		return 0;
	}
	return shader;
}

static void multiplyMatrix(const float *a, const float *b, float *result) {
	for (int i = 0; i < 4; i++) {
		for (int j = 0; j < 4; j++) {
			result[i * 4 + j] = 0.0f;
			for (int k = 0; k < 4; k++) {
				result[i * 4 + j] += a[k * 4 + j] * b[i * 4 + k];
			}
		}
	}
}

AndroidVRGraphicsManager::AndroidVRGraphicsManager(OpenXRSession *xrSession)
	: AndroidGraphicsManager(SkipInit{}),
	  _xrSession(xrSession),
	  _xrInput(nullptr),
	  _sceneTexture(0),
	  _sceneFBO(0),
	  _sceneDepthBuffer(0),
	  _sceneWidth(1920),
	  _sceneHeight(1080),
	  _vrQuadProgram(0),
	  _vrQuadVBO(0),
	  _vrUniformMVP(-1),
	  _vrUniformTexture(-1),
	  _vrMouseX(0),
	  _vrMouseY(0),
	  _vrMouseValid(false) {

	_xrInput = new OpenXRInput();
	_screenGeometry.setScreenSize(_sceneWidth, _sceneHeight);

	// Initialize the surface — at this point the EGL context has already been
	// set up by ScummVM.run() -> initEGL(), and surfaceChanged has been received.
	initSurface();
}

AndroidVRGraphicsManager::~AndroidVRGraphicsManager() {
	destroyVRShaders();

	if (_sceneFBO) {
		glDeleteFramebuffers(1, &_sceneFBO);
	}
	if (_sceneTexture) {
		glDeleteTextures(1, &_sceneTexture);
	}
	if (_sceneDepthBuffer) {
		glDeleteRenderbuffers(1, &_sceneDepthBuffer);
	}

	delete _xrInput;
}

void AndroidVRGraphicsManager::initSurface() {
	LOGI("Initializing VR surface");

	// Use the standard JNI surface init — this creates an EGL window surface
	// from the SurfaceView and makes the GL context current.
	// This is the same as what AndroidGraphicsManager::initSurface() does,
	// but without touch controls.
	assert(!JNI::haveSurface());
	if (!JNI::initSurface()) {
		LOGE("JNI::initSurface failed in VR mode");
		return;
	}

	// Now we have a valid EGL context. Set up ScummVM's OpenGL compositing.
	notifyContextCreate(OpenGL::kContextGLES2,
		new OpenGL::Backbuffer(),
		OpenGL::Texture::getRGBPixelFormat(),
		OpenGL::Texture::getRGBAPixelFormat());

	handleResize(JNI::egl_surface_width, JNI::egl_surface_height);

	// Create FBO wrapping the shared texture from the VR main thread.
	// The texture was created on the VR context and is shared via the
	// EGL shared context mechanism. We just need an FBO on THIS context.
	if (g_vrSharedTexture) {
		_sceneTexture = g_vrSharedTexture;
		glGenFramebuffers(1, &_sceneFBO);
		glBindFramebuffer(GL_FRAMEBUFFER, _sceneFBO);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, _sceneTexture, 0);

		GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
		if (status != GL_FRAMEBUFFER_COMPLETE) {
			LOGE("Shared scene FBO not complete: 0x%x", status);
		}
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		LOGI("Using shared texture %u from VR thread", _sceneTexture);
	} else {
		LOGE("No shared texture available from VR thread!");
	}

	// Initialize VR shaders for the virtual screen quad
	// VR shaders not needed here — VR rendering is handled by android_main
	// initVRShaders();

	LOGI("VR surface initialized: %dx%d, shared texture %u",
		JNI::egl_surface_width, JNI::egl_surface_height, _sceneTexture);
}

void AndroidVRGraphicsManager::resizeSurface() {
	// VR mode: no surface management needed — we render to a shared FBO
}

void AndroidVRGraphicsManager::deinitSurface() {
	if (!JNI::haveSurface())
		return;

	LOGI("Deinitializing VR surface");

	destroyVRShaders();

	if (_sceneFBO) { glDeleteFramebuffers(1, &_sceneFBO); _sceneFBO = 0; }
	if (_sceneTexture) { glDeleteTextures(1, &_sceneTexture); _sceneTexture = 0; }
	if (_sceneDepthBuffer) { glDeleteRenderbuffers(1, &_sceneDepthBuffer); _sceneDepthBuffer = 0; }

	notifyContextDestroy();
	JNI::deinitSurface();
}

void AndroidVRGraphicsManager::updateScreen() {
	// ScummVM renders to FBO 0 (the PBuffer backbuffer, now 640x480).
	// OpenGLGraphicsManager::updateScreen() composites everything,
	// then calls refreshScreen() which copies to the shared texture.
	OpenGLGraphicsManager::updateScreen();
}

bool AndroidVRGraphicsManager::loadVideoMode(uint requestedWidth, uint requestedHeight,
                                              bool resizable, int antialiasing) {
	// VR mode uses fixed scene resolution
	return true;
}

void AndroidVRGraphicsManager::refreshScreen() {
	// ScummVM just rendered to FBO 0 (PBuffer backbuffer).
	// Copy it to the shared texture FBO so the VR thread can display it.
	if (_sceneFBO) {
		glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, _sceneFBO);
		glBlitFramebuffer(0, 0, _sceneWidth, _sceneHeight,
			0, _sceneHeight, _sceneWidth, 0,
			GL_COLOR_BUFFER_BIT, GL_NEAREST);
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		glFinish();
	}

	// Extract diorama data from the SCUMM engine for 3D rendering
	dioramaExtract();
}

void AndroidVRGraphicsManager::renderEye(int eye, const float *eyePose, const float *eyeFov) {
	glClearColor(0.05f, 0.05f, 0.08f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	float viewMatrix[16];
	float projMatrix[16];
	float viewProjMatrix[16];

	VRScreenGeometry::poseToViewMatrix(eyePose, viewMatrix);
	VRScreenGeometry::fovToProjectionMatrix(eyeFov, 0.1f, 100.0f, projMatrix);
	multiplyMatrix(projMatrix, viewMatrix, viewProjMatrix);

	drawVirtualScreen(viewProjMatrix);
}

void AndroidVRGraphicsManager::drawVirtualScreen(const float *viewProjMatrix) {
	if (!_vrQuadProgram)
		return;

	glUseProgram(_vrQuadProgram);

	float modelMatrix[16];
	_screenGeometry.getModelMatrix(modelMatrix);

	float mvp[16];
	multiplyMatrix(viewProjMatrix, modelMatrix, mvp);

	glUniformMatrix4fv(_vrUniformMVP, 1, GL_FALSE, mvp);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, _sceneTexture);
	glUniform1i(_vrUniformTexture, 0);

	glBindBuffer(GL_ARRAY_BUFFER, _vrQuadVBO);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *)0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *)(3 * sizeof(float)));

	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

	glDisableVertexAttribArray(0);
	glDisableVertexAttribArray(1);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glUseProgram(0);
}

void AndroidVRGraphicsManager::processVRInput() {
	// TODO: Controller input - phase 3
}

void AndroidVRGraphicsManager::initVRShaders() {
	GLuint vs = compileShader(GL_VERTEX_SHADER, kVRVertexShader);
	GLuint fs = compileShader(GL_FRAGMENT_SHADER, kVRFragmentShader);

	if (!vs || !fs) {
		LOGE("Failed to compile VR shaders");
		if (vs) glDeleteShader(vs);
		if (fs) glDeleteShader(fs);
		return;
	}

	_vrQuadProgram = glCreateProgram();
	glAttachShader(_vrQuadProgram, vs);
	glAttachShader(_vrQuadProgram, fs);
	glLinkProgram(_vrQuadProgram);

	glDeleteShader(vs);
	glDeleteShader(fs);

	GLint linked = 0;
	glGetProgramiv(_vrQuadProgram, GL_LINK_STATUS, &linked);
	if (!linked) {
		char log[512];
		glGetProgramInfoLog(_vrQuadProgram, sizeof(log), nullptr, log);
		LOGE("Shader link error: %s", log);
		glDeleteProgram(_vrQuadProgram);
		_vrQuadProgram = 0;
		return;
	}

	_vrUniformMVP = glGetUniformLocation(_vrQuadProgram, "uMVP");
	_vrUniformTexture = glGetUniformLocation(_vrQuadProgram, "uTexture");

	float vertices[20];
	_screenGeometry.getQuadVertices(vertices);

	glGenBuffers(1, &_vrQuadVBO);
	glBindBuffer(GL_ARRAY_BUFFER, _vrQuadVBO);
	glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
	glBindBuffer(GL_ARRAY_BUFFER, 0);

	LOGI("VR shaders initialized");
}

void AndroidVRGraphicsManager::destroyVRShaders() {
	if (_vrQuadProgram) { glDeleteProgram(_vrQuadProgram); _vrQuadProgram = 0; }
	if (_vrQuadVBO) { glDeleteBuffers(1, &_vrQuadVBO); _vrQuadVBO = 0; }
}

#endif // USE_OPENXR
