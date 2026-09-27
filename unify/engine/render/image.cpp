// QOI ("Quite OK Image") codec for texture assets, and an uncompressed PNG writer for screenshots.
#include "render.h"
#include <cstdio>
#include <cstring>

namespace unify {

static uint32_t qoi_hash(const uint8_t* p) { return (p[0] * 3u + p[1] * 5u + p[2] * 7u + p[3] * 11u) % 64u; }

bool decode_qoi(const std::vector<uint8_t>& d, Image& out) {
    if (d.size() < 22 || std::memcmp(d.data(), "qoif", 4)) return false;
    auto be32 = [&](size_t o) { return uint32_t(d[o]) << 24 | uint32_t(d[o + 1]) << 16 | uint32_t(d[o + 2]) << 8 | d[o + 3]; };
    uint32_t w = be32(4), h = be32(8);
    if (w == 0 || h == 0 || w > 16384 || h > 16384) return false;
    out = Image(int(w), int(h));
    uint8_t index[64 * 4] = {};
    uint8_t px[4] = {0, 0, 0, 255};
    size_t p = 14, end = d.size() - 8;
    int run = 0;
    for (size_t i = 0; i < size_t(w) * h; i++) {
        if (run > 0) { run--; }
        else if (p < end) {
            uint8_t b = d[p++];
            if (b == 0xFE) { px[0] = d[p]; px[1] = d[p + 1]; px[2] = d[p + 2]; p += 3; }
            else if (b == 0xFF) { std::memcpy(px, &d[p], 4); p += 4; }
            else if ((b & 0xC0) == 0x00) { std::memcpy(px, &index[(b & 0x3F) * 4], 4); }
            else if ((b & 0xC0) == 0x40) { px[0] += ((b >> 4) & 3) - 2; px[1] += ((b >> 2) & 3) - 2; px[2] += (b & 3) - 2; }
            else if ((b & 0xC0) == 0x80) {
                uint8_t b2 = d[p++];
                int vg = (b & 0x3F) - 32;
                px[0] += uint8_t(vg - 8 + ((b2 >> 4) & 0xF));
                px[1] += uint8_t(vg);
                px[2] += uint8_t(vg - 8 + (b2 & 0xF));
            } else { run = b & 0x3F; }
            std::memcpy(&index[qoi_hash(px) * 4], px, 4);
        }
        std::memcpy(&out.pixels[i * 4], px, 4);
    }
    return true;
}

std::vector<uint8_t> encode_qoi(const Image& img) {
    std::vector<uint8_t> o;
    auto be32 = [&](uint32_t v) { for (int i = 3; i >= 0; i--) o.push_back(uint8_t(v >> (8 * i))); };
    o.insert(o.end(), {'q', 'o', 'i', 'f'});
    be32(uint32_t(img.width)); be32(uint32_t(img.height));
    o.push_back(4); o.push_back(0);
    uint8_t index[64 * 4] = {};
    uint8_t prev[4] = {0, 0, 0, 255};
    int run = 0;
    size_t n = size_t(img.width) * img.height;
    for (size_t i = 0; i < n; i++) {
        const uint8_t* px = &img.pixels[i * 4];
        if (!std::memcmp(px, prev, 4)) {
            if (++run == 62 || i == n - 1) { o.push_back(uint8_t(0xC0 | (run - 1))); run = 0; }
            continue;
        }
        if (run) { o.push_back(uint8_t(0xC0 | (run - 1))); run = 0; }
        uint32_t h = qoi_hash(px);
        if (!std::memcmp(&index[h * 4], px, 4)) { o.push_back(uint8_t(h)); }
        else {
            std::memcpy(&index[h * 4], px, 4);
            if (px[3] == prev[3]) {
                int8_t vr = int8_t(px[0] - prev[0]), vg = int8_t(px[1] - prev[1]), vb = int8_t(px[2] - prev[2]);
                int8_t vgr = int8_t(vr - vg), vgb = int8_t(vb - vg);
                if (vr > -3 && vr < 2 && vg > -3 && vg < 2 && vb > -3 && vb < 2) o.push_back(uint8_t(0x40 | (vr + 2) << 4 | (vg + 2) << 2 | (vb + 2)));
                else if (vgr > -9 && vgr < 8 && vg > -33 && vg < 32 && vgb > -9 && vgb < 8) { o.push_back(uint8_t(0x80 | (vg + 32))); o.push_back(uint8_t((vgr + 8) << 4 | (vgb + 8))); }
                else { o.push_back(0xFE); o.push_back(px[0]); o.push_back(px[1]); o.push_back(px[2]); }
            } else { o.push_back(0xFF); o.insert(o.end(), px, px + 4); }
        }
        std::memcpy(prev, px, 4);
    }
    o.insert(o.end(), {0, 0, 0, 0, 0, 0, 0, 1});
    return o;
}

bool load_image(const std::string& path, Image& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::vector<uint8_t> data;
    uint8_t buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) data.insert(data.end(), buf, buf + n);
    std::fclose(f);
    return decode_qoi(data, out);
}

static uint32_t crc_table[256];
static void init_crc() {
    for (uint32_t n = 0; n < 256; n++) { uint32_t c = n; for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; crc_table[n] = c; }
}
static uint32_t crc(const uint8_t* p, size_t n, uint32_t c = 0xFFFFFFFFu) { for (size_t i = 0; i < n; i++) c = crc_table[(c ^ p[i]) & 0xFF] ^ (c >> 8); return c; }

bool write_png(const std::string& path, const Image& img) {
    if (!crc_table[1]) init_crc();
    // Raw scanlines (filter 0) in stored (uncompressed) deflate blocks inside a zlib stream.
    std::vector<uint8_t> raw;
    for (int y = 0; y < img.height; y++) { raw.push_back(0); raw.insert(raw.end(), img.at(0, y), img.at(0, y) + img.width * 4); }
    std::vector<uint8_t> z = {0x78, 0x01};
    uint32_t a = 1, b = 0;
    for (uint8_t v : raw) { a = (a + v) % 65521; b = (b + a) % 65521; }
    for (size_t off = 0; off < raw.size() || off == 0; off += 65535) {
        size_t len = std::min<size_t>(65535, raw.size() - off);
        z.push_back(off + len >= raw.size() ? 1 : 0);
        z.push_back(uint8_t(len)); z.push_back(uint8_t(len >> 8));
        z.push_back(uint8_t(~len)); z.push_back(uint8_t(~len >> 8));
        z.insert(z.end(), raw.begin() + long(off), raw.begin() + long(off + len));
        if (raw.empty()) break;
    }
    uint32_t adler = b << 16 | a;
    for (int i = 3; i >= 0; i--) z.push_back(uint8_t(adler >> (8 * i)));

    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    auto chunk = [&](const char* type, const std::vector<uint8_t>& data) {
        uint8_t len[4] = {uint8_t(data.size() >> 24), uint8_t(data.size() >> 16), uint8_t(data.size() >> 8), uint8_t(data.size())};
        std::fwrite(len, 1, 4, f);
        std::fwrite(type, 1, 4, f);
        if (!data.empty()) std::fwrite(data.data(), 1, data.size(), f);
        uint32_t c = crc(reinterpret_cast<const uint8_t*>(type), 4);
        c = crc(data.data(), data.size(), c) ^ 0xFFFFFFFFu;
        uint8_t cb[4] = {uint8_t(c >> 24), uint8_t(c >> 16), uint8_t(c >> 8), uint8_t(c)};
        std::fwrite(cb, 1, 4, f);
    };
    const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    std::fwrite(sig, 1, 8, f);
    std::vector<uint8_t> ihdr = {uint8_t(img.width >> 24), uint8_t(img.width >> 16), uint8_t(img.width >> 8), uint8_t(img.width),
                                 uint8_t(img.height >> 24), uint8_t(img.height >> 16), uint8_t(img.height >> 8), uint8_t(img.height), 8, 6, 0, 0, 0};
    chunk("IHDR", ihdr);
    chunk("IDAT", z);
    chunk("IEND", {});
    return std::fclose(f) == 0;
}

// ------------------------------------------------------------------------------ bitmap font

// Classic 5x7 font, ASCII 32..126, column-major, bit 0 = top row.
static const uint8_t kFont5x7[95][5] = {
    {0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x5F,0x00,0x00},{0x00,0x07,0x00,0x07,0x00},{0x14,0x7F,0x14,0x7F,0x14},
    {0x24,0x2A,0x7F,0x2A,0x12},{0x23,0x13,0x08,0x64,0x62},{0x36,0x49,0x55,0x22,0x50},{0x00,0x05,0x03,0x00,0x00},
    {0x00,0x1C,0x22,0x41,0x00},{0x00,0x41,0x22,0x1C,0x00},{0x08,0x2A,0x1C,0x2A,0x08},{0x08,0x08,0x3E,0x08,0x08},
    {0x00,0x50,0x30,0x00,0x00},{0x08,0x08,0x08,0x08,0x08},{0x00,0x60,0x60,0x00,0x00},{0x20,0x10,0x08,0x04,0x02},
    {0x3E,0x51,0x49,0x45,0x3E},{0x00,0x42,0x7F,0x40,0x00},{0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4B,0x31},
    {0x18,0x14,0x12,0x7F,0x10},{0x27,0x45,0x45,0x45,0x39},{0x3C,0x4A,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1E},{0x00,0x36,0x36,0x00,0x00},{0x00,0x56,0x36,0x00,0x00},
    {0x08,0x14,0x22,0x41,0x00},{0x14,0x14,0x14,0x14,0x14},{0x00,0x41,0x22,0x14,0x08},{0x02,0x01,0x51,0x09,0x06},
    {0x32,0x49,0x79,0x41,0x3E},{0x7E,0x11,0x11,0x11,0x7E},{0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22},
    {0x7F,0x41,0x41,0x22,0x1C},{0x7F,0x49,0x49,0x49,0x41},{0x7F,0x09,0x09,0x01,0x01},{0x3E,0x41,0x41,0x51,0x32},
    {0x7F,0x08,0x08,0x08,0x7F},{0x00,0x41,0x7F,0x41,0x00},{0x20,0x40,0x41,0x3F,0x01},{0x7F,0x08,0x14,0x22,0x41},
    {0x7F,0x40,0x40,0x40,0x40},{0x7F,0x02,0x04,0x02,0x7F},{0x7F,0x04,0x08,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,0x09,0x09,0x09,0x06},{0x3E,0x41,0x51,0x21,0x5E},{0x7F,0x09,0x19,0x29,0x46},{0x46,0x49,0x49,0x49,0x31},
    {0x01,0x01,0x7F,0x01,0x01},{0x3F,0x40,0x40,0x40,0x3F},{0x1F,0x20,0x40,0x20,0x1F},{0x7F,0x20,0x18,0x20,0x7F},
    {0x63,0x14,0x08,0x14,0x63},{0x03,0x04,0x78,0x04,0x03},{0x61,0x51,0x49,0x45,0x43},{0x00,0x00,0x7F,0x41,0x41},
    {0x02,0x04,0x08,0x10,0x20},{0x41,0x41,0x7F,0x00,0x00},{0x04,0x02,0x01,0x02,0x04},{0x40,0x40,0x40,0x40,0x40},
    {0x00,0x01,0x02,0x04,0x00},{0x20,0x54,0x54,0x54,0x78},{0x7F,0x48,0x44,0x44,0x38},{0x38,0x44,0x44,0x44,0x20},
    {0x38,0x44,0x44,0x48,0x7F},{0x38,0x54,0x54,0x54,0x18},{0x08,0x7E,0x09,0x01,0x02},{0x08,0x14,0x54,0x54,0x3C},
    {0x7F,0x08,0x04,0x04,0x78},{0x00,0x44,0x7D,0x40,0x00},{0x20,0x40,0x44,0x3D,0x00},{0x00,0x7F,0x10,0x28,0x44},
    {0x00,0x41,0x7F,0x40,0x00},{0x7C,0x04,0x18,0x04,0x78},{0x7C,0x08,0x04,0x04,0x78},{0x38,0x44,0x44,0x44,0x38},
    {0x7C,0x14,0x14,0x14,0x08},{0x08,0x14,0x14,0x18,0x7C},{0x7C,0x08,0x04,0x04,0x08},{0x48,0x54,0x54,0x54,0x20},
    {0x04,0x3F,0x44,0x40,0x20},{0x3C,0x40,0x40,0x20,0x7C},{0x1C,0x20,0x40,0x20,0x1C},{0x3C,0x40,0x30,0x40,0x3C},
    {0x44,0x28,0x10,0x28,0x44},{0x0C,0x50,0x50,0x50,0x3C},{0x44,0x64,0x54,0x4C,0x44},{0x00,0x08,0x36,0x41,0x00},
    {0x00,0x00,0x7F,0x00,0x00},{0x00,0x41,0x36,0x08,0x00},{0x08,0x08,0x2A,0x1C,0x08},
};

BitmapFont BitmapFont::build() {
    BitmapFont f;
    f.image = Image(16 * 6, 6 * 8);
    for (int c = 0; c < 95; c++) {
        int cx = (c % 16) * 6, cy = (c / 16) * 8;
        for (int col = 0; col < 5; col++)
            for (int row = 0; row < 7; row++)
                if (kFont5x7[c][col] >> row & 1) {
                    uint8_t* p = f.image.at(cx + col, cy + row);
                    p[0] = p[1] = p[2] = p[3] = 255;
                }
    }
    return f;
}

RectF BitmapFont::glyph(char c) const {
    int i = (c < 32 || c > 126) ? '?' - 32 : c - 32;
    return {float((i % 16) * 6), float((i / 16) * 8), 6, 8};
}

void BitmapFont::draw(Renderer2D& r, const std::string& text, Vec2 pos, float scale, Color color, int16_t layer) const {
    float x = pos.x;
    for (char c : text) {
        if (c != ' ') r.screen_rect(texture, glyph(c), {x, pos.y, 6 * scale, 8 * scale}, color, layer);
        x += 6 * scale;
    }
}

}  // namespace unify
