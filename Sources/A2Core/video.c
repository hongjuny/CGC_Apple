// Video: 40/80-column text, lo-res, hi-res, double lo-res and double hi-res
// rendered into a 560x384 RGBA framebuffer. Each scanline is first expanded
// to 560 "dots" (the 14.318 MHz pixel clock); colour graphics are derived from
// a 4-dot NTSC phase window (MAME's "4-bit square filter"), which produces
// hi-res artifact colours and white runs naturally.
#include "a2internal.h"

#define RGB(r, g, b) ((uint32_t)(r) | ((uint32_t)(g) << 8) | ((uint32_t)(b) << 16) | 0xFF000000u)

// Lo-res colour n shows dot pattern n, bit (x & 3) at dot x.
static const uint32_t palette[16] = {
    RGB(0x00, 0x00, 0x00), RGB(0xDD, 0x00, 0x33), RGB(0x00, 0x00, 0x99), RGB(0xDD, 0x22, 0xDD),
    RGB(0x00, 0x77, 0x22), RGB(0x55, 0x55, 0x55), RGB(0x22, 0x22, 0xFF), RGB(0x66, 0xAA, 0xFF),
    RGB(0x88, 0x55, 0x00), RGB(0xFF, 0x66, 0x00), RGB(0xAA, 0xAA, 0xAA), RGB(0xFF, 0x99, 0x88),
    RGB(0x11, 0xDD, 0x00), RGB(0xFF, 0xFF, 0x00), RGB(0x44, 0xFF, 0x99), RGB(0xFF, 0xFF, 0xFF),
};

static uint32_t mono_color(a2_monitor m)
{
    switch (m) {
    case A2_MONITOR_GREEN: return RGB(0x33, 0xFF, 0x66);
    case A2_MONITOR_AMBER: return RGB(0xFF, 0xB0, 0x00);
    default:               return RGB(0xFF, 0xFF, 0xFF);
    }
}

static uint32_t dim(uint32_t c)
{
    return ((c >> 1) & 0x007F7F7Fu) | 0xFF000000u;
}

static uint8_t rotl4(uint8_t v, int n)
{
    n &= 3;
    return (uint8_t)(((v << n) | (v >> (4 - n))) & 0x0F);
}

static uint16_t text_row_address(int row)  // row 0..23
{
    return (uint16_t)(((row & 7) << 7) + (row >> 3) * 40);
}

static uint16_t hires_line_address(int y)  // y 0..191
{
    return (uint16_t)(((y & 7) << 10) + (((y >> 3) & 7) << 7) + (y >> 6) * 40);
}

// With 80STORE on, PAGE2 banks memory instead of selecting the display page.
static bool display_page2(const apple2 *a2)
{
    return a2->page2 && !(a2_is_iie(a2) && a2->store80);
}

bool video_in_vbl(uint64_t cycle)
{
    return (cycle % A2_CYCLES_PER_FRAME) >= 192 * A2_CYCLES_PER_LINE;
}

uint16_t video_scan_address(const apple2 *a2, uint64_t cycle)
{
    unsigned c = (unsigned)(cycle % A2_CYCLES_PER_FRAME);
    unsigned v = (c / A2_CYCLES_PER_LINE) % 192;
    unsigned h = c % A2_CYCLES_PER_LINE;
    unsigned col = h < 25 ? 0 : h - 25;
    bool gfx_line = !a2->text && !(a2->mixed && v >= 160);
    bool page2 = display_page2(a2);
    if (gfx_line && a2->hires)
        return (uint16_t)((page2 ? 0x4000 : 0x2000) + hires_line_address((int)v) + col);
    return (uint16_t)((page2 ? 0x0800 : 0x0400) + text_row_address((int)v >> 3) + col);
}

// ---- text ---------------------------------------------------------------------

// Returns the 7 pixels of a character row with bit x = pixel x (leftmost = bit 0).
static uint8_t glyph_row(const apple2 *a2, uint8_t code, int row, bool flash_on)
{
    if (!a2_is_iie(a2)) {
        // 341-0036: bit 6 is the leftmost pixel; $00-$3F inverse, $40-$7F flashing.
        uint8_t bits = a2->char_rom[code * 8 + row] & 0x7F;
        if (code < 0x40 || (code < 0x80 && flash_on))
            bits ^= 0x7F;
        uint8_t out = 0;
        for (int x = 0; x < 7; x++)
            out |= (uint8_t)(((bits >> (6 - x)) & 1) << x);
        return out;
    }
    // IIe video ROM: bit 0 is the leftmost pixel and data is stored inverted
    // (MAME a2_video_device::get_text_character, Invert=false, Flip=false).
    uint8_t invert = 0x7F;
    if (!a2->altchar) {
        if (code >= 0x40 && code <= 0x7F) {
            code &= 0x3F;
            if (flash_on)
                invert ^= 0x7F;
        }
    } else if (code >= 0x60 && code <= 0x7F) {
        code |= 0x80;        // lowercase inverse instead of flashing
        invert ^= 0x7F;
    }
    return (uint8_t)((a2->char_rom[code * 8 + row] & 0x7F) ^ invert);
}

static void text_dots(const apple2 *a2, int y, uint8_t *dots)
{
    uint16_t addr = (uint16_t)((display_page2(a2) ? 0x0800 : 0x0400) + text_row_address(y >> 3));
    bool flash_on = (a2->frame_count & 0x10) != 0;

    if (a2_is_iie(a2) && a2->col80) {
        // 80 columns: aux and main characters alternate, one dot per pixel.
        for (int col = 0; col < 40; col++) {
            uint8_t aux = glyph_row(a2, a2->ram[1][addr + col], y & 7, flash_on);
            uint8_t main_ = glyph_row(a2, a2->ram[0][addr + col], y & 7, flash_on);
            for (int x = 0; x < 7; x++) {
                dots[col * 14 + x] = (aux >> x) & 1;
                dots[col * 14 + 7 + x] = (main_ >> x) & 1;
            }
        }
        return;
    }
    for (int col = 0; col < 40; col++) {
        uint8_t bits = glyph_row(a2, a2->ram[0][addr + col], y & 7, flash_on);
        for (int x = 0; x < 7; x++) {
            uint8_t on = (bits >> x) & 1;
            dots[(col * 7 + x) * 2] = on;
            dots[(col * 7 + x) * 2 + 1] = on;
        }
    }
}

// ---- graphics -------------------------------------------------------------------

static void hires_dots(const apple2 *a2, int y, uint8_t *dots)
{
    const uint8_t *line = a2->ram[0] + (display_page2(a2) ? 0x4000 : 0x2000) + hires_line_address(y);
    for (int i = 0; i < A2_SCREEN_W + 1; i++)
        dots[i] = 0;
    for (int col = 0; col < 40; col++) {
        uint8_t v = line[col];
        int shift = (v & 0x80) ? 1 : 0;   // palette bit delays the byte by half a pixel
        for (int b = 0; b < 7; b++) {
            if (!((v >> b) & 1))
                continue;
            int x = (col * 7 + b) * 2 + shift;
            dots[x] = 1;
            dots[x + 1] = 1;
        }
    }
}

static void double_hires_dots(const apple2 *a2, int y, uint8_t *dots)
{
    uint16_t addr = (uint16_t)((display_page2(a2) ? 0x4000 : 0x2000) + hires_line_address(y));
    for (int col = 0; col < 40; col++) {
        uint8_t aux = a2->ram[1][addr + col], main_ = a2->ram[0][addr + col];
        for (int b = 0; b < 7; b++) {
            dots[col * 14 + b] = (aux >> b) & 1;
            dots[col * 14 + 7 + b] = (main_ >> b) & 1;
        }
    }
}

static void lores_dots(const apple2 *a2, int y, bool dbl, uint8_t *dots, uint8_t *colors)
{
    uint16_t addr = (uint16_t)((display_page2(a2) ? 0x0800 : 0x0400) + text_row_address(y >> 3));
    int shift = (y & 4) ? 4 : 0;
    for (int col = 0; col < 40; col++) {
        uint8_t c = (uint8_t)((a2->ram[0][addr + col] >> shift) & 0x0F);
        int first = col * 14;
        if (dbl) {
            // Double lo-res: aux block (colour bits rotated) then main block, 7 dots each.
            uint8_t ca = rotl4((uint8_t)((a2->ram[1][addr + col] >> shift) & 0x0F), 1);
            for (int x = first; x < first + 7; x++) {
                dots[x] = (ca >> (x & 3)) & 1;
                colors[x] = ca;
            }
            first += 7;
            for (int x = first; x < first + 7; x++) {
                dots[x] = (c >> (x & 3)) & 1;
                colors[x] = c;
            }
            continue;
        }
        for (int x = first; x < first + 14; x++) {
            dots[x] = (c >> (x & 3)) & 1;
            colors[x] = c;
        }
    }
}

// Colour from the 4-dot window around x; phase shifts the chroma reference
// (double hi-res runs one dot ahead of hi-res).
static void artifact_line(const uint8_t *dots, int phase, uint32_t *out)
{
    for (int x = 0; x < A2_SCREEN_W; x++) {
        unsigned pattern = 0;
        for (int k = -1; k <= 2; k++) {
            int xx = x + k;
            if (xx >= 0 && xx < A2_SCREEN_W && dots[xx])
                pattern |= 1u << ((xx + phase) & 3);
        }
        out[x] = palette[pattern];
    }
}

void video_render(apple2 *a2)
{
    uint8_t dots[A2_SCREEN_W + 4];
    uint8_t colors[A2_SCREEN_W];
    uint32_t mono = mono_color(a2->monitor);
    bool color_monitor = a2->monitor == A2_MONITOR_COLOR;
    bool e = a2_is_iie(a2);
    bool dbl = e && a2->col80 && a2->dhires;

    for (int y = 0; y < 192; y++) {
        uint32_t *out = a2->fb + (y * 2) * A2_SCREEN_W;
        bool text_line = a2->text || (a2->mixed && y >= 160);

        if (text_line) {
            text_dots(a2, y, dots);
            uint32_t fg = color_monitor ? RGB(0xFF, 0xFF, 0xFF) : mono;
            for (int x = 0; x < A2_SCREEN_W; x++)
                out[x] = dots[x] ? fg : palette[0];
        } else if (a2->hires) {
            if (dbl)
                double_hires_dots(a2, y, dots);
            else
                hires_dots(a2, y, dots);
            if (color_monitor) {
                artifact_line(dots, dbl ? 1 : 0, out);
            } else {
                for (int x = 0; x < A2_SCREEN_W; x++)
                    out[x] = dots[x] ? mono : palette[0];
            }
        } else {
            lores_dots(a2, y, dbl, dots, colors);
            for (int x = 0; x < A2_SCREEN_W; x++)
                out[x] = color_monitor ? palette[colors[x]] : (dots[x] ? mono : palette[0]);
        }

        uint32_t *next = out + A2_SCREEN_W;
        for (int x = 0; x < A2_SCREEN_W; x++)
            next[x] = a2->scanlines ? dim(out[x]) : out[x];
    }
}
