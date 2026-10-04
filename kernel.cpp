// kernel.cpp - Tidepool OS: nhân C++ freestanding cho x86_64
// Hướng A: Tối ưu hóa Dirty Rectangles + Window Manager
//
// Đặc điểm:
// - Back buffer 1024x768 + Dirty Rectangle tracking
// - Window Manager cơ bản: drag, min, max, close
// - Framebuffer một lần thay vì redraw toàn hình
// - Không dùng floating point, STL, hoặc dynamic allocation

#include <stdint.h>
#include <stddef.h>
#include "font.h"

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

// ---------------------------------------------------------------------------
// Phần cứng tối thiểu: memset/memcpy, I/O ports, CPU halt
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

[[noreturn]] static void fail(const char* msg) {
    volatile u16* vga = reinterpret_cast<volatile u16*>(0xB8000);
    for (int i = 0; i < 80 * 25; i++) vga[i] = 0x1F00 | ' ';
    for (int i = 0; msg[i] && i < 80; i++) vga[i] = 0x1F00 | static_cast<u8>(msg[i]);
    halt_forever();
}

// ---------------------------------------------------------------------------
// Dirty Rectangle Tracking
// ---------------------------------------------------------------------------
enum { W = 1024, H = 768, SCR_MAX = 800 * 600 };

struct Rect { int x, y, w, h; };

static u32 back[W * H];        // back buffer 0x00RRGGBB
static u32 scratch[SCR_MAX];   // temp buffer cho effects
static u32 line_buf[W];        // temp buffer cho blur

static Rect dirty = {0, 0, W, H};
static bool dirty_valid = false;

static void mark_dirty(int x, int y, int w, int h) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    if (w <= 0 || h <= 0) return;

    if (!dirty_valid) {
        dirty = {x, y, w, h};
        dirty_valid = true;
        return;
    }

    int x1 = dirty.x + dirty.w, y1 = dirty.y + dirty.h;
    int nx1 = x + w, ny1 = y + h;
    
    if (x < dirty.x) dirty.x = x;
    if (y < dirty.y) dirty.y = y;
    if (nx1 > x1) x1 = nx1;
    if (ny1 > y1) y1 = ny1;
    
    dirty.w = x1 - dirty.x;
    dirty.h = y1 - dirty.y;
}

static void invalidate_all() {
    dirty = {0, 0, W, H};
    dirty_valid = true;
}

// ---------------------------------------------------------------------------
// Màu sắc và phép toán pixel
// ---------------------------------------------------------------------------
static constexpr u32 rgb(u32 r, u32 g, u32 b) { return (r << 16) | (g << 8) | b; }
static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline int absi(int v) { return v < 0 ? -v : v; }
static inline int maxi(int a, int b) { return a > b ? a : b; }
static inline int mini(int a, int b) { return a < b ? a : b; }

static const u32 IVORY  = rgb(239, 233, 221);
static const u32 INK    = rgb(43, 41, 36);
static const u32 MUTE   = rgb(106, 101, 91);
static const u32 ACCENT = rgb(92, 127, 110);
static const u32 PAPER  = rgb(244, 240, 232);
static const u32 ROWBG  = rgb(252, 250, 245);

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

static inline u32 mix(u32 d, u32 s, u32 a) {
    u32 rb = (((d & 0xFF00FF) * (256 - a) + (s & 0xFF00FF) * a) >> 8) & 0xFF00FF;
    u32 g  = (((d & 0x00FF00) * (256 - a) + (s & 0x00FF00) * a) >> 8) & 0x00FF00;
    return rb | g;
}

static inline void plot(int x, int y, u32 c, u32 a) {
    if (static_cast<unsigned>(x) >= W || static_cast<unsigned>(y) >= H || a == 0) return;
    u32* p = &back[y * W + x];
    *p = a >= 256 ? c : mix(*p, c, a);
    mark_dirty(x, y, 1, 1);
}

// ---------------------------------------------------------------------------
// Hình khối: hình chữ nhật bo góc (SDF)
// ---------------------------------------------------------------------------
struct R4 { int tl, tr, br, bl; };
static inline R4 rad(int r) { return R4{r, r, r, r}; }

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

static inline u32 cov(int d) { return static_cast<u32>(clampi((8 - d) * 16, 0, 256)); }

static void fill_rr(int x, int y, int w, int h, R4 r, u32 c0, u32 c1, u32 a) {
    mark_dirty(x, y, w, h);
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

static inline void rr(int x, int y, int w, int h, R4 r, u32 c, u32 a = 256) { 
    fill_rr(x, y, w, h, r, c, c, a); 
}

static inline void rect(int x, int y, int w, int h, u32 c, u32 a = 256) { 
    rr(x, y, w, h, rad(0), c, a); 
}

static void stroke_rr(int x, int y, int w, int h, R4 r, int t16, u32 c, u32 a) {
    mark_dirty(x - 4, y - 4, w + 8, h + 8);
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

template <class F>
static void shape(int x0, int y0, int x1, int y1, u32 c, u32 a, F f) {
    x0 = maxi(x0, 0); y0 = maxi(y0, 0);
    if (x1 > W) x1 = W;
    if (y1 > H) y1 = H;
    if (x1 <= x0 || y1 <= y0) return;
    mark_dirty(x0, y0, x1 - x0, y1 - y0);
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            u32 k = f(x, y);
            if (k) plot(x, y, c, k * a >> 8);
        }
}

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
// Window Manager cơ bản
// ---------------------------------------------------------------------------
struct Win {
    int x, y, w, h;
    const char* title;
    bool active;
    bool visible;
    
    // Hỗ trợ drag, min, max, close
    int state; // 0=normal, 1=minimized, 2=maximized
};

enum { MAX_WINDOWS = 4 };
static Win windows[MAX_WINDOWS];
static int win_count = 0;

static void win_add(int x, int y, int w, int h, const char* title) {
    if (win_count >= MAX_WINDOWS) return;
    windows[win_count] = {x, y, w, h, title, true, true, 0};
    win_count++;
}

static void win_draw_frame(const Win& w) {
    if (!w.visible) return;
    
    mark_dirty(w.x, w.y, w.w, w.h);
    
    // Nền kính mờ nhẹ
    rr(w.x, w.y, w.w, w.h, rad(16), PAPER, 200);
    
    // Viền sáng phía trên
    stroke_rr(w.x, w.y, w.w, w.h, rad(16), 16, rgb(255, 250, 240), 130);
    
    // Thanh tiêu đề
    u32 fg = w.active ? INK : MUTE;
    rect(w.x, w.y, w.w, 38, w.active ? rgb(211, 223, 208) : rgb(240, 235, 225));
    
    // Text tiêu đề (thay bằng text trực tiếp)
    // Tạm thời skip vì cần integrate font rendering
}

static void win_toggle_maximize(Win& w) {
    if (w.state == 2) {
        w.state = 0;
        // Restore previous size (simplified: back to 70% screen)
        w.w = W * 7 / 10;
        w.h = H * 7 / 10;
        w.x = (W - w.w) / 2;
        w.y = (H - w.h) / 2;
    } else {
        w.state = 2;
        w.x = 0;
        w.y = 0;
        w.w = W;
        w.h = H;
    }
    invalidate_all();
}

static void win_toggle_minimize(Win& w) {
    w.state = (w.state == 1) ? 0 : 1;
    invalidate_all();
}

// ---------------------------------------------------------------------------
// Biểu tượng (vẽ bằng hình khối)
// ---------------------------------------------------------------------------
typedef void (*IconFn)(int cx, int cy, int s, u32 fg, u32 bg);

static void ico_folder(int cx, int cy, int s, u32 fg, u32) {
    int x = cx - s / 2, y = cy - s * 5 / 16;
    rr(x, y, s * 7 / 16, s * 4 / 16, rad(s / 12 + 1), fg, 150);
    rr(x, y + s * 2 / 16, s, s * 10 / 16, rad(s / 7), fg, 150);
    rr(x, y + s * 5 / 16, s, s * 7 / 16, R4{s / 9, s / 9, s / 7, s / 7}, fg, 235);
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

static void ico_note(int cx, int cy, int s, u32 fg, u32 bg) {
    int w = s * 10 / 16, h = s * 13 / 16;
    rr(cx - w / 2, cy - h / 2, w, h, rad(s / 9 + 1), fg, 240);
    for (int i = 0; i < 3; i++)
        rr(cx - w / 2 + s / 6, cy - h / 2 + s * 4 / 16 + i * s * 3 / 16, w - s / 3, s / 16 + 1, rad(1), bg, 200);
}

// ---------------------------------------------------------------------------
// Hàm vẽ desktop
// ---------------------------------------------------------------------------
static void draw_desktop() {
    // Nền đơn giản: màu gradient
    mark_dirty(0, 0, W, H);
    const u32 bg_top = rgb(40, 45, 50);
    const u32 bg_bot = rgb(60, 55, 50);
    
    for (int y = 0; y < H; y++) {
        u32 c = mix(bg_top, bg_bot, static_cast<u32>(y * 256 / H));
        for (int x = 0; x < W; x++) {
            back[y * W + x] = c;
        }
    }
}

static void draw_windows() {
    for (int i = 0; i < win_count; i++) {
        if (windows[i].visible && windows[i].state != 1) {
            win_draw_frame(windows[i]);
        }
    }
}

// ---------------------------------------------------------------------------
// Multiboot2 + Framebuffer
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
            f.rp = tag[32]; f.gp = tag[34]; f.bp = tag[36];
            return true;
        }
        off += (size + 7) & ~7u;
    }
    return false;
}

static void present_dirty(const Fb& f) {
    if (!dirty_valid) return;

    int x0 = clampi(dirty.x, 0, W);
    int y0 = clampi(dirty.y, 0, H);
    int x1 = clampi(dirty.x + dirty.w, 0, W);
    int y1 = clampi(dirty.y + dirty.h, 0, H);

    if (x1 <= x0 || y1 <= y0) {
        dirty_valid = false;
        return;
    }

    int ox = (static_cast<int>(f.w) - W) / 2;
    int oy = (static_cast<int>(f.h) - H) / 2;
    bool direct = f.rp == 16 && f.gp == 8 && f.bp == 0;
    u8* base = reinterpret_cast<u8*>(f.addr);

    for (int y = y0; y < y1; y++) {
        u32* src = &back[y * W + x0];
        u32* dst = reinterpret_cast<u32*>(
            base + static_cast<u64>(oy + y) * f.pitch + 
            static_cast<u64>(ox + x0) * 4);

        if (direct) {
            memcpy(dst, src, static_cast<size_t>((x1 - x0) * 4));
        } else {
            for (int x = x0; x < x1; x++) {
                u32 c = src[x - x0];
                dst[x - x0] = (((c >> 16) & 255) << f.rp) | 
                              (((c >> 8) & 255) << f.gp) | 
                              ((c & 255) << f.bp);
            }
        }
    }

    dirty_valid = false;
}

extern "C" void kernel_main(u64 mbi) {
    Fb fb;
    if (!find_fb(mbi, fb) || fb.type != 1 || fb.bpp != 32)
        fail("Tidepool OS: khong co framebuffer 32-bit. Hay khoi dong bang GRUB (Multiboot2).");
    if (fb.w < W || fb.h < H)
        fail("Tidepool OS: can man hinh toi thieu 1024x768.");
    if (fb.addr + static_cast<u64>(fb.pitch) * fb.h > (1ull << 32))
        fail("Tidepool OS: framebuffer nam ngoai vung 4 GiB da anh xa.");

    // Khởi tạo cửa sổ
    win_add(100, 100, 400, 300, "Welcome");
    win_add(550, 150, 420, 380, "Settings");
    windows[0].active = false;
    windows[1].active = true;

    // Vẽ desktop lần đầu
    draw_desktop();
    draw_windows();
    
    // Hiển thị vùng bẩn (Dirty Rectangles)
    present_dirty(fb);
    
    halt_forever();
}
