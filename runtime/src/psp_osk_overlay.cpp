// On-screen keyboard, drawn over the presented frame while the game waits on
// sceUtilityOsk (psp_osk_session.h). Main (GL) thread only. The key grid comes
// from psp_osk_keyboard.h; colours, radius, gap and the optional font from a
// skin JSON (psp_osk_skin.h, PSPRECOMP_OSK_SKIN). Text is rasterized with
// stb_truetype from a system font; PSPRECOMP_OSK_FONT=<.ttf> overrides the
// font file. Code points the main font lacks (the hint's Ⓐ Ⓑ Ⓧ Ⓨ, ⌫, ⇧) are
// drawn with a symbol fallback font when one exists on the host.
#include "psp_osk_overlay.h"
#include "psp_osk_session.h"
#include "psp_osk_skin.h"

#include <SDL.h>
#include <glad/glad.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

// Defined below: the skin is read once per process (PSPRECOMP_OSK_SKIN).
const OskSkin& skin();

namespace {

struct Font {
    bool tried = false;
    bool ok = false;
    std::vector<unsigned char> data;
    stbtt_fontinfo info{};
};

// Loads one font file into `f`; leaves `f.ok` false when it cannot be read.
bool load_font(Font& f, const std::string& path) {
    FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) return false;
    std::fseek(fp, 0, SEEK_END);
    const long n = std::ftell(fp);
    std::fseek(fp, 0, SEEK_SET);
    f.data.resize(n > 0 ? static_cast<size_t>(n) : 0);
    const size_t got = n > 0 ? std::fread(f.data.data(), 1, f.data.size(), fp) : 0;
    std::fclose(fp);
    if (got == f.data.size() && got > 0 &&
        stbtt_InitFont(&f.info, f.data.data(), stbtt_GetFontOffsetForIndex(f.data.data(), 0))) {
        f.ok = true;
    }
    return f.ok;
}

Font& font() {
    static Font f;
    if (f.tried) return f;
    f.tried = true;
    std::vector<std::string> paths;
    if (!skin().font.empty()) paths.push_back(skin().font);
    if (const char* e = std::getenv("PSPRECOMP_OSK_FONT")) paths.push_back(e);
    paths.push_back("C:/Windows/Fonts/segoeui.ttf");
    paths.push_back("C:/Windows/Fonts/arial.ttf");
    paths.push_back("/System/Library/Fonts/Supplemental/Arial.ttf");
    paths.push_back("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf");
    for (const std::string& p : paths) {
        if (load_font(f, p)) {
            std::fprintf(stderr, "[OSK] font %s\n", p.c_str());
            return f;
        }
    }
    std::fprintf(stderr, "[OSK] no font found; set PSPRECOMP_OSK_FONT=<file.ttf>\n");
    return f;
}

// Symbol fallback for glyphs the main font lacks (Segoe UI has no U+232B ⌫,
// U+21E7 ⇧ or U+24B6.. Ⓐ Ⓑ Ⓧ Ⓨ). Silent when no fallback exists.
Font& fallback_font() {
    static Font f;
    if (f.tried) return f;
    f.tried = true;
    const char* const paths[] = {"C:/Windows/Fonts/seguisym.ttf",
                                 "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"};
    for (const char* p : paths) {
        if (load_font(f, p)) return f;
    }
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

// Byte offset after `n` code points (for splitting the text at the cursor).
size_t utf8_prefix_bytes(const std::string& s, size_t n) {
    size_t i = 0, cps = 0;
    while (i < s.size() && cps < n) {
        const unsigned char b = static_cast<unsigned char>(s[i]);
        const size_t len = b < 0x80 ? 1 : (b >> 5) == 6 ? 2 : (b >> 4) == 14 ? 3 : (b >> 3) == 30 ? 4 : 1;
        i += std::min(len, s.size() - i);
        cps++;
    }
    return i;
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
    Font& fb = fallback_font();
    const float scale = stbtt_ScaleForPixelHeight(&f.info, px);
    const float fb_scale = fb.ok ? stbtt_ScaleForPixelHeight(&fb.info, px) : 0.0f;
    int ascent = 0, descent = 0, gap = 0;
    stbtt_GetFontVMetrics(&f.info, &ascent, &descent, &gap);
    const int base = static_cast<int>(std::lround(ascent * scale));
    const std::vector<int> cps = codepoints(utf8);

    // Each code point is drawn with the main font, or with the fallback when the
    // main font has no glyph for it. Fallback glyphs share the main baseline.
    std::vector<stbtt_fontinfo*> infos(cps.size(), &f.info);
    std::vector<float> scales(cps.size(), scale);
    for (size_t k = 0; k < cps.size(); k++) {
        if (fb.ok && stbtt_FindGlyphIndex(&f.info, cps[k]) == 0) {
            infos[k] = &fb.info;
            scales[k] = fb_scale;
        }
    }

    // Pen positions and glyph boxes first: a glyph may reach left of its pen (the
    // hook of "j") or past the line box (fallback symbols), so the bitmap covers
    // the union of the line box and every glyph box.
    struct Glyph { int x, y, w, h; };
    std::vector<Glyph> glyphs(cps.size());
    int min_x = 0, min_y = 0, max_x = 0, max_y = static_cast<int>(std::ceil((ascent - descent) * scale));
    float x = 0.0f;
    for (size_t k = 0; k < cps.size(); k++) {
        int adv = 0, lsb = 0, x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        stbtt_GetCodepointHMetrics(infos[k], cps[k], &adv, &lsb);
        stbtt_GetCodepointBitmapBox(infos[k], cps[k], scales[k], scales[k], &x0, &y0, &x1, &y1);
        glyphs[k] = {static_cast<int>(std::lround(x)) + x0, base + y0, x1 - x0, y1 - y0};
        if (glyphs[k].w > 0 && glyphs[k].h > 0) {
            min_x = std::min(min_x, glyphs[k].x);
            min_y = std::min(min_y, glyphs[k].y);
            max_x = std::max(max_x, glyphs[k].x + glyphs[k].w);
            max_y = std::max(max_y, glyphs[k].y + glyphs[k].h);
        }
        x += adv * scales[k];
        if (k + 1 < cps.size() && infos[k] == infos[k + 1])
            x += stbtt_GetCodepointKernAdvance(infos[k], cps[k], cps[k + 1]) * scales[k];
    }
    max_x = std::max(max_x, static_cast<int>(std::ceil(x)));
    img.w = max_x - min_x + 2;
    img.h = max_y - min_y + 2;
    img.a.assign(static_cast<size_t>(img.w) * img.h, 0);
    for (size_t k = 0; k < cps.size(); k++) {
        const Glyph& g = glyphs[k];
        if (g.w <= 0 || g.h <= 0) continue;
        const int gx = g.x - min_x + 1, gy = g.y - min_y + 1;
        stbtt_MakeCodepointBitmap(infos[k], &img.a[static_cast<size_t>(gy) * img.w + gx], g.w, g.h, img.w,
                                  scales[k], scales[k], cps[k]);
    }
    return img;
}

struct Gl {
    bool ready = false;
    GLuint prog = 0, vao = 0, vbo = 0;
    GLint u_color = -1, u_textured = -1, u_tex = -1, u_size = -1, u_radius = -1;
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
        "uniform vec2 u_size; uniform float u_radius;\n"
        "in vec2 v_uv; out vec4 o;\n"
        "void main(){ float a = u_textured != 0 ? texture(u_tex, v_uv).r : 1.0;"
        " vec2 p = v_uv * u_size;"
        " vec2 q = abs(p - u_size * 0.5) - (u_size * 0.5 - vec2(u_radius));"
        " float d = length(max(q, 0.0)) - u_radius;"
        " float cov = u_radius > 0.0 ? clamp(0.5 - d, 0.0, 1.0) : 1.0;"
        " o = vec4(u_color.rgb, u_color.a * a * cov); }\n";
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
    g.u_size = glGetUniformLocation(g.prog, "u_size");
    g.u_radius = glGetUniformLocation(g.prog, "u_radius");
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

// Draw a window-space rectangle (top-left origin, pixels). `radius` rounds the
// corners with a 1 px antialiased edge; text quads pass 0.
void rect(int win_w, int win_h, float x, float y, float w, float h, const float c[4], GLuint tex,
          float radius = 0.0f) {
    Gl& g = gl();
    const float x0 = x / win_w * 2.0f - 1.0f, x1 = (x + w) / win_w * 2.0f - 1.0f;
    const float y0 = 1.0f - y / win_h * 2.0f, y1 = 1.0f - (y + h) / win_h * 2.0f;
    const float v[24] = {x0, y0, 0, 0, x1, y0, 1, 0, x1, y1, 1, 1,
                         x0, y0, 0, 0, x1, y1, 1, 1, x0, y1, 0, 1};
    glBindBuffer(GL_ARRAY_BUFFER, g.vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(v), v);
    glUniform4fv(g.u_color, 1, c);
    glUniform1i(g.u_textured, tex != 0 ? 1 : 0);
    glUniform2f(g.u_size, w, h);
    glUniform1f(g.u_radius, radius > 0.0f ? std::min(radius, std::min(w, h) * 0.5f) : 0.0f);
    if (tex != 0) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex);
        glUniform1i(g.u_tex, 0);
    }
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

// Skin colour to the vec4 the shader expects, with the skin-wide opacity.
void rgba(const OskColor& c, float opacity, float out[4]) {
    out[0] = c.r;
    out[1] = c.g;
    out[2] = c.b;
    out[3] = c.a * opacity;
}

bool ui_english() {
    const char* lang = std::getenv("PSPRECOMP_UI_LANG");
    return lang && std::string(lang) == "en";
}

}  // namespace

// The keyboard model the event loop drives and this overlay draws (main thread only).
OskKeyboard& psp_osk_keyboard_ref() {
    static OskKeyboard k(ui_english() ? "en" : "es");
    return k;
}

const OskSkin& skin() {
    static OskSkin s;
    static bool loaded = false;
    if (loaded) return s;
    loaded = true;
    const char* path = std::getenv("PSPRECOMP_OSK_SKIN");
    if (!path || !*path) return s;
    FILE* fp = std::fopen(path, "rb");
    if (!fp) {
        std::fprintf(stderr, "[OSK] skin %s not found: built-in Deck skin\n", path);
        return s;
    }
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, fp)) > 0) text.append(buf, n);
    std::fclose(fp);
    std::string dir = path;
    const size_t slash = dir.find_last_of("/\\");
    dir = slash == std::string::npos ? "." : dir.substr(0, slash);
    std::vector<std::string> log;
    s = osk_skin_parse(text, dir, &log);
    for (const std::string& l : log) std::fprintf(stderr, "%s\n", l.c_str());
    std::fprintf(stderr, "[OSK] skin %s (%s)\n", s.name.c_str(), path);
    return s;
}

OskKeyboard& psp_osk_keyboard() {
    return psp_osk_keyboard_ref();
}

void psp_osk_keyboard_reset() {
    psp_osk_keyboard_ref() = OskKeyboard(ui_english() ? "en" : "es");
}

bool psp_osk_overlay_draw(int win_w, int win_h, int rx, int ry, int rw, int rh) {
    const OskSession::View v = osk_session().view();
    if (!v.active || win_w <= 0 || win_h <= 0 || rw <= 0 || rh <= 0) return false;
    const OskSkin& sk = skin();
    OskKeyboard& kb = psp_osk_keyboard();
    static TextTex title, before, after, counter, hint;
    static std::map<std::string, TextTex> labels;

    // Game rectangle in top-left window coordinates (ry is GL bottom-left).
    const float gx = static_cast<float>(rx);
    const float gy = static_cast<float>(win_h - (ry + rh));
    const float pad = rh * 0.028f;
    const float gap = sk.gap * rh / 1080.0f;
    const float radius = sk.radius * rh / 1080.0f;
    const float row_h = rh * 0.085f;
    const int title_px = static_cast<int>(rh * 0.055f);
    const int text_px = static_cast<int>(rh * 0.070f);
    const int small_px = static_cast<int>(rh * 0.040f);
    const int label_px = static_cast<int>(rh * 0.045f);
    const int hint_px = static_cast<int>(rh * 0.030f);

    // Text split at the cursor (code-point index) into two rasterized pieces.
    const size_t cut = utf8_prefix_bytes(v.text, v.cursor);
    const std::string pre = v.text.substr(0, cut), post = v.text.substr(cut);
    update_text(title, v.title.empty() ? std::string("Texto") : v.title, title_px);
    update_text(before, pre, text_px);
    update_text(after, post, text_px);
    update_text(counter, std::to_string(v.length) + "/" + std::to_string(v.max_len), small_px);
    update_text(hint, ui_english()
                          ? "Ⓐ type   Ⓑ delete   Ⓧ shift   Ⓨ space   LB/RB cursor   Select symbols   Start OK"
                          : "Ⓐ escribir   Ⓑ borrar   Ⓧ mayús   Ⓨ espacio   LB/RB cursor   Select símbolos   Start aceptar",
               hint_px);
    const bool blink = (SDL_GetTicks() / 500) % 2 == 0;

    // Panel: 92 % of the game width, anchored to its bottom, as tall as its content.
    const float fh = static_cast<float>(std::max(before.h, after.h)) + pad * 0.5f;
    const float pw = rw * 0.92f;
    const float ph = std::min(rh * 0.95f, pad + title.h + pad * 0.5f + fh + pad * 0.6f +
                                              row_h * kb.rows().size() + pad * 0.4f + hint.h + pad);
    const float px = gx + (rw - pw) * 0.5f, py = gy + rh - ph;

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

    float c[4];
    rgba(sk.background, sk.opacity, c);
    rect(win_w, win_h, gx, gy, static_cast<float>(rw), static_cast<float>(rh), c, 0);
    rgba(sk.panel, sk.opacity, c);
    rect(win_w, win_h, px, py, pw, ph, c, 0, radius);

    float y = py + pad;
    rgba(sk.accent, sk.opacity, c);
    rect(win_w, win_h, px + pad, y, static_cast<float>(title.w), static_cast<float>(title.h), c, title.tex);
    y += title.h + pad * 0.5f;

    rgba(sk.key, sk.opacity, c);
    rect(win_w, win_h, px + pad, y, pw - 2 * pad, fh, c, 0, radius * 0.6f);
    const float tx = px + pad * 1.5f, ty = y + (fh - before.h) * 0.5f;
    rgba(sk.key_text, sk.opacity, c);
    rect(win_w, win_h, tx, ty, static_cast<float>(before.w), static_cast<float>(before.h), c, before.tex);
    if (blink) {
        rgba(sk.focus, sk.opacity, c);
        rect(win_w, win_h, tx + before.w, ty, 2.0f, static_cast<float>(before.h), c, 0);
    }
    rgba(sk.key_text, sk.opacity, c);
    rect(win_w, win_h, tx + before.w + 2.0f, ty, static_cast<float>(after.w), static_cast<float>(after.h), c,
         after.tex);
    rgba(sk.key_text, sk.opacity, c);
    rect(win_w, win_h, px + pw - pad * 1.5f - counter.w, y + (fh - counter.h) * 0.5f,
         static_cast<float>(counter.w), static_cast<float>(counter.h), c, counter.tex);
    y += fh + pad * 0.6f;

    // Key grid: rows left to right, widths proportional to OskKey::width.
    const auto& rows = kb.rows();
    const float inner_w = pw - 2 * pad;
    for (size_t r = 0; r < rows.size(); r++) {
        const auto& row = rows[r];
        const int n = static_cast<int>(row.size());
        float units = 0.0f;
        for (const OskKey& k : row) units += k.width;
        const float unit_w = (inner_w - gap * (n - 1)) / (units > 0.0f ? units : 1.0f);
        float kx = px + pad;
        for (int i = 0; i < n; i++) {
            const OskKey& k = row[i];
            const float kw = k.width * unit_w, kh = row_h - gap;
            const bool focused = static_cast<int>(r) == kb.focus_row() && i == kb.focus_col();
            const bool shift_on = k.kind == OskKeyKind::Shift && (kb.shift() || kb.caps());
            const bool layer_on = k.kind == OskKeyKind::Layer && kb.symbols();
            const bool accent_on = k.kind == OskKeyKind::Accent && !kb.pending_accent().empty();
            if (focused) rgba(sk.focus, sk.opacity, c);
            else if (shift_on || layer_on || accent_on) rgba(sk.accent, sk.opacity, c);
            else rgba(sk.key, sk.opacity, c);
            rect(win_w, win_h, kx, y, kw, kh, c, 0, radius * 0.6f);
            const std::string cache_key = k.label + "@" + std::to_string(label_px);
            TextTex& t = labels[cache_key];
            update_text(t, k.label, label_px);
            rgba(focused ? sk.focus_text : sk.key_text, sk.opacity, c);
            rect(win_w, win_h, kx + (kw - t.w) * 0.5f, y + (kh - t.h) * 0.5f, static_cast<float>(t.w),
                 static_cast<float>(t.h), c, t.tex);
            kx += kw + gap;
        }
        y += row_h;
    }

    // The hint sits on the panel, not on a key: light or dark grey by the panel's luminance.
    const float lum = 0.2126f * sk.panel.r + 0.7152f * sk.panel.g + 0.0722f * sk.panel.b;
    const OskColor hint_col = lum < 0.5f ? OskColor{0.78f, 0.80f, 0.84f, 1.0f} : OskColor{0.30f, 0.32f, 0.36f, 1.0f};
    rgba(hint_col, sk.opacity, c);
    rect(win_w, win_h, px + (pw - hint.w) * 0.5f, py + ph - pad - hint.h, static_cast<float>(hint.w),
         static_cast<float>(hint.h), c, hint.tex);

    glDisable(GL_BLEND);
    glBindVertexArray(0);
    glUseProgram(0);
    return true;
}

MenuPrompt& psp_menu_prompt() {
    static MenuPrompt p;
    return p;
}

bool psp_menu_prompt_overlay_draw(int win_w, int win_h, int rx, int ry, int rw, int rh) {
    MenuPrompt& p = psp_menu_prompt();
    if (!p.active() || win_w <= 0 || win_h <= 0 || rw <= 0 || rh <= 0) return false;
    static TextTex question, yes, no, hint;
    const PromptText t = menu_prompt_text(std::getenv("PSPRECOMP_UI_LANG"));
    static const PromptColors col = menu_prompt_colors(std::getenv("PSPRECOMP_UI_COLORS"));
    const float gx = static_cast<float>(rx), gy = static_cast<float>(win_h - (ry + rh));
    const float pw = rw * 0.70f, ph = rh * 0.34f;
    const float px = gx + (rw - pw) * 0.5f, py = gy + (rh - ph) * 0.5f, pad = rh * 0.035f;
    update_text(question, t.question, static_cast<int>(rh * 0.060f));
    update_text(yes, t.yes, static_cast<int>(rh * 0.070f));
    update_text(no, t.no, static_cast<int>(rh * 0.070f));
    update_text(hint, t.hint, static_cast<int>(rh * 0.040f));
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
    const float shade[4] = {0.0f, 0.0f, 0.0f, 0.55f};
    rect(win_w, win_h, gx, gy, static_cast<float>(rw), static_cast<float>(rh), shade, 0);
    rect(win_w, win_h, px, py, pw, ph, col.panel, 0);
    rect(win_w, win_h, px, py, pw, rh * 0.008f, col.accent, 0);
    float y = py + pad;
    rect(win_w, win_h, px + (pw - question.w) * 0.5f, y, static_cast<float>(question.w),
         static_cast<float>(question.h), col.text, question.tex);
    y += question.h + pad;
    const float bw = pw * 0.30f, bh = yes.h + pad * 0.6f;
    const float yx = px + pw * 0.5f - bw - pad, nx = px + pw * 0.5f + pad;
    rect(win_w, win_h, p.yes_selected() ? yx : nx, y, bw, bh, col.choice_bg, 0);
    rect(win_w, win_h, yx + (bw - yes.w) * 0.5f, y + pad * 0.3f, static_cast<float>(yes.w),
         static_cast<float>(yes.h), p.yes_selected() ? col.choice_text : col.other_text, yes.tex);
    rect(win_w, win_h, nx + (bw - no.w) * 0.5f, y + pad * 0.3f, static_cast<float>(no.w),
         static_cast<float>(no.h), p.yes_selected() ? col.other_text : col.choice_text, no.tex);
    y += bh + pad;
    rect(win_w, win_h, px + (pw - hint.w) * 0.5f, y, static_cast<float>(hint.w),
         static_cast<float>(hint.h), col.hint, hint.tex);
    glDisable(GL_BLEND);
    glBindVertexArray(0);
    glUseProgram(0);
    return true;
}
