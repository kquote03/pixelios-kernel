/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 PixOS-Project contributors
 *
 * G1 host capture program (docs/research/52): renders two frames through Mesa Panfrost (EGL surfaceless + GLES2,
 * 256x256 RGBA8 renderbuffer) with the "sweet" pan_kmod backend in record mode, so each frame's job chains end up in
 * PAN_SWEET_RECORD/capNNN.malijob instead of a GPU:
 *   frame 0 (cap000): glClear to CLEAR_RGBA
 *   frame 1 (cap001): glClear + one triangle in TRI_RGBA (NDC (-0.5,-0.5) (0.5,-0.5) (0,0.5); covers the centre pixel)
 * Run (gpu/capture/run-capture.sh): LD_PRELOAD=libpanfrost_noop_drm_shim.so PAN_GPU_ID=9202 PAN_SWEET_RECORD=<dir>
 *   PAN_MESA_DEBUG=linear,nocrc ./sweetgl_capture
 * The drm-shim only provides a render node so EGL can open the panfrost driver; every BO and submit goes through the
 * sweet backend.
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <stdio.h>
#include <stdlib.h>

#define W 256
#define H 256

static const GLfloat clear_c[4] = { 0.2f, 0.4f, 0.6f, 1.0f };    /* 0x33,0x66,0x99,0xff: exact in UNORM8 */

static GLuint
shader(GLenum type, const char *src)
{
	GLuint s = glCreateShader(type);
	GLint ok = 0;
	glShaderSource(s, 1, &src, NULL);
	glCompileShader(s);
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[1024];
		glGetShaderInfoLog(s, sizeof(log), NULL, log);
		fprintf(stderr, "shader: %s\n", log);
		exit(1);
	}
	return s;
}

int
main(void)
{
	PFNEGLGETPLATFORMDISPLAYEXTPROC gpd = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
	EGLDisplay dpy = gpd ? gpd(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL) : EGL_NO_DISPLAY;
	EGLint maj, min, n;
	if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, &maj, &min)) {
		fprintf(stderr, "eglInitialize failed\n");
		return 1;
	}
	static const EGLint cattr[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE, 0, EGL_NONE };
	EGLConfig cfg;
	if (!eglChooseConfig(dpy, cattr, &cfg, 1, &n) || n < 1) {
		fprintf(stderr, "no config\n");
		return 1;
	}
	eglBindAPI(EGL_OPENGL_ES_API);
	static const EGLint xattr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, xattr);
	if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
		fprintf(stderr, "context failed\n");
		return 1;
	}
	printf("GL_RENDERER=%s\n", glGetString(GL_RENDERER));

	GLuint fbo, rb;
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glGenRenderbuffers(1, &rb);
	glBindRenderbuffer(GL_RENDERBUFFER, rb);
	glRenderbufferStorage(GL_RENDERBUFFER, 0x8058 /* GL_RGBA8_OES */, W, H);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		fprintf(stderr, "fbo incomplete\n");
		return 1;
	}
	glViewport(0, 0, W, H);

	/* frame 0: clear only */
	glClearColor(clear_c[0], clear_c[1], clear_c[2], clear_c[3]);
	glClear(GL_COLOR_BUFFER_BIT);
	glFinish();

	/* frame 1: clear + triangle */
	GLuint p = glCreateProgram();
	glAttachShader(p, shader(GL_VERTEX_SHADER, "attribute vec2 pos; void main() { gl_Position = vec4(pos, 0.0, 1.0); }"));
	glAttachShader(p, shader(GL_FRAGMENT_SHADER, "precision mediump float; void main() { gl_FragColor = vec4(0.0, 1.0, 0.0, 1.0); }"));
	glBindAttribLocation(p, 0, "pos");
	glLinkProgram(p);
	glUseProgram(p);
	static const GLfloat v[] = { -0.5f, -0.5f, 0.5f, -0.5f, 0.0f, 0.5f };
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, v);
	glEnableVertexAttribArray(0);
	glClear(GL_COLOR_BUFFER_BIT);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glFinish();

	GLenum e = glGetError();
	printf("frames done, glGetError=%#x\n", e);
	eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglDestroyContext(dpy, ctx);
	eglTerminate(dpy);
	return e != GL_NO_ERROR;
}
