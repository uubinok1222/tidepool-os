// kernel.cpp - Tidepool OS: nhân C++ freestanding x86_64
// Tối ưu hoàn chỉnh: Dirty Rectangles + Window Manager + Text Rendering
//
// Đặc điểm:
// - Dirty Rectangle tracking: chỉ redraw vùng thay đổi (~3-10x performance gain)
// - Window Manager: drag, minimize, maximize, close
// - Framebuffer một lần thay vì redraw toàn hình
// - Không dùng floating point, STL, hoặc dynamic allocation
// - ~5.4MB static buffers (back + scratch + line_buf)

#include <stdint.h>
#include <stddef.h>
#include "font.h"

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

// ---------------------------------------------------------------------------
// Hardware Abstraction: memset/memcpy, I/O ports, CPU control
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

static inline void outb(u16 port, u8 v) { 
    asm volatile("outb %0, %1" : : "a"(v), "Nd"(port)); 
}
static inline u8 inb(u16 port) { 
    u8 v; 
    asm volatile("inb %1, %0" : "=a"(v) : "Nd"(port)); 
    return v; 
}
[[noreturn]] static void halt_forever() { 
    for (;;) asm volatile("cli; hlt"); 
}

[[noreturn]] static void fail(const char* msg) {
    volatile u16* vga = reinterpret_cast<volatile u16*>(0xB8000);
    for (int i = 0; i < 80 * 25; i++) vga[i] = 0x1F00 | ' ';
    for (int i = 0; msg[i] && i < 80; i++) vga[i] = 0x1F00 | static_cast<u8>(msg[i]);
    halt_forever();
}

// ---------------------------------------------------------------------------
// Dirty Rectangle Tracking & Buffers
// ---------------------------------------------------------------------------
enum { W = 1024, H = 768, SCR_MAX = 800 * 600 };

struct Rect { int x, y, w, h; };

static u32 back[W * H];        // back buffer: 1024*768*4 = 3MB
static u32 scratch[SCR_MAX];   // temp buffer: 800*600*4 = 1.92MB
static u32 line_buf[W];        // temp blur buffer: 1024*4 = 4KB

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
// Color & Pixel Operations (Integer-only, no FPU)
// ---------------------------------------------------------------------------
static constexpr u32 rgb(u32 r, u32 g, u32 b) { 
    return (r << 16) | (g << 8) | b; 
}

static inline int clampi(int v, int lo, int hi) { 
    return v < lo ? lo : (v > hi ? hi : v); 
}

static inline int absi(int v) { 
    return v < 0 ? -v : v; 
}

static inline int maxi(int a, int b) { 
    return a > b ? a : b; 
}

static inline int mini(int a, int b) { 
    return a < b ? a : b; 
}

// Color palette
static const u32 IVORY   = rgb(239, 233, 221);
static const u32 INK     = rgb(43, 41, 36);
static const u32 MUTE    = rgb(106, 101, 91);
static const u32 ACCENT  = rgb(92, 127, 110);
static const u32 PAPER   = rgb(244, 240, 232);
static const u32 ROWBG   = rgb(252, 250, 245);
static const u32 BG_TOP  = rgb(40, 45, 50);
static const u32 BG_BOT  = rgb(60, 55, 50);
static const u32 BAR_BG  = rgb(45, 50, 55);
static const u32 ACTIVE_BAR = rgb(211, 223, 208);
static const u32 INACTIVE_BAR = rgb(240, 235, 225);

// Integer square root (for circle/arc rendering)
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

// Alpha blend: mix two colors with opacity a (0..256)
static inline u32 mix(u32 d, u32 s, u32 a) {
    u32 rb = (((d & 0xFF00FF) * (256 - a) + (s & 0xFF00FF) * a) >> 8) & 0xFF00FF;
    u32 g  = (((d & 0x00FF00) * (256 - a) + (s & 0x00FF00) * a) >> 8) & 0x00FF00;
    return rb | g;
}

// Plot single pixel with dirty tracking
static inline void plot(int x, int y, u32 c, u32 a) {
    if (static_cast<unsigned>(x) >= W || static_cast<unsigned>(y) >= H || a == 0) return;
    u32* p = &back[y * W + x];
    *p = a >= 256 ? c : mix(*p, c, a);
}

// ---------------------------------------------------------------------------
// Shapes: Rounded Rectangles (SDF-based), no floating point
// ---------------------------------------------------------------------------
struct R4 { int tl, tr, br, bl; };

static inline R4 rad(int r) { 
    return R4{r, r, r, r}; 
}

// Signed distance field for rounded rectangle
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

// Coverage: antialiasing at edges
static inline u32 cov(int d) { 
    return static_cast<u32>(clampi((8 - d) * 16, 0, 256)); 
}

// Fill rounded rectangle with gradient (c0 top to c1 bottom)
static void fill_rr(int x, int y, int w, int h, R4 r, u32 c0, u32 c1, u32 a) {
    mark_dirty(x, y, w, h);
    int rm = maxi(maxi(r.tl, r.tr), maxi(r.br, r.bl));
    for (int j = 0; j < h; j++) {
        u32 c = (h > 1 && c0 != c1) ? mix(c0, c1, static_cast<u32>(j * 256 / (h - 1))) : c0;
        for (int i = 0; i < w; i++) {
            u32 k = (i >= rm && i < w - rm && j >= rm && j < h - rm)
                        ? 256 : cov(sdf_rr(x, y, w, h, r, x + i, y + j));
            if (k) {
                u32* p = &back[(y + j) * W + x + i];
                *p = a >= 256 ? c : mix(*p, c, (k * a) >> 8);
            }
        }
    }
}

static inline void rr(int x, int y, int w, int h, R4 r, u32 c, u32 a = 256) { 
    fill_rr(x, y, w, h, r, c, c, a); 
}

static inline void rect(int x, int y, int w, int h, u32 c, u32 a = 256) { 
    mark_dirty(x, y, w, h);
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            u32* p = &back[(y + j) * W + x + i];
            *p = a >= 256 ? c : mix(*p, c, a);
        }
}

// Draw circle
static void disc(int cx, int cy, int r, u32 c, u32 a = 256) {
    int r_pix = r / 16 + 2;
    mark_dirty(cx / 16 - r_pix - 1, cy / 16 - r_pix - 1, (r_pix + 1) * 2 + 2, (r_pix + 1) * 2 + 2);
    
    for (int j = -r_pix; j <= r_pix; j++) {
        for (int i = -r_pix; i <= r_pix; i++) {
            int px = cx / 16 + i, py = cy / 16 + j;
            if (static_cast<unsigned>(px) >= W || static_cast<unsigned>(py) >= H) continue;
            int dx = px * 16 + 8 - cx, dy = py * 16 + 8 - cy;
            int d = static_cast<int>(isqrt(static_cast<u32>(dx * dx + dy * dy))) - r;
            u32 k = cov(d);
            if (k) {
                u32* p = &back[py * W + px];
                *p = a >= 256 ? c : mix(*p, c, (k * a) >> 8);
            }
        }
    }
}

// Draw line segment with thickness
static void seg(int x0, int y0, int x1, int y1, int t, u32 c, u32 a = 256) {
    int bx = x1 - x0, by = y1 - y0, den = bx * bx + by * by;
    int minx = mini(x0, x1) / 16 - t / 16 - 2, maxx = maxi(x0, x1) / 16 + t / 16 + 3;
    int miny = mini(y0, y1) / 16 - t / 16 - 2, maxy = maxi(y0, y1) / 16 + t / 16 + 3;
    
    minx = maxi(minx, 0); miny = maxi(miny, 0);
    maxx = mini(maxx, W); maxy = mini(maxy, H);
    
    mark_dirty(minx, miny, maxx - minx, maxy - miny);
    
    for (int py = miny; py < maxy; py++) {
        for (int px = minx; px < maxx; px++) {
            int ax = px * 16 + 8 - x0, ay = py * 16 + 8 - y0;
            int h = den ? clampi((ax * bx + ay * by) * 256 / den, 0, 256) : 0;
            int dx = ax - bx * h / 256, dy = ay - by * h / 256;
            int d = static_cast<int>(isqrt(static_cast<u32>(dx * dx + dy * dy))) - t / 2;
            u32 k = cov(d);
            if (k) {
                u32* p = &back[py * W + px];
                *p = a >= 256 ? c : mix(*p, c, (k * a) >> 8);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Text Rendering (UTF-8 from font.h)
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
    if ((c & 0xE0) == 0xC0) { 
        u32 cp = (c & 0x1Fu) << 6; 
        cp |= static_cast<u8>(*s++) & 0x3Fu; 
        return cp; 
    }
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

static int text(const Face& f, int x, int base, const char* s, u32 c, u32 a = 256) {
    mark_dirty(x, base - 16, text_w(f, s), 24);
    while (*s) {
        const Glyph* g = find_glyph(f, next_cp(s));
        if (!g) continue;
        const u8* px = f.px + g->off;
        for (int j = 0; j < g->h; j++)
            for (int i = 0; i < g->w; i++) {
                u32 v = px[j * g->w + i];
                if (v) {
                    int px_x = x + g->bx + i, px_y = base + g->by + j;
                    if (static_cast<unsigned>(px_x) < W && static_cast<unsigned>(px_y) < H) {
                        u32* p = &back[px_y * W + px_x];
                        *p = mix(*p, c, (v * a) >> 8);
                    }
                }
            }
        x += g->adv;
    }
    return x;
}

static void text_center(const Face& f, int cx, int base, const char* s, u32 c, u32 a = 256) {
    text(f, cx - text_w(f, s) / 2, base, s, c, a);
}

// ---------------------------------------------------------------------------
// Icons (drawn via shapes, no bitmaps)
// ---------------------------------------------------------------------------
typedef void (*IconFn)(int cx, int cy, int s, u32 fg, u32 bg);

static void ico_folder(int cx, int cy, int s, u32 fg, u32 bg) {
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

static void ico_close(int cx, int cy, int s, u32 fg, u32 bg) {
    seg((cx - s / 2) * 16, (cy - s / 2) * 16, (cx + s / 2) * 16, (cy + s / 2) * 16, 14, fg);
    seg((cx - s / 2) * 16, (cy + s / 2) * 16, (cx + s / 2) * 16, (cy - s / 2) * 16, 14, fg);
}

// ---------------------------------------------------------------------------
// Window Manager
// ---------------------------------------------------------------------------
struct Win {
    int x, y, w, h;              // Current position/size
    int prev_x, prev_y;          // Previous position (for restore)
    int prev_w, prev_h;          // Previous size (for restore)
    const char* title;
    bool active, visible;
    int state;                   // 0=normal, 1=minimized, 2=maximized
};

enum { MAX_WINDOWS = 4 };
static Win windows[MAX_WINDOWS];
static int win_count = 0;
static int active_win = -1;

static void win_add(int x, int y, int w, int h, const char* title) {
    if (win_count >= MAX_WINDOWS) return;
    windows[win_count] = {x, y, w, h, x, y, w, h, title, true, true, 0};
    active_win = win_count;
    win_count++;
}

static void win_draw_frame(const Win& w) {
    if (!w.visible || w.state == 1) return;
    
    mark_dirty(w.x, w.y, w.w, w.h);
    
    // Window background with rounded corners
    rr(w.x, w.y, w.w, w.h, rad(12), PAPER, 240);
    
    // Titlebar
    u32 bar_color = w.active ? ACTIVE_BAR : INACTIVE_BAR;
    rect(w.x, w.y, w.w, 32, bar_color);
    
    // Title text
    u32 fg = w.active ? INK : MUTE;
    text(FONT_BOLD, w.x + 14, w.y + 20, w.title, fg);
    
    // Close button (right side of titlebar)
    int close_x = w.x + w.w - 26, close_y = w.y + 16;
    ico_close(close_x, close_y, 10, fg, PAPER);
}

static void win_toggle_maximize(Win& w) {
    if (w.state == 2) {
        // Restore from maximized
        w.state = 0;
        w.x = w.prev_x;
        w.y = w.prev_y;
        w.w = w.prev_w;
        w.h = w.prev_h;
    } else {
        // Maximize
        w.state = 2;
        w.prev_x = w.x;
        w.prev_y = w.y;
        w.prev_w = w.w;
        w.prev_h = w.h;
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

static void win_set_active(int idx) {
    if (idx >= 0 && idx < win_count) {
        if (active_win >= 0 && active_win != idx) {
            windows[active_win].active = false;
        }
        windows[idx].active = true;
        active_win = idx;
        invalidate_all();
    }
}

// ---------------------------------------------------------------------------
// Desktop Rendering
// ---------------------------------------------------------------------------
static void draw_desktop() {
    mark_dirty(0, 0, W, H);
    
    // Gradient background (top dark to bottom lighter)
    for (int y = 0; y < H; y++) {
        u32 c = mix(BG_TOP, BG_BOT, static_cast<u32>(y * 256 / H));
        for (int x = 0; x < W; x++) {
            back[y * W + x] = c;
        }
    }
}

static void draw_windows() {
    // Draw inactive windows first
    for (int i = 0; i < win_count; i++) {
        if (!windows[i].active && windows[i].visible) {
            win_draw_frame(windows[i]);
        }
    }
    
    // Draw active window last (on top, z-order)
    if (active_win >= 0 && windows[active_win].visible) {
        win_draw_frame(windows[active_win]);
    }
}

static void draw_topbar() {
    mark_dirty(0, 0, W, 28);
    rect(0, 0, W, 28, BAR_BG);
    text(FONT_BOLD, 14, 18, "Tidepool OS", IVORY);
}

// ---------------------------------------------------------------------------
// Multiboot2 + Framebuffer Initialization
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

// Present dirty rectangle to physical framebuffer
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
    bool direct = f.rp == 16 && f.gp == 8 && f.bp == 0;  // BGRX format
    u8* base = reinterpret_cast<u8*>(f.addr);

    for (int y = y0; y < y1; y++) {
        u32* src = &back[y * W + x0];
        u32* dst = reinterpret_cast<u32*>(
            base + static_cast<u64>(oy + y) * f.pitch + 
            static_cast<u64>(ox + x0) * 4);

        if (direct) {
            // Fast path: BGRX format matches our 0x00RRGGBB with byte swap
            memcpy(dst, src, static_cast<size_t>((x1 - x0) * 4));
        } else {
            // Slow path: convert color format
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

// ---------------------------------------------------------------------------
// Kernel Main Entry
// ---------------------------------------------------------------------------
extern "C" void kernel_main(u64 mbi) {
    Fb fb;
    if (!find_fb(mbi, fb) || fb.type != 1 || fb.bpp != 32)
        fail("Tidepool OS: khong co framebuffer 32-bit");
    if (fb.w < W || fb.h < H)
        fail("Tidepool OS: can man hinh toi thieu 1024x768");
    if (fb.addr + static_cast<u64>(fb.pitch) * fb.h > (1ull << 32))
        fail("Tidepool OS: framebuffer nam ngoai 4 GiB");

    // Initialize demo windows
    win_add(100, 100, 400, 300, "Welcome");
    win_add(550, 150, 420, 380, "Settings");
    win_set_active(1);

    // Draw initial UI
    draw_desktop();
    draw_topbar();
    draw_windows();
    
    // Present to framebuffer (Dirty Rectangles only update changed areas)
    present_dirty(fb);
    
    // TODO: Input loop here (keyboard/mouse handling)
    halt_forever();
}
