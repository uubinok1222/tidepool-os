// kernel.cpp - Tidepool OS: nhân C++ freestanding cho x86_64.
//
// Không có std, libc, heap hay hệ điều hành bên dưới. Chỉ dùng <stdint.h>/<stddef.h>
// (do chính trình biên dịch cung cấp). Toàn bộ giao diện được vẽ bằng số nguyên
// (không dùng số thực/SSE) vào một back buffer, rồi chép một lần ra framebuffer của GRUB.

#include <stdint.h>
#include <stddef.h>
#include "font.h"

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

// ---------------------------------------------------------------------------
// Phần cứng tối thiểu: memset/memcpy (GCC có thể tự gọi), cổng I/O, dừng CPU
// ---------------------------------------------------------------------------
extern "C" {
void* memset(void* d, int c, size_t n) {
    void* r = d;
    asm volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory");
    return r;
}
void* memcpy(void* d, const void* s, size_t n) {
    void* r = d;
    asm volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
    return r;
}
}

static inline void outb(u16 port, u8 v) { asm volatile("outb %0, %1" : : "a"(v), "Nd"(port)); }
static inline u8 inb(u16 port) { u8 v; asm volatile("inb %1, %0" : "=a"(v) : "Nd"(port)); return v; }
[[noreturn]] static void halt_forever() { for (;;) asm volatile("cli; hlt"); }

// Dùng khi chưa có framebuffer: in thông báo ra VGA text mode (0xB8000) rồi dừng.
[[noreturn]] static void fail(const char* msg) {
    volatile u16* vga = reinterpret_cast<volatile u16*>(0xB8000);
    for (int i = 0; i < 80 * 25; i++) vga[i] = 0x1F00 | ' ';
    for (int i = 0; msg[i] && i < 80; i++) vga[i] = 0x1F00 | static_cast<u8>(msg[i]);
    halt_forever();
}

// ---------------------------------------------------------------------------
// Số học nguyên, màu và canvas
// ---------------------------------------------------------------------------
enum { W = 1024, H = 768, SCR_MAX = 800 * 600 };

static u32 back[W * H];        // canvas 0x00RRGGBB, vẽ xong mới chép ra màn hình
static u32 scratch[SCR_MAX];   // vùng đệm cho hiệu ứng kính mờ
static u32 line_buf[W];        // một dòng/cột cho bộ làm mờ

static constexpr u32 rgb(u32 r, u32 g, u32 b) { return (r << 16) | (g << 8) | b; }
static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline int absi(int v) { return v < 0 ? -v : v; }
static inline int maxi(int a, int b) { return a > b ? a : b; }

static const u32 IVORY  = rgb(239, 233, 221);
static const u32 INK    = rgb(43, 41, 36);
static const u32 MUTE   = rgb(106, 101, 91);
static const u32 ACCENT = rgb(92, 127, 110);
static const u32 PAPER  = rgb(244, 240, 232);   // nền cửa sổ kính
static const u32 ROWBG  = rgb(252, 250, 245);   // nền các thẻ trong Cài đặt

static u32 isqrt(u32 n) {
    u32 r = 0, bit = 1u << 30;
    while (bit > n) bit >>= 2;
    while (bit) {
        if (n >= r + bit) { n -= r + bit; r = (r >> 1) + bit; }
        else r >>= 1;
        bit >>= 2;
    }
    return r;
}

// Trộn hai màu, a = 0..256 là độ đục của src.
static inline u32 mix(u32 d, u32 s, u32 a) {
    u32 rb = (((d & 0xFF00FF) * (256 - a) + (s & 0xFF00FF) * a) >> 8) & 0xFF00FF;
    u32 g  = (((d & 0x00FF00) * (256 - a) + (s & 0x00FF00) * a) >> 8) & 0x00FF00;
    return rb | g;
}

static inline void plot(int x, int y, u32 c, u32 a) {
    if (static_cast<unsigned>(x) >= W || static_cast<unsigned>(y) >= H || a == 0) return;
    u32* p = &back[y * W + x];
    *p = a >= 256 ? c : mix(*p, c, a);
}

// ---------------------------------------------------------------------------
// Hình khối: hình chữ nhật bo góc bằng khoảng cách có dấu (SDF), đơn vị 1/16 px
// ---------------------------------------------------------------------------
struct R4 { int tl, tr, br, bl; };
static inline R4 rad(int r) { return R4{r, r, r, r}; }

// Khoảng cách từ tâm điểm ảnh (px,py) tới biên hình; âm = bên trong.
static int sdf_rr(int x, int y, int w, int h, R4 r, int px, int py) {
    int X = px * 16 + 8 - (x * 16 + w * 8);
    int Y = py * 16 + 8 - (y * 16 + h * 8);
    int rd = (X < 0 ? (Y < 0 ? r.tl : r.bl) : (Y < 0 ? r.tr : r.br)) * 16;
    int qx = absi(X) - (w * 8 - rd), qy = absi(Y) - (h * 8 - rd);
    int ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
    int len = (ox || oy) ? static_cast<int>(isqrt(static_cast<u32>(ox * ox + oy * oy))) : 0;
    int inner = qx > qy ? qx : qy;
    if (inner > 0) inner = 0;
    return len + inner - rd;
}

// Độ phủ (0..256) của điểm ảnh khi cách biên d (1/16 px): khử răng cưa ở mép.
static inline u32 cov(int d) { return static_cast<u32>(clampi((8 - d) * 16, 0, 256)); }

// Tô hình chữ nhật bo góc, màu chuyển dọc từ c0 (trên) xuống c1 (dưới).
static void fill_rr(int x, int y, int w, int h, R4 r, u32 c0, u32 c1, u32 a) {
    int rm = maxi(maxi(r.tl, r.tr), maxi(r.br, r.bl));
    for (int j = 0; j < h; j++) {
        u32 c = (h > 1 && c0 != c1) ? mix(c0, c1, static_cast<u32>(j * 256 / (h - 1))) : c0;
        for (int i = 0; i < w; i++) {
            u32 k = (i >= rm && i < w - rm && j >= rm && j < h - rm)
                        ? 256 : cov(sdf_rr(x, y, w, h, r, x + i, y + j));
            if (k) plot(x + i, y + j, c, k * a >> 8);
        }
    }
}
static inline void rr(int x, int y, int w, int h, R4 r, u32 c, u32 a = 256) { fill_rr(x, y, w, h, r, c, c, a); }
static inline void rect(int x, int y, int w, int h, u32 c, u32 a = 256) { rr(x, y, w, h, rad(0), c, a); }

// Viền dày t16 (1/16 px) bám theo mép trong của hình.
static void stroke_rr(int x, int y, int w, int h, R4 r, int t16, u32 c, u32 a) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            if (i > 3 && i < w - 4 && j > 3 && j < h - 4 && t16 <= 32) {
                int rm = maxi(maxi(r.tl, r.tr), maxi(r.br, r.bl));
                if (i >= rm && i < w - rm && j >= rm && j < h - rm) continue;
            }
            int d = sdf_rr(x, y, w, h, r, x + i, y + j);
            u32 k = cov(d), in = static_cast<u32>(clampi((d + t16 + 8) * 16, 0, 256));
            if (in < k) k = in;
            if (k) plot(x + i, y + j, c, k * a >> 8);
        }
}

// Vẽ một hình bất kỳ trong hộp [x0,x1)x[y0,y1) theo hàm độ phủ f(x,y) -> 0..256.
template <class F>
static void shape(int x0, int y0, int x1, int y1, u32 c, u32 a, F f) {
    x0 = maxi(x0, 0); y0 = maxi(y0, 0);
    if (x1 > W) x1 = W;
    if (y1 > H) y1 = H;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            u32 k = f(x, y);
            if (k) plot(x, y, c, k * a >> 8);
        }
}

// Toạ độ/bán kính của các hàm dưới đây tính theo 1/16 px.
static void disc(int cx, int cy, int r, u32 c, u32 a = 256) {
    int p = r / 16 + 2;
    shape(cx / 16 - p, cy / 16 - p, cx / 16 + p + 1, cy / 16 + p + 1, c, a, [&](int x, int y) {
        int dx = x * 16 + 8 - cx, dy = y * 16 + 8 - cy;
        return cov(static_cast<int>(isqrt(static_cast<u32>(dx * dx + dy * dy))) - r);
    });
}

static void ring(int cx, int cy, int r, int t, u32 c, u32 a = 256) {
    int p = (r + t) / 16 + 2;
    shape(cx / 16 - p, cy / 16 - p, cx / 16 + p + 1, cy / 16 + p + 1, c, a, [&](int x, int y) {
        int dx = x * 16 + 8 - cx, dy = y * 16 + 8 - cy;
        return cov(absi(static_cast<int>(isqrt(static_cast<u32>(dx * dx + dy * dy))) - r) - t / 2);
    });
}

// Cung tròn 90 độ: dir 0 = hướng lên, 1 = hướng sang phải, 2 = nửa trên.
static void arc(int cx, int cy, int r, int t, int dir, u32 c, u32 a = 256) {
    int p = (r + t) / 16 + 2;
    shape(cx / 16 - p, cy / 16 - p, cx / 16 + p + 1, cy / 16 + p + 1, c, a, [&](int x, int y) {
        int dx = x * 16 + 8 - cx, dy = y * 16 + 8 - cy;
        bool ok = dir == 0 ? (dy < 0 && absi(dx) <= -dy)
                : dir == 1 ? (dx > 0 && absi(dy) <= dx)
                           : (dy <= 0);
        if (!ok) return static_cast<u32>(0);
        return cov(absi(static_cast<int>(isqrt(static_cast<u32>(dx * dx + dy * dy))) - r) - t / 2);
    });
}

// Đoạn thẳng dày t (đầu bo tròn).
static void seg(int x0, int y0, int x1, int y1, int t, u32 c, u32 a = 256) {
    int bx = x1 - x0, by = y1 - y0, den = bx * bx + by * by;
    int minx = (x0 < x1 ? x0 : x1) / 16 - t / 16 - 2, maxx = (x0 > x1 ? x0 : x1) / 16 + t / 16 + 3;
    int miny = (y0 < y1 ? y0 : y1) / 16 - t / 16 - 2, maxy = (y0 > y1 ? y0 : y1) / 16 + t / 16 + 3;
    shape(minx, miny, maxx, maxy, c, a, [&](int x, int y) {
        int ax = x * 16 + 8 - x0, ay = y * 16 + 8 - y0;
        int h = den ? clampi((ax * bx + ay * by) * 256 / den, 0, 256) : 0;
        int dx = ax - bx * h / 256, dy = ay - by * h / 256;
        return cov(static_cast<int>(isqrt(static_cast<u32>(dx * dx + dy * dy))) - t / 2);
    });
}

// ---------------------------------------------------------------------------
// Hiệu ứng: bóng đổ mềm và kính mờ (backdrop blur)
// ---------------------------------------------------------------------------
// Bóng đổ nằm ngoài hình (không đè lên bên trong), mờ dần trong `blur` px, lệch xuống dy px.
static void shadow(int x, int y, int w, int h, int r, int blur, int dy, u32 maxa) {
    const u32 sc = rgb(16, 12, 8);
    const int B = blur * 16;
    int x0 = clampi(x - blur, 0, W), x1 = clampi(x + w + blur, 0, W);
    int y0 = clampi(y - blur, 0, H), y1 = clampi(y + h + blur + dy, 0, H);
    R4 rd = rad(r);
    for (int py = y0; py < y1; py++)
        for (int px = x0; px < x1; px++) {
            if (px >= x + r && px < x + w - r && py >= y + r && py < y + h - r) continue;
            int d1 = sdf_rr(x, y + dy, w, h, rd, px, py);
            if (d1 >= B) continue;
            int t = d1 <= 0 ? 256 : 256 - d1 * 256 / B;
            u32 a = maxa * static_cast<u32>(t * t >> 8) >> 8;
            if (!a) continue;
            a = a * (256 - cov(sdf_rr(x, y, w, h, rd, px, py))) >> 8;
            plot(px, py, sc, a);
        }
}

// Làm mờ một dòng (hoặc cột, với stride) bằng cửa sổ trượt 2r+1 điểm.
static void blur_axis(u32* base, int len, int stride, int r) {
    for (int i = 0; i < len; i++) line_buf[i] = base[i * stride];
    int n = 2 * r + 1, sr = 0, sg = 0, sb = 0;
    int inv = 65536 / n;
    for (int k = -r; k <= r; k++) {
        u32 c = line_buf[clampi(k, 0, len - 1)];
        sr += (c >> 16) & 255; sg += (c >> 8) & 255; sb += c & 255;
    }
    for (int i = 0; i < len; i++) {
        base[i * stride] = (static_cast<u32>(sr * inv >> 16) << 16) |
                           (static_cast<u32>(sg * inv >> 16) << 8) | static_cast<u32>(sb * inv >> 16);
        u32 a = line_buf[clampi(i + r + 1, 0, len - 1)], b = line_buf[clampi(i - r, 0, len - 1)];
        sr += static_cast<int>((a >> 16) & 255) - static_cast<int>((b >> 16) & 255);
        sg += static_cast<int>((a >> 8) & 255) - static_cast<int>((b >> 8) & 255);
        sb += static_cast<int>(a & 255) - static_cast<int>(b & 255);
    }
}

// Kính mờ: lấy nền phía sau, làm mờ 2 lượt, phủ màu tint (độ đục ta), cắt theo góc bo.
static void glass(int x, int y, int w, int h, R4 r, u32 tint, u32 ta, int blur_r) {
    if (x < 0 || y < 0 || x + w > W || y + h > H || w * h > SCR_MAX) return;
    for (int j = 0; j < h; j++) memcpy(&scratch[j * w], &back[(y + j) * W + x], static_cast<size_t>(w) * 4);
    for (int pass = 0; pass < 2; pass++) {
        for (int j = 0; j < h; j++) blur_axis(scratch + j * w, w, 1, blur_r);
        for (int i = 0; i < w; i++) blur_axis(scratch + i, h, w, blur_r);
    }
    int rm = maxi(maxi(r.tl, r.tr), maxi(r.br, r.bl));
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            u32 k = (i >= rm && i < w - rm && j >= rm && j < h - rm)
                        ? 256 : cov(sdf_rr(x, y, w, h, r, x + i, y + j));
            if (!k) continue;
            u32* p = &back[(y + j) * W + x + i];
            *p = mix(*p, mix(scratch[j * w + i], tint, ta), k);
        }
}

// ---------------------------------------------------------------------------
// Chữ: UTF-8 -> glyph bitmap alpha (font.h), có kerning cố định theo advance
// ---------------------------------------------------------------------------
static const Glyph* find_glyph(const Face& f, u32 cp) {
    int lo = 0, hi = f.n - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (f.g[m].cp == cp) return &f.g[m];
        if (f.g[m].cp < cp) lo = m + 1; else hi = m - 1;
    }
    return nullptr;
}

static u32 next_cp(const char*& s) {
    u8 c = static_cast<u8>(*s++);
    if (c < 0x80) return c;
    if ((c & 0xE0) == 0xC0) { u32 cp = (c & 0x1Fu) << 6; cp |= static_cast<u8>(*s++) & 0x3Fu; return cp; }
    if ((c & 0xF0) == 0xE0) {
        u32 cp = (c & 0x0Fu) << 12;
        cp |= (static_cast<u8>(*s++) & 0x3Fu) << 6;
        cp |= static_cast<u8>(*s++) & 0x3Fu;
        return cp;
    }
    return '?';
}

static int text_w(const Face& f, const char* s) {
    int w = 0;
    while (*s) {
        const Glyph* g = find_glyph(f, next_cp(s));
        if (g) w += g->adv;
    }
    return w;
}

// Vẽ chữ với đường cơ sở tại `base`; trả về hoành độ sau ký tự cuối.
static int text(const Face& f, int x, int base, const char* s, u32 c, u32 a = 256) {
    while (*s) {
        const Glyph* g = find_glyph(f, next_cp(s));
        if (!g) continue;
        const u8* px = f.px + g->off;
        for (int j = 0; j < g->h; j++)
            for (int i = 0; i < g->w; i++) {
                u32 v = px[j * g->w + i];
                if (v) plot(x + g->bx + i, base + g->by + j, c, v * a >> 8);
            }
        x += g->adv;
    }
    return x;
}
static void text_center(const Face& f, int cx, int base, const char* s, u32 c, u32 a = 256) {
    text(f, cx - text_w(f, s) / 2, base, s, c, a);
}

// ---------------------------------------------------------------------------
// Biểu tượng (vẽ bằng hình khối, cx/cy là tâm, s là cỡ theo px)
// ---------------------------------------------------------------------------
typedef void (*IconFn)(int cx, int cy, int s, u32 fg, u32 bg);

static void ico_folder(int cx, int cy, int s, u32 fg, u32) {
    int x = cx - s / 2, y = cy - s * 5 / 16;
    rr(x, y, s * 7 / 16, s * 4 / 16, rad(s / 12 + 1), fg, 150);
    rr(x, y + s * 2 / 16, s, s * 10 / 16, rad(s / 7), fg, 150);
    rr(x, y + s * 5 / 16, s, s * 7 / 16, R4{s / 9, s / 9, s / 7, s / 7}, fg, 235);
}
static void ico_note(int cx, int cy, int s, u32 fg, u32 bg) {
    int w = s * 10 / 16, h = s * 13 / 16;
    rr(cx - w / 2, cy - h / 2, w, h, rad(s / 9 + 1), fg, 240);
    for (int i = 0; i < 3; i++)
        rr(cx - w / 2 + s / 6, cy - h / 2 + s * 4 / 16 + i * s * 3 / 16, w - s / 3, s / 16 + 1, rad(1), bg, 200);
}
static void ico_gear(int cx, int cy, int s, u32 fg, u32 bg) {
    static const int ux[8] = {1024, 724, 0, -724, -1024, -724, 0, 724};
    static const int uy[8] = {0, 724, 1024, 724, 0, -724, -1024, -724};
    int c16x = cx * 16, c16y = cy * 16;
    for (int i = 0; i < 8; i++)
        disc(c16x + ux[i] * s * 7 / 16 / 64, c16y + uy[i] * s * 7 / 16 / 64, s * 16 * 2 / 16, fg, 255);
    disc(c16x, c16y, s * 16 * 6 / 16, fg, 255);
    disc(c16x, c16y, s * 16 * 3 / 16, bg, 255);
}
static void ico_trash(int cx, int cy, int s, u32 fg, u32) {
    rr(cx - s * 5 / 16, cy - s / 8, s * 10 / 16, s * 9 / 16, R4{s / 14, s / 14, s / 8, s / 8}, fg, 235);
    rr(cx - s * 6 / 16, cy - s * 5 / 16, s * 12 / 16, s / 8 + 1, rad(s / 14 + 1), fg, 255);
    rr(cx - s * 2 / 16, cy - s * 7 / 16, s * 4 / 16, s / 8 + 1, rad(s / 14 + 1), fg, 255);
}
static void ico_wifi(int cx, int cy, int s, u32 c, u32) {
    int ax = cx * 16, ay = (cy + s * 5 / 16) * 16, t = s * 26 / 16;
    for (int k = 1; k <= 3; k++) arc(ax, ay, s * 3 * k, t, 0, c);
    disc(ax, ay, s * 24 / 16, c);
}
static void ico_sound(int cx, int cy, int s, u32 c, u32) {
    rect(cx - s * 6 / 16, cy - s * 2 / 16, s * 3 / 16 + 1, s * 4 / 16, c);
    for (int i = 0; i < s * 4 / 16; i++)
        rect(cx - s * 3 / 16 + i, cy - s * 2 / 16 - i, 1, s * 4 / 16 + 2 * i, c);
    arc(cx * 16 + s, cy * 16, s * 4, s * 26 / 16, 1, c);
    arc(cx * 16 + s, cy * 16, s * 7, s * 26 / 16, 1, c);
}
static void ico_display(int cx, int cy, int s, u32 c, u32) {
    int w = s * 14 / 16, h = s * 9 / 16;
    stroke_rr(cx - w / 2, cy - h / 2 - s / 12, w, h, rad(s / 10 + 1), s * 28 / 16, c, 256);
    rect(cx - s * 3 / 16, cy + h / 2 - s / 12 + s / 16, s * 6 / 16, s / 16 + 1, c);
}
static void ico_info(int cx, int cy, int s, u32 c, u32) {
    ring(cx * 16, cy * 16, s * 7, s * 26 / 16, c);
    disc(cx * 16, (cy - s * 3 / 16) * 16, s * 14 / 16, c);
    seg(cx * 16, (cy - s / 16) * 16, cx * 16, (cy + s * 3 / 16) * 16, s * 26 / 16, c);
}
static void ico_lock(int cx, int cy, int s, u32 c, u32) {
    rr(cx - s * 5 / 16, cy - s / 16, s * 10 / 16, s * 7 / 16, rad(s / 10 + 1), c, 255);
    arc(cx * 16, (cy - s / 16) * 16, s * 3, s * 28 / 16, 2, c);
    seg((cx - s * 3 / 16) * 16, (cy - s / 16) * 16, (cx - s * 3 / 16) * 16, (cy - s * 3 / 16) * 16, s * 28 / 16, c);
    seg((cx + s * 3 / 16) * 16, (cy - s / 16) * 16, (cx + s * 3 / 16) * 16, (cy - s * 3 / 16) * 16, s * 28 / 16, c);
}

// Ô ứng dụng kiểu squircle: bóng mềm, viền sáng phía trên, màu chuyển dọc.
static void tile(int x, int y, int s, u32 c0, u32 c1, IconFn icon) {
    int r = s * 15 / 54;
    shadow(x, y, s, s, r, 12, 5, 110);
    rr(x, y, s, s, rad(r), rgb(255, 255, 255), 90);
    fill_rr(x, y + 1, s, s - 1, rad(r), c0, c1, 256);
    icon(x + s / 2, y + s / 2, s / 2 + 2, IVORY, mix(c0, c1, 128));
}

// ---------------------------------------------------------------------------
// Màn hình nền: các mảng màu dịu + hạt noise (giống bản HTML)
// ---------------------------------------------------------------------------
struct Blob { int cx, cy, r; u32 c, s; };
static const Blob blobs[4] = {
    {150, 90, 540, rgb(111, 132, 102), 205}, {900, 230, 500, rgb(216, 185, 142), 205},
    {430, 840, 600, rgb(127, 156, 150), 205}, {860, 760, 400, rgb(199, 138, 116), 140},
};

static void wallpaper() {
    const u32 bg = rgb(36, 39, 35);
    u32 rng = 0x1234567;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            u32 col = bg;
            for (const Blob& b : blobs) {
                int dx = x - b.cx, dy = y - b.cy;
                u32 d2 = static_cast<u32>(dx * dx + dy * dy), r2 = static_cast<u32>(b.r * b.r);
                if (d2 >= r2) continue;
                u32 t = (r2 - d2) * ((1u << 24) / r2) >> 16;      // 0..256
                col = mix(col, b.c, (t * t >> 8) * b.s >> 8);
            }
            rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
            int n = static_cast<int>(rng & 7) - 3;
            int r = clampi(static_cast<int>((col >> 16) & 255) + n, 0, 255);
            int g = clampi(static_cast<int>((col >> 8) & 255) + n, 0, 255);
            int bl = clampi(static_cast<int>(col & 255) + n, 0, 255);
            back[y * W + x] = rgb(static_cast<u32>(r), static_cast<u32>(g), static_cast<u32>(bl));
        }
}

// ---------------------------------------------------------------------------
// Cửa sổ (kiểu Windows: Thu nhỏ / Phóng to / Đóng ở bên phải)
// ---------------------------------------------------------------------------
struct Win { int x, y, w, h; const char* title; IconFn icon; bool active; };

static void window_frame(const Win& w) {
    if (w.active) shadow(w.x, w.y, w.w, w.h, 16, 46, 22, 120);
    else          shadow(w.x, w.y, w.w, w.h, 16, 24, 10, 90);
    glass(w.x, w.y, w.w, w.h, rad(16), PAPER, w.active ? 190 : 176, 10);
    stroke_rr(w.x, w.y, w.w, w.h, rad(16), 16, rgb(255, 250, 240), 130);

    u32 fg = w.active ? INK : MUTE;
    w.icon(w.x + 22, w.y + 19, 16, fg, PAPER);
    text(FONT_BOLD, w.x + 38, w.y + 24, w.title, fg);

    int by = w.y + 19, bx = w.x + w.w;
    seg((bx - 128) * 16, by * 16, (bx - 116) * 16, by * 16, 26, fg);                       // thu nhỏ
    stroke_rr(bx - 82, by - 6, 12, 12, rad(2), 26, fg, 256);                                // phóng to
    seg((bx - 29) * 16, (by - 6) * 16, (bx - 17) * 16, (by + 6) * 16, 26, fg);              // đóng
    seg((bx - 17) * 16, (by - 6) * 16, (bx - 29) * 16, (by + 6) * 16, 26, fg);
}

static void toggle(int x, int y, bool on) {
    if (on) {
        rr(x, y, 50, 30, rad(15), ACCENT);
        disc((x + 35) * 16, (y + 15) * 16, 9 * 16, IVORY);
    } else {
        stroke_rr(x, y, 50, 30, rad(15), 32, rgb(138, 133, 118), 256);
        disc((x + 15) * 16, (y + 15) * 16, 8 * 16, rgb(138, 133, 118));
    }
}

// Một hàng trong nhóm thẻ: biểu tượng (tuỳ chọn), tiêu đề, dòng phụ, công tắc (tuỳ chọn).
static void settings_row(int x, int y, int w, int h, R4 r, IconFn icon, const char* title,
                         const char* sub, int sw /* -1: không có, 0: tắt, 1: bật */) {
    rr(x, y, w, h, r, ROWBG, 205);
    int tx = x + 18;
    if (icon) { icon(x + 29, y + h / 2, 22, rgb(85, 96, 79), ROWBG); tx = x + 54; }
    text(FONT_BOLD, tx, y + (sub ? 23 : h / 2 + 5), title, INK);
    if (sub) text(FONT_SM, tx, y + 42, sub, MUTE);
    if (sw >= 0) toggle(x + w - 18 - 50, y + h / 2 - 15, sw == 1);
}

static void settings_window(const Win& w) {
    window_frame(w);
    const int navw = 190;
    // Cột trái: danh mục
    rr(w.x, w.y + 38, navw, w.h - 38, R4{0, 0, 0, 16}, rgb(255, 250, 240), 70);
    rect(w.x + navw, w.y + 38, 1, w.h - 38, INK, 18);
    struct Item { const char* label; IconFn icon; };
    static const Item items[4] = {
        {"Mạng", ico_wifi}, {"Màn hình", ico_display}, {"Âm thanh", ico_sound}, {"Hệ thống", ico_info}};
    for (int i = 0; i < 4; i++) {
        int y = w.y + 46 + i * 44;
        bool sel = i == 0;
        if (sel) rr(w.x + 10, y, navw - 20, 40, rad(20), rgb(211, 223, 208), 256);
        items[i].icon(w.x + 34, y + 20, 20, sel ? rgb(31, 58, 43) : rgb(85, 96, 79), PAPER);
        text(sel ? FONT_BOLD : FONT_REG, w.x + 56, y + 25, items[i].label, sel ? rgb(31, 58, 43) : INK);
    }
    // Cột phải: thẻ bo tròn
    int px = w.x + navw + 22, pw = w.w - navw - 44, y = w.y + 76;
    text(FONT_HEAD, px, y, "Mạng", INK);
    y += 18;
    settings_row(px, y, pw, 56, R4{22, 22, 6, 6}, nullptr, "Wi-Fi", "Đang dùng Tidepool-5G", 1);
    settings_row(px, y + 59, pw, 56, R4{6, 6, 22, 22}, nullptr, "Chế độ máy bay", "Tắt hết sóng cho yên tĩnh", 0);
    y += 115 + 30;
    text(FONT_BOLD, px + 6, y, "Mạng quanh đây", MUTE);
    y += 12;
    settings_row(px, y, pw, 56, R4{22, 22, 6, 6}, ico_wifi, "Tidepool-5G", "Đang dùng", -1);
    settings_row(px, y + 59, pw, 56, rad(6), ico_wifi, "Cafe Mây", "Không cần mật khẩu", -1);
    settings_row(px, y + 118, pw, 56, R4{6, 6, 22, 22}, ico_lock, "Nhà bên", "Cần mật khẩu", -1);
}

static void welcome_window(const Win& w) {
    window_frame(w);
    int x = w.x + 22, y = w.y + 38 + 40;
    text(FONT_HEAD, x, y, "Tidepool OS", INK);
    text(FONT_REG, x, y + 28, "Chào mừng bạn đến với", MUTE);
    text(FONT_REG, x, y + 48, "hệ điều hành của riêng bạn.", MUTE);
    text(FONT_REG, x, y + 88, "• Nhân C++ thuần, không std", INK);
    text(FONT_REG, x, y + 112, "• GRUB, Multiboot2, long mode", INK);
    text(FONT_REG, x, y + 136, "• Vẽ thẳng lên framebuffer", INK);
}

// ---------------------------------------------------------------------------
// Thanh trên cùng, Dock, icon màn hình nền
// ---------------------------------------------------------------------------
static void topbar(int hh, int mm) {
    glass(0, 0, W, 32, rad(0), rgb(36, 34, 30), 97, 8);
    int x = text(FONT_BOLD, 14, 21, "Tidepool", IVORY);
    text(FONT_REG, x + 14, 21, "Cài đặt", IVORY, 235);

    char clk[6] = {static_cast<char>('0' + hh / 10), static_cast<char>('0' + hh % 10), ':',
                   static_cast<char>('0' + mm / 10), static_cast<char>('0' + mm % 10), 0};
    int cw = text_w(FONT_REG, clk), cx = W - 14 - cw;
    text(FONT_REG, cx, 21, clk, IVORY);
    ico_sound(cx - 24, 16, 16, IVORY, 0);
    ico_wifi(cx - 54, 16, 16, IVORY, 0);
}

static void desktop_icon(int y, const char* label, u32 c0, u32 c1, IconFn icon, bool glass_tile) {
    const int cell = 84, s = 58, cx = 14 + cell / 2, x = cx - s / 2;
    if (glass_tile) {
        shadow(x, y, s, s, 16, 12, 5, 90);
        rr(x, y, s, s, rad(16), IVORY, 60);
        stroke_rr(x, y, s, s, rad(16), 16, IVORY, 110);
        icon(cx, y + s / 2, s / 2 + 2, IVORY, 0);
    } else {
        tile(x, y, s, c0, c1, icon);
    }
    int base = y + s + 20;
    text_center(FONT_SM, cx, base + 1, label, rgb(20, 16, 10), 150);
    text_center(FONT_SM, cx, base, label, IVORY);
}

static void dock() {
    const int ts = 54, gap = 14, DW = 3 * ts + 2 * gap + 32, DH = 80;
    int dx = (W - DW) / 2, dy = H - 12 - DH;
    shadow(dx, dy, DW, DH, 28, 40, 18, 110);
    glass(dx, dy, DW, DH, rad(28), rgb(244, 238, 226), 52, 10);
    stroke_rr(dx, dy, DW, DH, rad(28), 16, rgb(255, 248, 235), 88);
    struct App { u32 c0, c1; IconFn icon; bool running; };
    static const App apps[3] = {
        {rgb(155, 176, 196), rgb(95, 123, 149), ico_folder, false},
        {rgb(230, 194, 122), rgb(196, 138, 69), ico_note, true},
        {rgb(179, 173, 161), rgb(122, 117, 106), ico_gear, true},
    };
    for (int i = 0; i < 3; i++) {
        int x = dx + 16 + i * (ts + gap), y = dy + 10;
        tile(x, y, ts, apps[i].c0, apps[i].c1, apps[i].icon);
        if (apps[i].running) disc((x + ts / 2) * 16, (y + ts + 7) * 16, 2 * 16, IVORY, 240);
    }
}

// ---------------------------------------------------------------------------
// Đồng hồ thời gian thực (CMOS RTC, cổng 0x70/0x71)
// ---------------------------------------------------------------------------
static u8 cmos(u8 reg) { outb(0x70, reg); return inb(0x71); }

static void read_clock(int& hh, int& mm) {
    while (cmos(0x0A) & 0x80) {}                       // đợi RTC cập nhật xong
    u8 m = cmos(0x02), h = cmos(0x04), b = cmos(0x0B);
    bool pm = (h & 0x80) != 0;
    h &= 0x7F;
    if (!(b & 4)) { m = static_cast<u8>((m & 15) + (m >> 4) * 10); h = static_cast<u8>((h & 15) + (h >> 4) * 10); }
    if (!(b & 2)) { h %= 12; if (pm) h += 12; }        // chế độ 12 giờ -> 24 giờ
    hh = h % 24; mm = m % 60;
}

static void draw_desktop() {
    int hh = 0, mm = 0;
    read_clock(hh, mm);
    wallpaper();
    desktop_icon(48,  "Tệp",       rgb(155, 176, 196), rgb(95, 123, 149), ico_folder, false);
    desktop_icon(146, "Ghi chú",   rgb(230, 194, 122), rgb(196, 138, 69), ico_note, false);
    desktop_icon(244, "Thùng rác", 0, 0, ico_trash, true);
    welcome_window({100, 72, 470, 300, "Ghi chú", ico_note, false});    // cửa sổ phía sau (không hoạt động)
    settings_window({360, 196, 620, 450, "Cài đặt", ico_gear, true});    // cửa sổ đang hoạt động
    topbar(hh, mm);
    dock();
}

// ---------------------------------------------------------------------------
// Multiboot2: tìm framebuffer do GRUB dựng sẵn, rồi chép canvas ra màn hình
// ---------------------------------------------------------------------------
struct Fb { u64 addr; u32 pitch, w, h; u8 bpp, type, rp, gp, bp; };

static bool find_fb(u64 mbi, Fb& f) {
    const u8* p = reinterpret_cast<const u8*>(mbi);
    u32 total = *reinterpret_cast<const u32*>(p);
    for (u32 off = 8; off + 8 <= total;) {
        const u8* tag = p + off;
        u32 type = *reinterpret_cast<const u32*>(tag), size = *reinterpret_cast<const u32*>(tag + 4);
        if (type == 0) break;
        if (type == 8 && size >= 31) {
            f.addr = *reinterpret_cast<const u64*>(tag + 8);
            f.pitch = *reinterpret_cast<const u32*>(tag + 16);
            f.w = *reinterpret_cast<const u32*>(tag + 20);
            f.h = *reinterpret_cast<const u32*>(tag + 24);
            f.bpp = tag[28];
            f.type = tag[29];
            f.rp = tag[32]; f.gp = tag[34]; f.bp = tag[36];   // vị trí bit của R, G, B
            return true;
        }
        off += (size + 7) & ~7u;
    }
    return false;
}

static void present(const Fb& f) {
    int ox = (static_cast<int>(f.w) - W) / 2, oy = (static_cast<int>(f.h) - H) / 2;
    bool direct = f.rp == 16 && f.gp == 8 && f.bp == 0;     // BGRX: chép nguyên dòng
    u8* base = reinterpret_cast<u8*>(f.addr);
    if (ox || oy) for (u32 y = 0; y < f.h; y++) memset(base + static_cast<u64>(y) * f.pitch, 0, f.w * 4);
    for (int y = 0; y < H; y++) {
        u32* dst = reinterpret_cast<u32*>(base + static_cast<u64>(oy + y) * f.pitch) + ox;
        const u32* src = &back[y * W];
        if (direct) { memcpy(dst, src, W * 4); continue; }
        for (int x = 0; x < W; x++) {
            u32 c = src[x];
            dst[x] = (((c >> 16) & 255) << f.rp) | (((c >> 8) & 255) << f.gp) | ((c & 255) << f.bp);
        }
    }
}

extern "C" void kernel_main(u64 mbi) {
    Fb fb;
    if (!find_fb(mbi, fb) || fb.type != 1 || fb.bpp != 32)
        fail("Tidepool OS: khong co framebuffer 32-bit. Hay khoi dong bang GRUB (Multiboot2).");
    if (fb.w < W || fb.h < H)
        fail("Tidepool OS: can man hinh toi thieu 1024x768.");
    if (fb.addr + static_cast<u64>(fb.pitch) * fb.h > (1ull << 32))
        fail("Tidepool OS: framebuffer nam ngoai vung 4 GiB da anh xa.");

    draw_desktop();
    present(fb);
    halt_forever();
}
