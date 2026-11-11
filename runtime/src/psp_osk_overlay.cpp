// On-screen keyboard, drawn over the presented frame while the game waits on
// sceUtilityOsk (psp_osk_session.h). Main (GL) thread only. The key grid comes
// from psp_osk_keyboard.h; colours, radius, gap and the optional font from a
// skin JSON (psp_osk_skin.h, PSPRECOMP_OSK_SKIN). Text is rasterized with
// stb_truetype from the project font (<exe>/assets/fonts/) or a system font; PSPRECOMP_OSK_FONT=<.ttf> overrides the
// font file. Code points the main font lacks (the hint's Ⓐ Ⓑ Ⓧ Ⓨ, ⌫, ⇧) are
// drawn with a symbol fallback font when one exists on the host.
#include "psp_osk_overlay.h"
#include "psp_gl_api.h"
#include "psp_icon_strip.h"
#include "psp_osk_session.h"
#include "psp_osk_skin.h"
#include "psp_texrep_png.h"

#include <SDL.h>
#include <glad/glad.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <vector>

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
    std::ifstream in(std::filesystem::u8path(path), std::ios::binary);  // UTF-8 paths (accented folders)
    if (!in) return false;
    f.data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    if (!f.data.empty() &&
        stbtt_InitFont(&f.info, f.data.data(), stbtt_GetFontOffsetForIndex(f.data.data(), 0))) {
        f.ok = true;
    }
    return f.ok;
}

Font& font() {
    static Font f;
    static std::once_flag once;
    std::call_once(once, [] {
        f.tried = true;
        std::vector<std::string> paths;
        if (!skin().font.empty()) paths.push_back(skin().font);
        if (const char* e = std::getenv("PSPRECOMP_OSK_FONT")) paths.push_back(e);
        if (char* base = SDL_GetBasePath()) {  // the project font shipped in <exe>/assets/fonts/
            const std::string ui = psp_ui_font_path(base);
            SDL_free(base);
            if (!ui.empty()) paths.push_back(ui);
        }
        paths.push_back("C:/Windows/Fonts/segoeui.ttf");
        paths.push_back("C:/Windows/Fonts/arial.ttf");
        paths.push_back("/System/Library/Fonts/Supplemental/Arial.ttf");
        paths.push_back("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf");
        for (const std::string& p : paths) {
            if (load_font(f, p)) {
                std::fprintf(stderr, "[OSK] font %s\n", p.c_str());
                return;
            }
        }
        std::fprintf(stderr, "[OSK] no font found; set PSPRECOMP_OSK_FONT=<file.ttf>\n");
    });
    return f;
}

// Symbol fallback for glyphs the main font lacks (Segoe UI has no U+232B ⌫,
// U+21E7 ⇧ or U+24B6.. Ⓐ Ⓑ Ⓧ Ⓨ). Silent when no fallback exists.
Font& fallback_font() {
    static Font f;
    static std::once_flag once;
    std::call_once(once, [] {
        f.tried = true;
        const char* const paths[] = {"C:/Windows/Fonts/seguisym.ttf",
                                     "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"};
        for (const char* p : paths) {
            if (load_font(f, p)) return;
        }
    });
    return f;
}

// Emoji outlines for achievement icons (Segoe UI Emoji keeps monochrome glyf outlines next to its colour
// layers). Silent when absent: the icon is then skipped by the rasterizer like any missing glyph.
Font& emoji_font() {
    static Font f;
    static std::once_flag once;
    std::call_once(once, [] {
        f.tried = true;
        const char* const paths[] = {"C:/Windows/Fonts/seguiemj.ttf",
                                     "/usr/share/fonts/truetype/noto/NotoEmoji-Regular.ttf"};
        for (const char* p : paths) {
            if (load_font(f, p)) return;
        }
    });
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
    Font& em = emoji_font();
    const float scale = stbtt_ScaleForPixelHeight(&f.info, px);
    const float fb_scale = fb.ok ? stbtt_ScaleForPixelHeight(&fb.info, px) : 0.0f;
    const float em_scale = em.ok ? stbtt_ScaleForPixelHeight(&em.info, px) : 0.0f;
    int ascent = 0, descent = 0, gap = 0;
    stbtt_GetFontVMetrics(&f.info, &ascent, &descent, &gap);
    const int base = static_cast<int>(std::lround(ascent * scale));
    const std::vector<int> cps = codepoints(utf8);

    // Each code point is drawn with the main font; a letter it lacks is composed from its own base letter and
    // spacing accent (psp_glyph_parts: the project font has "´" and "e" but no "é"), else drawn with the fallback
    // font. Every part shares the main baseline.
    struct Part { stbtt_fontinfo* info; float scale; int cp, x, y, w, h; bool flip; };
    std::vector<Part> parts;
    auto has = [](Font& ft, int cp) { return ft.ok && stbtt_FindGlyphIndex(&ft.info, cp) != 0; };
    auto place = [&](stbtt_fontinfo* in, float sc, int cp, float pen) {
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        stbtt_GetCodepointBitmapBox(in, cp, sc, sc, &x0, &y0, &x1, &y1);
        return Part{in, sc, cp, static_cast<int>(std::lround(pen)) + x0, base + y0, x1 - x0, y1 - y0, false};
    };
    const int mark_gap = std::max(1, static_cast<int>(std::lround(px * 0.08f)));
    float x = 0.0f;
    for (size_t k = 0; k < cps.size(); k++) {
        stbtt_fontinfo* in = &f.info;
        float sc = scale;
        int cp = cps[k];
        GlyphParts gp{0, 0, false};
        bool composed = false;
        if (!has(f, cp)) {
            if (psp_glyph_parts(cp, &gp) && has(f, gp.base) && (gp.mark == 0 || has(f, gp.mark))) composed = true;
            else if (has(fb, cp)) { in = &fb.info; sc = fb_scale; }
            else if (has(em, cp)) { in = &em.info; sc = em_scale; }
            else if (fb.ok) { in = &fb.info; sc = fb_scale; }  // as before: the symbol font's notdef
        }
        if (composed) cp = gp.base;
        Part body = place(in, sc, cp, x);
        if (composed && gp.flip) {  // hangs from about three quarters of the cap height down past the baseline
            body.flip = true;
            body.y = base + static_cast<int>(std::lround((body.y - base) * 0.72f));
        }
        parts.push_back(body);
        if (composed && gp.mark) {  // centred over the letter, clear of it
            Part m = place(in, sc, gp.mark, x);
            m.x = body.x + (body.w - m.w) / 2;
            if (m.y + m.h > body.y - mark_gap) m.y = body.y - mark_gap - m.h;
            parts.push_back(m);
        }
        int adv = 0, lsb = 0;
        stbtt_GetCodepointHMetrics(in, cp, &adv, &lsb);
        x += adv * sc;
        if (k + 1 < cps.size() && in == &f.info && has(f, cps[k + 1]))
            x += stbtt_GetCodepointKernAdvance(in, cp, cps[k + 1]) * sc;
    }

    // The bitmap covers the union of the line box and every part (a glyph may reach left of its pen, like the hook
    // of "j", or above the line, like the accent of "É").
    int min_x = 0, min_y = 0, max_x = static_cast<int>(std::ceil(x)), max_y = static_cast<int>(std::ceil((ascent - descent) * scale));
    for (const Part& q : parts) {
        if (q.w <= 0 || q.h <= 0) continue;
        min_x = std::min(min_x, q.x);
        min_y = std::min(min_y, q.y);
        max_x = std::max(max_x, q.x + q.w);
        max_y = std::max(max_y, q.y + q.h);
    }
    img.w = max_x - min_x + 2;
    img.h = max_y - min_y + 2;
    img.a.assign(static_cast<size_t>(img.w) * img.h, 0);
    std::vector<unsigned char> tmp;
    for (const Part& q : parts) {  // each part on its own, then max-blended: parts may overlap (the dot of "í")
        if (q.w <= 0 || q.h <= 0) continue;
        tmp.assign(static_cast<size_t>(q.w) * q.h, 0);
        stbtt_MakeCodepointBitmap(q.info, tmp.data(), q.w, q.h, q.w, q.scale, q.scale, q.cp);
        const int ox = q.x - min_x + 1, oy = q.y - min_y + 1;
        for (int r = 0; r < q.h; r++)
            for (int c = 0; c < q.w; c++) {
                const unsigned char v = q.flip ? tmp[static_cast<size_t>(q.h - 1 - r) * q.w + (q.w - 1 - c)]
                                               : tmp[static_cast<size_t>(r) * q.w + c];
                unsigned char& d = img.a[static_cast<size_t>(oy + r) * img.w + ox + c];
                d = std::max(d, v);
            }
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
    const char* srcs[2] = {psp_glsl_header(psp_gl_api(), type == GL_FRAGMENT_SHADER), src};
    glShaderSource(s, 2, srcs, nullptr);
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
        "layout(location=0) in vec2 a_pos; layout(location=1) in vec2 a_uv;\n"
        "out vec2 v_uv; void main(){ gl_Position = vec4(a_pos, 0.0, 1.0); v_uv = a_uv; }\n";
    const char* fs =
        "uniform sampler2D u_tex; uniform vec4 u_color; uniform int u_textured;\n"
        "uniform vec2 u_size; uniform float u_radius;\n"
        "in vec2 v_uv; out vec4 o;\n"
        // u_textured: 0 flat colour, 1 text (red = coverage), 2 RGBA image tinted by multiply (tag icons)
        "void main(){ vec4 t = u_textured != 0 ? texture(u_tex, v_uv) : vec4(1.0);"
        " float a = u_textured == 1 ? t.r : t.a;"
        " vec3 rgb = u_textured == 2 ? t.rgb * u_color.rgb : u_color.rgb;"
        " vec2 p = v_uv * u_size;"
        " vec2 q = abs(p - u_size * 0.5) - (u_size * 0.5 - vec2(u_radius));"
        " float d = length(max(q, 0.0)) - u_radius;"
        " float cov = u_radius > 0.0 ? clamp(0.5 - d, 0.0, 1.0) : 1.0;"
        " o = vec4(rgb, u_color.a * a * cov); }\n";
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
// corners with a 1 px antialiased edge; text quads pass 0. `image`: tex is RGBA, tinted by c (else a text mask).
void rect(int win_w, int win_h, float x, float y, float w, float h, const float c[4], GLuint tex,
          float radius = 0.0f, bool image = false) {
    Gl& g = gl();
    const float x0 = x / win_w * 2.0f - 1.0f, x1 = (x + w) / win_w * 2.0f - 1.0f;
    const float y0 = 1.0f - y / win_h * 2.0f, y1 = 1.0f - (y + h) / win_h * 2.0f;
    const float v[24] = {x0, y0, 0, 0, x1, y0, 1, 0, x1, y1, 1, 1,
                         x0, y0, 0, 0, x1, y1, 1, 1, x0, y1, 0, 1};
    glBindBuffer(GL_ARRAY_BUFFER, g.vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(v), v);
    glUniform4fv(g.u_color, 1, c);
    glUniform1i(g.u_textured, tex == 0 ? 0 : image ? 2 : 1);
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
    static std::once_flag once;
    std::call_once(once, [] {
        const char* path = std::getenv("PSPRECOMP_OSK_SKIN");
        if (!path || !*path) return;
        FILE* fp = std::fopen(path, "rb");
        if (!fp) {
            std::fprintf(stderr, "[OSK] skin %s not found: built-in Deck skin\n", path);
            return;
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
    });
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
                          ? "Ⓐ type   Ⓑ delete / cancel   Ⓧ shift   Ⓨ space   LB/RB cursor   Select symbols   Start OK"
                          : "Ⓐ escribir   Ⓑ borrar / cancelar   Ⓧ mayús   Ⓨ espacio   LB/RB cursor   Select símbolos   Start aceptar",
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

namespace {
// A toast as the worker prepared it: text bitmaps and, for an achievement card, the decoded image.
struct ToastImages {
    TextImage title, name, desc;
    int iw = 0, ih = 0;
    std::vector<uint8_t> rgba;  // card image, RGBA8; empty = medallion without image
    uint32_t rgb = 0;           // 0 = plain toast
    int64_t ms = kToastMs;
};
struct ToastTex { GLuint tex = 0; int w = 0, h = 0; };
struct ToastShown {
    ToastTex title, name, desc, image;
    uint32_t rgb = 0;
    int64_t ms = kToastMs, start_ms = -1;
};
std::mutex g_toast_mu;
std::deque<ToastImages> g_toast_in;          // posted, not uploaded yet (guarded by g_toast_mu)
std::atomic<int> g_toast_posted{0};          // > 0: g_toast_in has something
std::atomic<int> g_toast_rh{720};            // last game image height, for the raster size

void tex_params(GLint min_filter) {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, min_filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

ToastTex upload(const TextImage& img) {
    ToastTex t{0, img.w, img.h};
    glGenTextures(1, &t.tex);
    glBindTexture(GL_TEXTURE_2D, t.tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, img.w > 0 ? img.w : 1, img.h > 0 ? img.h : 1, 0, GL_RED,
                 GL_UNSIGNED_BYTE, img.a.empty() ? nullptr : img.a.data());
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    tex_params(GL_LINEAR);
    return t;
}

// The 512 px image is drawn at ~100 px: mipmapped so it does not shimmer.
ToastTex upload_image(const ToastImages& im) {
    ToastTex t{0, im.iw, im.ih};
    if (im.rgba.empty()) return t;
    glGenTextures(1, &t.tex);
    glBindTexture(GL_TEXTURE_2D, t.tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, im.iw, im.ih, 0, GL_RGBA, GL_UNSIGNED_BYTE, im.rgba.data());
    glGenerateMipmap(GL_TEXTURE_2D);
    tex_params(GL_LINEAR_MIPMAP_LINEAR);
    return t;
}

void free_toast(ToastShown& s) {
    for (ToastTex* t : {&s.title, &s.name, &s.desc, &s.image})
        if (t->tex) glDeleteTextures(1, &t->tex);
}

// Reads and decodes `path` (PNG or JPEG) into `im`; false when it cannot.
bool load_image(const std::string& path, ToastImages& im) {
    std::ifstream in(std::filesystem::u8path(path), std::ios::binary);
    if (!in) return false;
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return ge_texrep_decode_png(bytes.data(), bytes.size(), &im.iw, &im.ih, &im.rgba);
}

void colour(uint32_t rgb, float a, float out[4]) {
    out[0] = ((rgb >> 16) & 0xFF) / 255.0f;
    out[1] = ((rgb >> 8) & 0xFF) / 255.0f;
    out[2] = (rgb & 0xFF) / 255.0f;
    out[3] = a;
}

// Card colours (spec 2026-11-08 section 3).
constexpr uint32_t kCream = 0xFCEBD4u, kRule = 0x1A120Cu, kBody = 0x2B1D14u, kShadow = 0x120C08u;

// The achievement card (option B): cream card, tier-colour ribbon across the top, medallion with the image
// centred on the card's left edge, name in the tier colour, black rule, description. Sizes in u = rh / 720.
void draw_card(int win_w, int win_h, float gx, float gy, int rw, int rh, const ToastShown& s, int64_t elapsed) {
    const ToastPose p = psp_toast_pose(elapsed, s.ms);
    if (p.alpha <= 0.0f) return;
    const float u = rh / 720.0f;
    const float d = 128.0f * u;  // medallion diameter
    const float pad_l = d * 0.5f + 26.0f * u, pad_r = 26.0f * u, pad_t = 30.0f * u, pad_b = 18.0f * u;
    const float gap = 10.0f * u, rule_h = 3.0f * u, overhang = 12.0f * u;
    const float text_w = static_cast<float>(std::max({s.name.w, s.desc.w, s.title.w}));
    const float cw = std::max(rw * 0.30f, pad_l + text_w + pad_r);
    const float ch = std::max(pad_t + s.name.h + gap + rule_h + gap + s.desc.h + pad_b, d * 0.92f);
    const float margin = rh * 0.03f, ribbon_h = s.title.h + 8.0f * u, ribbon_up = ribbon_h * 0.45f;
    const float cx = gx + rw - margin - overhang - cw;
    const float cy = gy + margin + ribbon_up + p.drop * (ch + margin + ribbon_up);
    float c[4];
    colour(kShadow, 0.55f * p.alpha, c);
    rect(win_w, win_h, cx, cy + 6.0f * u, cw, ch, c, 0, 22.0f * u);
    colour(kCream, p.alpha, c);
    rect(win_w, win_h, cx, cy, cw, ch, c, 0, 22.0f * u);
    if (p.ribbon > 0.0f) {  // unfurls left to right
        colour(s.rgb, p.alpha, c);
        rect(win_w, win_h, cx, cy - ribbon_up, (cw + overhang) * p.ribbon, ribbon_h, c, 0, 6.0f * u);
        // The text is cut at the ribbon's unfurled end (scissor in bottom-left window pixels).
        glEnable(GL_SCISSOR_TEST);
        glScissor(static_cast<GLint>(cx), static_cast<GLint>(win_h - (cy - ribbon_up + ribbon_h)),
                  static_cast<GLsizei>(std::ceil((cw + overhang) * p.ribbon)), static_cast<GLsizei>(std::ceil(ribbon_h)));
        colour(kCream, p.alpha, c);
        rect(win_w, win_h, cx + pad_l, cy - ribbon_up + (ribbon_h - s.title.h) * 0.5f, static_cast<float>(s.title.w),
             static_cast<float>(s.title.h), c, s.title.tex);
        glDisable(GL_SCISSOR_TEST);
    }
    float y = cy + pad_t;
    colour(s.rgb, p.alpha, c);
    rect(win_w, win_h, cx + pad_l, y, static_cast<float>(s.name.w), static_cast<float>(s.name.h), c, s.name.tex);
    y += s.name.h + gap;
    colour(kRule, p.alpha, c);
    if (p.rule > 0.0f) rect(win_w, win_h, cx, y, cw * p.rule, rule_h, c, 0);
    y += rule_h + gap;
    colour(kBody, p.alpha, c);
    if (s.desc.w > 0)
        rect(win_w, win_h, cx + pad_l, y, static_cast<float>(s.desc.w), static_cast<float>(s.desc.h), c, s.desc.tex);
    if (p.medal > 0.0f) {  // pops in about its centre, on top of the ribbon
        const float mx = cx, my = cy + ch * 0.5f, md = d * p.medal;
        auto disc = [&](float dd, const float col[4], GLuint tex) {
            rect(win_w, win_h, mx - dd * 0.5f, my - dd * 0.5f, dd, dd, col, tex, dd * 0.5f, tex != 0);
        };
        colour(s.rgb, p.alpha, c);
        disc(md, c, 0);
        colour(kCream, p.alpha, c);
        disc(md * 0.90f, c, 0);
        colour(0xFFFFFFu, p.alpha, c);
        if (s.image.tex) disc(md * 0.805f, c, s.image.tex);
    }
}
}  // namespace

void psp_toast_post(const ToastText& t) {
    const int rh = g_toast_rh.load(std::memory_order_relaxed);
    ToastImages im;
    im.rgb = t.rgb;
    im.ms = t.ms;
    im.title = rasterize(t.title, rh * 0.036f);
    im.name = rasterize(t.name, rh * 0.050f);
    if (t.rgb != 0) {
        im.desc = rasterize(t.desc, rh * 0.030f);
        if (!load_image(t.image, im)) {
            const std::string fallback = std::filesystem::u8path(t.image).parent_path().u8string() + "/_default.jpg";
            std::fprintf(stderr, "[ACH] image %s unreadable; using _default.jpg\n", t.image.c_str());
            if (!load_image(fallback, im)) im.rgba.clear();
        }
    }
    std::lock_guard<std::mutex> lock(g_toast_mu);
    g_toast_in.push_back(std::move(im));
    g_toast_posted.fetch_add(1, std::memory_order_release);
}

bool psp_toast_overlay_draw(int win_w, int win_h, int rx, int ry, int rw, int rh) {
    static std::deque<ToastShown> shown;
    if (rh > 0) g_toast_rh.store(rh, std::memory_order_relaxed);
    if (g_toast_posted.load(std::memory_order_acquire) > 0) {
        std::deque<ToastImages> in;
        {
            std::lock_guard<std::mutex> lock(g_toast_mu);
            in.swap(g_toast_in);
            g_toast_posted.store(0, std::memory_order_relaxed);
        }
        for (const ToastImages& im : in)
            shown.push_back({upload(im.title), upload(im.name), upload(im.desc), upload_image(im), im.rgb, im.ms, -1});
    }
    if (shown.empty() || win_w <= 0 || win_h <= 0 || rw <= 0 || rh <= 0) return false;
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    if (shown.front().start_ms < 0) shown.front().start_ms = now;
    if (now - shown.front().start_ms >= shown.front().ms) {
        free_toast(shown.front());
        shown.pop_front();
        if (shown.empty()) return false;
        shown.front().start_ms = now;
    }
    const ToastShown& s = shown.front();
    const float gx = static_cast<float>(rx), gy = static_cast<float>(win_h - (ry + rh));
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
    if (s.rgb != 0) {
        draw_card(win_w, win_h, gx, gy, rw, rh, s, now - s.start_ms);
    } else {
        const float a = psp_toast_alpha(now - s.start_ms, s.ms);
        static const PromptColors col = menu_prompt_colors(std::getenv("PSPRECOMP_UI_COLORS"));
        auto faded = [a](const float c[4], float out[4]) { out[0] = c[0]; out[1] = c[1]; out[2] = c[2]; out[3] = c[3] * a; };
        float panel[4], accent[4], hint[4], text[4];
        faded(col.panel, panel);
        faded(col.accent, accent);
        faded(col.hint, hint);
        faded(col.text, text);
        const float pad = rh * 0.022f, margin = rh * 0.03f;
        const float pw = std::max(rw * 0.30f, static_cast<float>(std::max(s.title.w, s.name.w)) + pad * 3.0f);
        const float ph = s.title.h + s.name.h + pad * 2.5f;
        const float px = gx + rw - pw - margin, py = gy + margin;
        rect(win_w, win_h, px, py, pw, ph, panel, 0, pad * 0.6f);
        rect(win_w, win_h, px, py, rh * 0.008f, ph, accent, 0);
        rect(win_w, win_h, px + pad * 1.5f, py + pad, static_cast<float>(s.title.w), static_cast<float>(s.title.h), hint,
             s.title.tex);
        rect(win_w, win_h, px + pad * 1.5f, py + pad * 1.5f + s.title.h, static_cast<float>(s.name.w),
             static_cast<float>(s.name.h), text, s.name.tex);
    }
    glDisable(GL_BLEND);
    glBindVertexArray(0);
    glUseProgram(0);
    return true;
}

namespace {
std::vector<TagRect> g_placed;  // tags drawn this frame (GL thread only)
std::mutex g_icon_mu;
IconStrip g_icon_new;            // decoded by psp_world_tag_set_icon, not uploaded yet (guarded by g_icon_mu)
std::vector<GLuint> g_icon_tex;  // one texture per frame of the uploaded strip (GL thread only)

// GL thread: uploads a newly registered strip, one mipmapped texture per frame (drawn at a few pixels).
void icon_upload() {
    IconStrip s;
    {
        std::lock_guard<std::mutex> l(g_icon_mu);
        if (g_icon_new.frames == 0) return;
        s = std::move(g_icon_new);
        g_icon_new = IconStrip();
    }
    if (!g_icon_tex.empty()) glDeleteTextures(static_cast<GLsizei>(g_icon_tex.size()), g_icon_tex.data());
    g_icon_tex.assign(s.frames, 0);
    glGenTextures(s.frames, g_icon_tex.data());
    const int fw = s.w / s.frames;
    std::vector<uint8_t> frame(static_cast<size_t>(fw) * s.h * 4);
    for (int f = 0; f < s.frames; f++) {
        for (int y = 0; y < s.h; y++)
            std::copy_n(&s.rgba[(static_cast<size_t>(y) * s.w + f * fw) * 4], fw * 4, &frame[static_cast<size_t>(y) * fw * 4]);
        glBindTexture(GL_TEXTURE_2D, g_icon_tex[f]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, fw, s.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, frame.data());
        glGenerateMipmap(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
}
}  // namespace

bool psp_world_tag_set_icon(const std::string& png_path, int frames) {
    IconStrip s;
    if (!psp_icon_strip_load(png_path, frames, &s)) return false;
    std::lock_guard<std::mutex> l(g_icon_mu);
    g_icon_new = std::move(s);
    return true;
}

void psp_world_tags_begin_frame() { g_placed.clear(); }

void psp_world_tag_draw_target(const WorldTag& t, int fb_w, int fb_h) {
    static std::map<std::string, TextTex> cache;  // by text + size; numbers repeat, so it stays small
    if (fb_w <= 0 || fb_h <= 0) return;
    icon_upload();
    const bool icon = t.icon >= 0 && t.icon < static_cast<int>(g_icon_tex.size());
    const int px = t.size_px > 0.0f ? std::max(8, static_cast<int>(std::lround(fb_h * t.size_px / 272.0f)))
                                    : std::max(12, static_cast<int>(std::lround(fb_h * 0.046f)));
    if (cache.size() > 48) {  // a long session of changing numbers: start over
        for (auto& kv : cache) glDeleteTextures(1, &kv.second.tex);
        cache.clear();
    }
    TextTex* text = nullptr;
    if (!t.text.empty()) {
        text = &cache[t.text + "@" + std::to_string(px)];
        update_text(*text, t.text, px);
        if (text->w <= 0) text = nullptr;
    }
    if (!text && !icon) return;
    const float sx = fb_w / 480.0f, sy = fb_h / 272.0f, o = std::max(1.0f, fb_h / 360.0f);
    const float tw = text ? static_cast<float>(text->w) : 0.0f, th = text ? static_cast<float>(text->h) : 0.0f;
    const float iw = icon ? std::round(7.0f * sx) : 0.0f, ih = icon ? std::round(6.0f * sy) : 0.0f;  // 7x6 PSP px
    const float gap = icon && text ? std::round(sx) : 0.0f;
    const float w = iw + gap + tw, h = std::max(th, ih);
    TagRect at = psp_tag_unstack({t.x * sx - w * t.align, t.y * sy - (t.above ? h : 0.0f), w, h}, g_placed, t.above);
    at.x = std::round(at.x);  // whole pixels: the glyph texels land 1:1, sharp at render scale 1
    at.y = std::round(at.y);
    Gl& g = gl();
    glUseProgram(g.prog);
    glBindVertexArray(g.vao);
    glViewport(0, 0, fb_w, fb_h);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBlendEquation(GL_FUNC_ADD);
    const float c[4] = {t.r / 255.0f, t.g / 255.0f, t.b / 255.0f, 1.0f};
    if (icon) rect(fb_w, fb_h, at.x, std::round(at.y + (h - ih) * 0.5f), iw, ih, c, g_icon_tex[t.icon], 0.0f, true);
    if (text) {
        const float x = at.x + iw + gap, y = std::round(at.y + (h - th) * 0.5f);
        const float shadow[4] = {0.0f, 0.0f, 0.0f, 0.85f};
        const float offs[4][2] = {{-o, 0.0f}, {o, 0.0f}, {0.0f, -o}, {0.0f, o}};
        for (const auto& d : offs) rect(fb_w, fb_h, x + d[0], y + d[1], tw, th, shadow, text->tex);
        rect(fb_w, fb_h, x, y, tw, th, c, text->tex);
    }
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBindVertexArray(0);
}
