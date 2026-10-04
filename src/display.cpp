#include "display.h"

#include <SDL.h>
#include <epoxy/gl.h>
#include <linux/videodev2.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace fv {

namespace {

// GLSL 150 (paired with an OpenGL 3.2 core context, same as the ImGui SDL2 +
// OpenGL3 example this backend is modelled on).
const char* kVertexSrc = R"(#version 150
in vec2 aPos;
in vec2 aUV;
out vec2 vUV;
void main() {
    vUV = aUV;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

// RGB formats are uploaded with the byte order already normalized by
// glTexSubImage2D's format/type (see Display::upload), so this shader just
// samples the texture; it works for every non-UYVY PixelFormat.
const char* kFragDirectSrc = R"(#version 150
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uTex;
uniform int uGrey;
void main() {
    vec4 t = texture(uTex, vUV);
    vec3 rgb = (uGrey == 1) ? vec3(t.r) : t.rgb;
    FragColor = vec4(rgb, 1.0);
}
)";

// UYVY decode on the GPU (replaces the M0 CPU conversion). The texture holds
// the raw packed bytes reinterpreted as RGBA8 at half the source width: each
// texel is one (U, Y0, V, Y1) group, exactly as V4L2's UYVY layout has it in
// memory. texelFetch is used (not filtered sampling) so chroma pairing is
// never blurred across source pixels.
const char* kFragUyvySrc = R"(#version 150
in vec2 vUV;
out vec4 FragColor;
uniform sampler2D uTex;
uniform int uFrameWidth;
uniform int uFrameHeight;
uniform int uBt709;
void main() {
    int px = clamp(int(vUV.x * float(uFrameWidth)), 0, uFrameWidth - 1);
    int py = clamp(int(vUV.y * float(uFrameHeight)), 0, uFrameHeight - 1);
    vec4 t = texelFetch(uTex, ivec2(px / 2, py), 0);
    float y = ((px % 2) == 0) ? t.g : t.a;
    float u = t.r;
    float v = t.b;
    float yy = (y * 255.0 - 16.0) / 219.0;
    float uu = (u * 255.0 - 128.0) / 224.0;
    float vv = (v * 255.0 - 128.0) / 224.0;
    vec3 rgb = (uBt709 == 1)
        ? vec3(yy + 1.5748 * vv, yy - 0.1873 * uu - 0.4681 * vv, yy + 1.8556 * uu)
        : vec3(yy + 1.4020 * vv, yy - 0.3441 * uu - 0.7141 * vv, yy + 1.7720 * uu);
    FragColor = vec4(clamp(rgb, 0.0, 1.0), 1.0);
}
)";

unsigned compile(GLenum type, const char* src, std::string& err) {
    unsigned s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(s, sizeof log, nullptr, log);
        err = std::string("shader compile: ") + log;
        glDeleteShader(s);
        return 0;
    }
    return s;
}

unsigned link(unsigned vs, unsigned fs, std::string& err) {
    unsigned p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glBindAttribLocation(p, 0, "aPos");
    glBindAttribLocation(p, 1, "aUV");
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetProgramInfoLog(p, sizeof log, nullptr, log);
        err = std::string("shader link: ") + log;
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

}  // namespace

Display::~Display() { destroy(); }

bool Display::create(uint32_t frameW, uint32_t frameH, bool fullscreen, bool vsync, std::string& err, int windowW,
                     int windowH) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        err = std::string("SDL_Init: ") + SDL_GetError();
        return false;
    }
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);

    int scale = 1;
    SDL_Rect usable;
    if (SDL_GetDisplayUsableBounds(0, &usable) == 0 && frameW && frameH) {
        // The 60 px allowance approximates title bar and panel.
        scale = std::max(1, std::min<int>(usable.w / int(frameW), (usable.h - 60) / int(frameH)));
    }
    int ww = int(frameW) * scale, wh = int(frameH) * scale;
    if (windowW > 0 && windowH > 0) {
        ww = windowW, wh = windowH;
        if (SDL_GetDisplayUsableBounds(0, &usable) == 0) ww = std::min(ww, usable.w), wh = std::min(wh, usable.h - 60);
    }
    window_ = SDL_CreateWindow("freshVision", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, ww, wh,
                               SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (!window_) {
        err = std::string("SDL_CreateWindow: ") + SDL_GetError();
        return false;
    }
    glContext_ = SDL_GL_CreateContext(window_);
    if (!glContext_) {
        err = std::string("SDL_GL_CreateContext: ") + SDL_GetError();
        return false;
    }
    setVsync(vsync);
    if (fullscreen) toggleFullscreen();

    if (!compileShaders(err)) return false;

    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    // pos.xy, uv.xy; drawn as a triangle fan covering the current glViewport.
    const float quad[] = {
        -1.f, 1.f,  0.f, 0.f,  //
        1.f,  1.f,  1.f, 0.f,  //
        1.f,  -1.f, 1.f, 1.f,  //
        -1.f, -1.f, 0.f, 1.f,
    };
    glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glBindVertexArray(0);

    std::printf("OpenGL: %s / %s, GLSL %s, swap interval %d\n", glGetString(GL_RENDERER), glGetString(GL_VERSION),
                glGetString(GL_SHADING_LANGUAGE_VERSION), SDL_GL_GetSwapInterval());
    return true;
}

bool Display::compileShaders(std::string& err) {
    unsigned vs = compile(GL_VERTEX_SHADER, kVertexSrc, err);
    if (!vs) return false;
    unsigned fsDirect = compile(GL_FRAGMENT_SHADER, kFragDirectSrc, err);
    if (!fsDirect) {
        glDeleteShader(vs);
        return false;
    }
    unsigned fsUyvy = compile(GL_FRAGMENT_SHADER, kFragUyvySrc, err);
    if (!fsUyvy) {
        glDeleteShader(vs);
        glDeleteShader(fsDirect);
        return false;
    }
    progDirect_ = link(vs, fsDirect, err);
    if (progDirect_) progUyvy_ = link(vs, fsUyvy, err);
    glDeleteShader(vs);
    glDeleteShader(fsDirect);
    glDeleteShader(fsUyvy);
    return progDirect_ != 0 && progUyvy_ != 0;
}

void Display::destroy() {
    if (glContext_) {
        SDL_GL_MakeCurrent(window_, glContext_);
        if (texture_) glDeleteTextures(1, &texture_);
        if (pbo_[0]) glDeleteBuffers(2, pbo_);
        if (vbo_) glDeleteBuffers(1, &vbo_);
        if (vao_) glDeleteVertexArrays(1, &vao_);
        if (progDirect_) glDeleteProgram(progDirect_);
        if (progUyvy_) glDeleteProgram(progUyvy_);
        texture_ = vbo_ = vao_ = progDirect_ = progUyvy_ = 0;
        pbo_[0] = pbo_[1] = 0;
        pboBytes_ = 0;
        SDL_GL_DeleteContext(glContext_);
        glContext_ = nullptr;
    }
    if (window_) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
        SDL_Quit();
    }
}

bool Display::setFrame(const PixelFormat& pf, uint32_t w, uint32_t h, uint32_t colorspace, std::string& err) {
    colorspace_ = colorspace;
    if (texture_ && pf.fourcc == format_.fourcc && w == w_ && h == h_) return true;

    if (texture_) glDeleteTextures(1, &texture_);
    glGenTextures(1, &texture_);
    glBindTexture(GL_TEXTURE_2D, texture_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    uint32_t texW = (pf.layout == Layout::Uyvy) ? (w + 1) / 2 : w;
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, GLsizei(texW), GLsizei(h), 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    if (glGetError() != GL_NO_ERROR) {
        err = "cannot create a " + std::to_string(w) + "x" + std::to_string(h) + " texture";
        return false;
    }
    format_ = pf;
    w_ = w;
    h_ = h;
    return true;
}

unsigned Display::uploadPbo(const uint8_t* data, size_t bytes) {
    if (bytes != pboBytes_) {
        if (pbo_[0]) glDeleteBuffers(2, pbo_);
        glGenBuffers(2, pbo_);
        for (int i = 0; i < 2; ++i) {
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo_[i]);
            glBufferData(GL_PIXEL_UNPACK_BUFFER, GLsizeiptr(bytes), nullptr, GL_STREAM_DRAW);
        }
        pboBytes_ = bytes;
        pboIndex_ = 0;
    }
    pboIndex_ ^= 1;  // alternate: write into the PBO the GPU is least likely to still be reading
    unsigned pbo = pbo_[pboIndex_];
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo);
    // Orphan (fresh allocation) before mapping, so a still-in-flight DMA from
    // the previous upload on this PBO never stalls this one.
    glBufferData(GL_PIXEL_UNPACK_BUFFER, GLsizeiptr(bytes), nullptr, GL_STREAM_DRAW);
    void* ptr = glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0, GLsizeiptr(bytes),
                                 GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
    if (ptr) {
        std::memcpy(ptr, data, bytes);
        glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
    } else {
        glBufferSubData(GL_PIXEL_UNPACK_BUFFER, 0, GLsizeiptr(bytes), data);
    }
    return pbo;
}

void Display::upload(const uint8_t* data, size_t stride) {
    size_t bytes = stride * h_;
    uploadPbo(data, bytes);  // leaves this PBO bound to GL_PIXEL_UNPACK_BUFFER

    glBindTexture(GL_TEXTURE_2D, texture_);
    if (format_.layout == Layout::Uyvy) {
        GLsizei rowTexels = GLsizei(stride / 4);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, rowTexels);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, GLsizei((w_ + 1) / 2), GLsizei(h_), GL_RGBA, GL_UNSIGNED_BYTE,
                        nullptr);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    } else {
        GLenum glFormat = GL_BGRA;
        GLenum glType = GL_UNSIGNED_BYTE;
        switch (format_.layout) {
            case Layout::Bgrx8: break;
            case Layout::Rgbx8: glFormat = GL_RGBA; break;
            case Layout::Xrgb8: glType = GL_UNSIGNED_INT_8_8_8_8; break;
            case Layout::Xbgr8: glFormat = GL_RGBA; glType = GL_UNSIGNED_INT_8_8_8_8; break;
            case Layout::Rgb24: glFormat = GL_RGB; break;
            case Layout::Bgr24: glFormat = GL_BGR; break;
            case Layout::Uyvy: break;  // unreachable, handled above
            case Layout::Grey8: glFormat = GL_RED; break;
        }
        GLsizei rowTexels = GLsizei(stride / format_.bytesPerPixel);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, rowTexels);
        // 3-byte pixels: rows needn't be a multiple of 4 bytes (a decoded
        // 12x NTSC line is 2219 x 3 = 6657 bytes).
        if (format_.bytesPerPixel % 4) glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, GLsizei(w_), GLsizei(h_), glFormat, glType, nullptr);
        if (format_.bytesPerPixel % 4) glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    }
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
}

void Display::beginFrame() {
    int ww = 0, wh = 0;
    SDL_GL_GetDrawableSize(window_, &ww, &wh);
    glViewport(0, 0, ww, wh);
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);
}

int Display::maxScreenFactor(double pixelAspect) const {
    if (!window_ || !w_ || !h_) return 0;
    int idx = SDL_GetWindowDisplayIndex(window_);
    SDL_Rect usable;
    if (idx < 0 || SDL_GetDisplayUsableBounds(idx, &usable) != 0) return 0;
    double effW = double(w_) * (pixelAspect > 0 ? pixelAspect : 1.0);
    // Same 60 px title-bar allowance as create() uses for the initial size.
    int best = 0;
    for (int f = 1; f <= kMaxIntegerFactor; ++f)
        if (effW * f <= usable.w + 0.5 && double(h_) * f <= usable.h - 60 + 0.5) best = f;
    return best;
}

void Display::drawVideo(const DisplayOptions& opts) {
    if (!texture_ || !w_ || !h_) return;
    int fullW = 0, fullH = 0;
    SDL_GL_GetDrawableSize(window_, &fullW, &fullH);
    const bool area = opts.areaW > 0 && opts.areaH > 0;
    const int ww = area ? opts.areaW : fullW, wh = area ? opts.areaH : fullH;
    const int ax = area ? opts.areaX : 0, ay = area ? fullH - opts.areaY - opts.areaH : 0;  // GL origin: bottom-left
    double effW = double(w_) * (opts.pixelAspect > 0 ? opts.pixelAspect : 1.0);

    int dw, dh;
    if (opts.scaleMode == ScaleMode::Stretch) {
        dw = ww;
        dh = wh;
    } else if (opts.scaleMode == ScaleMode::Integer && opts.integerFactor > 0) {
        // Fixed factor: the picture keeps its size whatever the window does.
        // Centered; a window smaller than the picture cuts it at the edges
        // (a negative viewport origin is valid GL and just clips).
        int f = std::min(opts.integerFactor, kMaxIntegerFactor);
        dw = int(std::lround(effW * f));
        dh = int(h_) * f;
    } else if (opts.scaleMode == ScaleMode::Integer) {
        int factor = std::max(1, std::min(int(ww / effW), int(wh / double(h_))));
        // Only use an integer factor if it actually fits; else fall through to a fractional fit.
        if (double(factor) * effW <= ww + 0.5 && double(factor) * h_ <= wh + 0.5 && factor >= 1 &&
            int(ww / effW) >= 1 && int(wh / double(h_)) >= 1) {
            dw = int(std::lround(effW * factor));
            dh = int(h_ * factor);
        } else {
            double f = std::min(double(ww) / effW, double(wh) / h_);
            dw = std::max(1, int(std::lround(effW * f)));
            dh = std::max(1, int(std::lround(h_ * f)));
        }
    } else {  // Fit
        double f = std::min(double(ww) / effW, double(wh) / h_);
        dw = std::max(1, int(std::lround(effW * f)));
        dh = std::max(1, int(std::lround(h_ * f)));
    }
    glViewport(ax + (ww - dw) / 2, ay + (wh - dh) / 2, dw, dh);

    unsigned prog = (format_.layout == Layout::Uyvy) ? progUyvy_ : progDirect_;
    glUseProgram(prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture_);
    GLenum filter = opts.linearFilter ? GL_LINEAR : GL_NEAREST;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glUniform1i(glGetUniformLocation(prog, "uTex"), 0);
    if (format_.layout == Layout::Uyvy) {
        bool bt709 = colorspace_ == V4L2_COLORSPACE_REC709 || (colorspace_ == 0 && h_ >= 720);
        glUniform1i(glGetUniformLocation(prog, "uFrameWidth"), GLint(w_));
        glUniform1i(glGetUniformLocation(prog, "uFrameHeight"), GLint(h_));
        glUniform1i(glGetUniformLocation(prog, "uBt709"), bt709 ? 1 : 0);
    } else {
        // progDirect_ is shared across every non-UYVY format, so uGrey must
        // be set explicitly every draw -- a stale 1 from a previous Grey8
        // frame would otherwise tint a later RGB frame red-channel-only.
        glUniform1i(glGetUniformLocation(prog, "uGrey"), format_.layout == Layout::Grey8 ? 1 : 0);
    }

    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    glBindVertexArray(0);
}

void Display::endFrame() {
    SDL_GL_SwapWindow(window_);
    glFinish();
}

void Display::setTitle(const std::string& title) {
    if (window_) SDL_SetWindowTitle(window_, title.c_str());
}

void Display::toggleFullscreen() {
    fullscreen_ = !fullscreen_;
    SDL_SetWindowFullscreen(window_, fullscreen_ ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}

void Display::setVsync(bool on) {
    vsync_ = on;
    if (SDL_GL_SetSwapInterval(on ? 1 : 0) != 0) {
        std::fprintf(stderr, "SDL_GL_SetSwapInterval: %s\n", SDL_GetError());
        vsync_ = SDL_GL_GetSwapInterval() != 0;
    }
}

}  // namespace fv
