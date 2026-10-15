// On-screen keyboard text box, drawn over the presented frame while the game
// waits on sceUtilityOsk (psp_osk_session.h). Main (GL) thread only. Text is
// rasterized with stb_truetype from a system font; PSPRECOMP_OSK_FONT=<.ttf>
// overrides the font file.
#include "psp_osk_overlay.h"
#include "psp_osk_session.h"

#include <SDL.h>
#include <glad/glad.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

namespace {

struct Font {
    bool tried = false;
    bool ok = false;
    std::vector<unsigned char> data;
    stbtt_fontinfo info{};
};

Font& font() {
    static Font f;
    if (f.tried) return f;
    f.tried = true;
    std::vector<std::string> paths;
    if (const char* e = std::getenv("PSPRECOMP_OSK_FONT")) paths.push_back(e);
    paths.push_back("C:/Windows/Fonts/segoeui.ttf");
    paths.push_back("C:/Windows/Fonts/arial.ttf");
    paths.push_back("/System/Library/Fonts/Supplemental/Arial.ttf");
    paths.push_back("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf");
    for (const std::string& p : paths) {
        FILE* fp = std::fopen(p.c_str(), "rb");
        if (!fp) continue;
        std::fseek(fp, 0, SEEK_END);
        const long n = std::ftell(fp);
        std::fseek(fp, 0, SEEK_SET);
        f.data.resize(n > 0 ? static_cast<size_t>(n) : 0);
        const size_t got = n > 0 ? std::fread(f.data.data(), 1, f.data.size(), fp) : 0;
        std::fclose(fp);
        if (got == f.data.size() && got > 0 &&
            stbtt_InitFont(&f.info, f.data.data(), stbtt_GetFontOffsetForIndex(f.data.data(), 0))) {
            f.ok = true;
            std::fprintf(stderr, "[OSK] font %s\n", p.c_str());
            return f;
        }
    }
    std::fprintf(stderr, "[OSK] no font found; set PSPRECOMP_OSK_FONT=<file.ttf>\n");
    return f;
}

// UTF-8 -> code points (enough for drawing; invalid bytes become '?').
std::vector<int> codepoints(const std::string& s) {
    std::vector<int> out;
    for (size_t i = 0; i < s.size();) {
        const unsigned char b = static_cast<unsigned char>(s[i]);
        int len = b < 0x80 ? 1 : (b >> 5) == 6 ? 2 : (b >> 4) == 14 ? 3 : (b >> 3) == 30 ? 4 : 0;
        if (len == 0 || i + len > s.size()) { out.push_back('?'); i++; continue; }
        int cp = len == 1 ? b : b & (0xFF >> (len + 1));
        for (int j = 1; j < len; j++) cp = (cp << 6) | (static_cast<unsigned char>(s[i + j]) & 0x3F);
        out.push_back(cp);
        i += len;
    }
    return out;
}

// A line of text rasterized to an 8-bit coverage bitmap.
struct TextImage {
    int w = 0, h = 0;
    std::vector<unsigned char> a;
};

TextImage rasterize(const std::string& utf8, float px) {
    TextImage img;
    Font& f = font();
    if (!f.ok || px < 4.0f) return img;
    const float scale = stbtt_ScaleForPixelHeight(&f.info, px);
    int ascent = 0, descent = 0, gap = 0;
    stbtt_GetFontVMetrics(&f.info, &ascent, &descent, &gap);
    const int base = static_cast<int>(std::lround(ascent * scale));
    const std::vector<int> cps = codepoints(utf8);
    float x = 0.0f;
    for (size_t k = 0; k < cps.size(); k++) {
        int adv = 0, lsb = 0;
        stbtt_GetCodepointHMetrics(&f.info, cps[k], &adv, &lsb);
        x += adv * scale;
        if (k + 1 < cps.size()) x += stbtt_GetCodepointKernAdvance(&f.info, cps[k], cps[k + 1]) * scale;
    }
    img.w = static_cast<int>(std::ceil(x)) + 2;
    img.h = static_cast<int>(std::ceil((ascent - descent) * scale)) + 2;
    img.a.assign(static_cast<size_t>(img.w) * img.h, 0);
    x = 1.0f;
    for (size_t k = 0; k < cps.size(); k++) {
        int adv = 0, lsb = 0, x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        stbtt_GetCodepointHMetrics(&f.info, cps[k], &adv, &lsb);
        stbtt_GetCodepointBitmapBox(&f.info, cps[k], scale, scale, &x0, &y0, &x1, &y1);
        const int gx = static_cast<int>(std::lround(x)) + x0;
        const int gy = base + y0 + 1;
        const int gw = x1 - x0, gh = y1 - y0;
        if (gw > 0 && gh > 0 && gx >= 0 && gy >= 0 && gx + gw <= img.w && gy + gh <= img.h) {
            stbtt_MakeCodepointBitmap(&f.info, &img.a[static_cast<size_t>(gy) * img.w + gx], gw, gh,
                                      img.w, scale, scale, cps[k]);
        }
        x += adv * scale;
        if (k + 1 < cps.size()) x += stbtt_GetCodepointKernAdvance(&f.info, cps[k], cps[k + 1]) * scale;
    }
    return img;
}

struct Gl {
    bool ready = false;
    GLuint prog = 0, vao = 0, vbo = 0;
    GLint u_color = -1, u_textured = -1, u_tex = -1;
};

GLuint compile(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        std::fprintf(stderr, "[OSK] shader: %s\n", log);
    }
    return s;
}

Gl& gl() {
    static Gl g;
    if (g.ready) return g;
    const char* vs =
        "#version 330 core\n"
        "layout(location=0) in vec2 a_pos; layout(location=1) in vec2 a_uv;\n"
        "out vec2 v_uv; void main(){ gl_Position = vec4(a_pos, 0.0, 1.0); v_uv = a_uv; }\n";
    const char* fs =
        "#version 330 core\n"
        "uniform sampler2D u_tex; uniform vec4 u_color; uniform int u_textured;\n"
        "in vec2 v_uv; out vec4 o;\n"
        "void main(){ float a = u_textured != 0 ? texture(u_tex, v_uv).r : 1.0;"
        " o = vec4(u_color.rgb, u_color.a * a); }\n";
    g.prog = glCreateProgram();
    const GLuint v = compile(GL_VERTEX_SHADER, vs), f = compile(GL_FRAGMENT_SHADER, fs);
    glAttachShader(g.prog, v);
    glAttachShader(g.prog, f);
    glLinkProgram(g.prog);
    glDeleteShader(v);
    glDeleteShader(f);
    g.u_color = glGetUniformLocation(g.prog, "u_color");
    g.u_textured = glGetUniformLocation(g.prog, "u_textured");
    g.u_tex = glGetUniformLocation(g.prog, "u_tex");
    glGenVertexArrays(1, &g.vao);
    glGenBuffers(1, &g.vbo);
    glBindVertexArray(g.vao);
    glBindBuffer(GL_ARRAY_BUFFER, g.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 24, nullptr, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          reinterpret_cast<void*>(2 * sizeof(float)));
    glBindVertexArray(0);
    g.ready = true;
    return g;
}

// One cached text texture per role, re-rasterized only when its text or
// pixel size changes.
struct TextTex {
    std::string text;
    int px = 0;
    GLuint tex = 0;
    int w = 0, h = 0;
};

void update_text(TextTex& t, const std::string& text, int px) {
    if (t.tex != 0 && t.text == text && t.px == px) return;
    t.text = text;
    t.px = px;
    const TextImage img = rasterize(text, static_cast<float>(px));
    t.w = img.w;
    t.h = img.h;
    if (t.tex == 0) glGenTextures(1, &t.tex);
    glBindTexture(GL_TEXTURE_2D, t.tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, img.w > 0 ? img.w : 1, img.h > 0 ? img.h : 1, 0, GL_RED,
                 GL_UNSIGNED_BYTE, img.a.empty() ? nullptr : img.a.data());
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

// Draw a window-space rectangle (top-left origin, pixels).
void rect(int win_w, int win_h, float x, float y, float w, float h, const float c[4], GLuint tex) {
    Gl& g = gl();
    const float x0 = x / win_w * 2.0f - 1.0f, x1 = (x + w) / win_w * 2.0f - 1.0f;
    const float y0 = 1.0f - y / win_h * 2.0f, y1 = 1.0f - (y + h) / win_h * 2.0f;
    const float v[24] = {x0, y0, 0, 0, x1, y0, 1, 0, x1, y1, 1, 1,
                         x0, y0, 0, 0, x1, y1, 1, 1, x0, y1, 0, 1};
    glBindBuffer(GL_ARRAY_BUFFER, g.vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(v), v);
    glUniform4fv(g.u_color, 1, c);
    glUniform1i(g.u_textured, tex != 0 ? 1 : 0);
    if (tex != 0) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex);
        glUniform1i(g.u_tex, 0);
    }
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

}  // namespace

bool psp_osk_overlay_draw(int win_w, int win_h, int rx, int ry, int rw, int rh) {
    const OskSession::View v = osk_session().view();
    if (!v.active || win_w <= 0 || win_h <= 0 || rw <= 0 || rh <= 0) return false;
    static TextTex title, text, counter, hint;

    // Game rectangle in top-left window coordinates (ry is GL bottom-left).
    const float gx = static_cast<float>(rx);
    const float gy = static_cast<float>(win_h - (ry + rh));
    const float pw = rw * 0.84f, ph = rh * 0.40f;
    const float px = gx + (rw - pw) * 0.5f, py = gy + rh * 0.28f;
    const float pad = rh * 0.035f;
    const int title_px = static_cast<int>(rh * 0.060f);
    const int text_px = static_cast<int>(rh * 0.085f);
    const int small_px = static_cast<int>(rh * 0.045f);

    update_text(title, v.title.empty() ? std::string("Texto") : v.title, title_px);
    const bool blink = (SDL_GetTicks() / 500) % 2 == 0;
    update_text(text, v.text + (blink ? "|" : " "), text_px);
    update_text(counter, std::to_string(v.length) + "/" + std::to_string(v.max_len), small_px);
    update_text(hint, "Enter: aceptar    Esc: cancelar    Retroceso: borrar", small_px);

    Gl& g = gl();
    glUseProgram(g.prog);
    glBindVertexArray(g.vao);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glBlendEquation(GL_FUNC_ADD);

    const float shade[4] = {0.0f, 0.0f, 0.0f, 0.45f};
    const float panel[4] = {0.07f, 0.06f, 0.10f, 0.92f};
    const float field[4] = {1.0f, 1.0f, 1.0f, 0.10f};
    const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    const float grey[4] = {0.75f, 0.75f, 0.80f, 1.0f};
    const float accent[4] = {1.0f, 0.82f, 0.30f, 1.0f};

    rect(win_w, win_h, gx, gy, static_cast<float>(rw), static_cast<float>(rh), shade, 0);
    rect(win_w, win_h, px, py, pw, ph, panel, 0);
    rect(win_w, win_h, px, py, pw, rh * 0.008f, accent, 0);
    float y = py + pad;
    rect(win_w, win_h, px + pad, y, static_cast<float>(title.w), static_cast<float>(title.h), accent, title.tex);
    y += title.h + pad * 0.6f;
    const float fh = text.h + pad * 0.6f;
    rect(win_w, win_h, px + pad, y, pw - 2 * pad, fh, field, 0);
    rect(win_w, win_h, px + pad * 1.4f, y + pad * 0.3f, static_cast<float>(text.w), static_cast<float>(text.h), white,
         text.tex);
    rect(win_w, win_h, px + pw - pad * 1.4f - counter.w, y + (fh - counter.h) * 0.5f,
         static_cast<float>(counter.w), static_cast<float>(counter.h), grey, counter.tex);
    y += fh + pad * 0.8f;
    rect(win_w, win_h, px + pad, y, static_cast<float>(hint.w), static_cast<float>(hint.h), grey, hint.tex);

    glDisable(GL_BLEND);
    glBindVertexArray(0);
    glUseProgram(0);
    return true;
}
