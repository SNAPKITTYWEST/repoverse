// Generates the demo game's binary assets (original pixel art, SFX, streamed music) so
// the repository ships no third-party media. Deterministic: same output every run.
//   unify_assetgen <assets-dir>
#include "render/render.h"
#include "audio/audio.h"
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include <sys/stat.h>

using namespace unify;

static void px(Image& im, int x, int y, Color c) {
    if (x < 0 || y < 0 || x >= im.width || y >= im.height) return;
    uint8_t* p = im.at(x, y);
    p[0] = c.r; p[1] = c.g; p[2] = c.b; p[3] = c.a;
}
static void rect(Image& im, int x, int y, int w, int h, Color c) { for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) px(im, x + i, y + j, c); }
static void disc(Image& im, float cx, float cy, float r, Color c) {
    for (int y = 0; y < im.height; y++) for (int x = 0; x < im.width; x++)
        if ((x + 0.5f - cx) * (x + 0.5f - cx) + (y + 0.5f - cy) * (y + 0.5f - cy) <= r * r) px(im, x, y, c);
}

// Player: 16x24, a small robot courier. `pose`: 0/1 idle breathe, 2..5 run cycle, 6 jump, 7 fall.
static Image player(int pose) {
    Image im(16, 24);
    const Color body{64, 156, 255, 255}, dark{30, 70, 150, 255}, visor{240, 250, 255, 255}, eye{20, 20, 40, 255}, boot{40, 40, 52, 255};
    int bob = pose == 1 ? 1 : 0;
    rect(im, 3, 3 + bob, 10, 9, body);           // head
    rect(im, 4, 5 + bob, 8, 4, visor);           // visor
    rect(im, 6, 6 + bob, 1, 2, eye);
    rect(im, 9, 6 + bob, 1, 2, eye);
    rect(im, 7, 1 + bob, 2, 2, dark);            // antenna
    rect(im, 4, 12 + bob, 8, 7, body);           // torso
    rect(im, 5, 14 + bob, 6, 1, dark);
    int lf = 0, rf = 0;                          // leg offsets
    if (pose >= 2 && pose <= 5) { static const int cyc[4][2] = {{-2, 2}, {0, 0}, {2, -2}, {0, 0}}; lf = cyc[pose - 2][0]; rf = cyc[pose - 2][1]; }
    if (pose == 6) { lf = -1; rf = 1; }
    rect(im, 5 + lf, 19, 2, 4, dark);
    rect(im, 9 + rf, 19, 2, 4, dark);
    rect(im, 4 + lf, 22, 3, 2, boot);
    rect(im, 9 + rf, 22, 3, 2, boot);
    if (pose == 6) { rect(im, 1, 9, 3, 2, body); rect(im, 12, 9, 3, 2, body); }       // arms up
    else if (pose == 7) { rect(im, 1, 13, 3, 2, body); rect(im, 12, 13, 3, 2, body); } // arms out
    else { rect(im, 2, 13 + bob, 2, 5, body); rect(im, 12, 13 + bob, 2, 5, body); }
    return im;
}

static Image tile(bool top) {
    Image im(16, 16, {120, 84, 52, 255});
    for (int i = 0; i < 16; i++) for (int j = 0; j < 16; j++) if (((i * 7 + j * 13) % 11) == 0) px(im, i, j, {98, 66, 40, 255});
    if (top) { rect(im, 0, 0, 16, 4, {86, 190, 90, 255}); for (int i = 0; i < 16; i += 3) px(im, i, 4, {86, 190, 90, 255}); rect(im, 0, 0, 16, 1, {140, 230, 130, 255}); }
    return im;
}

static Image crate() {
    Image im(16, 16, {176, 122, 60, 255});
    rect(im, 0, 0, 16, 2, {120, 80, 36, 255}); rect(im, 0, 14, 16, 2, {120, 80, 36, 255});
    rect(im, 0, 0, 2, 16, {120, 80, 36, 255}); rect(im, 14, 0, 2, 16, {120, 80, 36, 255});
    for (int i = 2; i < 14; i++) { px(im, i, i, {120, 80, 36, 255}); px(im, i, 15 - i, {120, 80, 36, 255}); }
    return im;
}

static Image coin(int f) {
    Image im(12, 12);
    float w = std::fabs(std::cos(f * 3.14159f / 4)) * 5 + 1;
    for (int y = 0; y < 12; y++) for (int x = 0; x < 12; x++) {
        float dx = (x + 0.5f - 6) / w, dy = (y + 0.5f - 6) / 5.5f;
        if (dx * dx + dy * dy <= 1) px(im, x, y, dx * dx + dy * dy < 0.4f ? Color{255, 236, 120, 255} : Color{232, 176, 40, 255});
    }
    return im;
}

static Image ball() { Image im(16, 16); disc(im, 8, 8, 7.5f, {230, 70, 90, 255}); disc(im, 6, 5, 2.5f, {255, 180, 190, 255}); return im; }
static Image spark() { Image im(4, 4); rect(im, 1, 0, 2, 4, {255, 250, 200, 255}); rect(im, 0, 1, 4, 2, {255, 250, 200, 255}); return im; }
static Image flag(int f) {
    Image im(16, 32);
    rect(im, 2, 0, 2, 32, {220, 220, 230, 255});
    for (int y = 0; y < 10; y++) for (int x = 0; x < 10; x++) {
        int wave = int(std::round(std::sin((x + f * 2) * 0.8) * 1.2));
        px(im, 4 + x, 2 + y + wave, (x / 2 + y / 2) % 2 ? Color{40, 40, 40, 255} : Color{250, 250, 250, 255});
    }
    return im;
}
static Image platform() {
    Image im(48, 12, {90, 100, 128, 255});
    rect(im, 0, 0, 48, 2, {150, 170, 210, 255});
    for (int x = 2; x < 48; x += 8) rect(im, x, 5, 4, 3, {60, 66, 90, 255});
    return im;
}
static Image sky() {
    Image im(16, 64);
    for (int y = 0; y < 64; y++) { float t = y / 63.0f; rect(im, 0, y, 16, 1, {uint8_t(40 + 50 * t), uint8_t(60 + 90 * t), uint8_t(120 + 100 * t), 255}); }
    return im;
}
static Image cloud() {
    Image im(32, 16);
    disc(im, 9, 10, 6, {245, 248, 255, 255}); disc(im, 17, 7, 7, {245, 248, 255, 255}); disc(im, 24, 10, 6, {245, 248, 255, 255});
    rect(im, 8, 10, 18, 6, {245, 248, 255, 255});
    return im;
}

// ---------------------------------------------------------------------------------- audio
static std::vector<int16_t> render_sfx(float seconds, float (*fn)(float t, float dur)) {
    int n = int(seconds * 44100);
    std::vector<int16_t> pcm(size_t(n) * 2);
    for (int i = 0; i < n; i++) {
        float v = fn(i / 44100.0f, seconds);
        int16_t s = int16_t(std::lround(std::max(-1.0f, std::min(1.0f, v)) * 20000));
        pcm[size_t(i) * 2] = pcm[size_t(i) * 2 + 1] = s;
    }
    return pcm;
}
static float square(float phase) { return std::fmod(phase, 1.0f) < 0.5f ? 1.0f : -1.0f; }

static std::vector<int16_t> render_music(int* loop_start, int* loop_end) {
    // 8 bars of a looping chiptune at 120 BPM (16 s). Loop points skip the 2-bar intro.
    const float bpm = 120, beat = 60 / bpm;
    const int bars = 10, total = int(bars * 4 * beat * 44100);
    std::vector<float> l(size_t(total), 0), r(size_t(total), 0);
    static const int bass[8] = {45, 45, 41, 41, 43, 43, 40, 40};
    static const int melody[32] = {69, 72, 76, 72, 74, 72, 69, 67, 65, 69, 72, 69, 67, 71, 74, 71,
                                   69, 72, 76, 79, 77, 76, 74, 72, 71, 72, 74, 76, 72, 71, 69, -1};
    auto note_freq = [](int n) { return 440.0f * std::pow(2.0f, (n - 69) / 12.0f); };
    for (int i = 0; i < total; i++) {
        float t = i / 44100.0f;
        int step = int(t / (beat / 2));              // eighth notes
        int bar = int(t / (4 * beat));
        float in_step = std::fmod(t, beat / 2) / (beat / 2);
        float env = std::exp(-in_step * 3);
        float b = square(t * note_freq(bass[(bar * 2 + (step % 8) / 4) % 8] - 12)) * 0.18f * (0.6f + 0.4f * env);
        float m = 0;
        if (bar >= 2) {
            int n = melody[step % 32];
            if (n > 0) m = square(t * note_freq(n)) * 0.12f * env + square(t * note_freq(n) * 1.005f) * 0.05f * env;
        }
        float hat = (step % 2 == 1 && in_step < 0.08f) ? (float((i * 1103515245u + 12345u) >> 16 & 0xFF) / 128.0f - 1) * 0.06f * (1 - in_step / 0.08f) : 0;
        l[size_t(i)] = b + m * 0.8f + hat;
        r[size_t(i)] = b + m + hat * 0.7f;
    }
    std::vector<int16_t> pcm(size_t(total) * 2);
    for (int i = 0; i < total; i++) {
        pcm[size_t(i) * 2] = int16_t(std::lround(std::max(-1.0f, std::min(1.0f, l[size_t(i)])) * 26000));
        pcm[size_t(i) * 2 + 1] = int16_t(std::lround(std::max(-1.0f, std::min(1.0f, r[size_t(i)])) * 26000));
    }
    *loop_start = int(2 * 4 * beat * 44100);
    *loop_end = total;
    return pcm;
}

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: unify_assetgen <assets-dir>\n"); return 2; }
    std::string root = argv[1];
    for (const char* d : {"textures", "audio", "music", "saves"}) mkdir((root + "/" + d).c_str(), 0755);

    std::vector<std::pair<std::string, Image>> sprites;
    const char* poses[8] = {"player_idle_0", "player_idle_1", "player_run_0", "player_run_1", "player_run_2", "player_run_3", "player_jump", "player_fall"};
    for (int i = 0; i < 8; i++) sprites.push_back({poses[i], player(i)});
    sprites.push_back({"tile_top", tile(true)});
    sprites.push_back({"tile", tile(false)});
    sprites.push_back({"crate", crate()});
    for (int i = 0; i < 4; i++) sprites.push_back({"coin_" + std::to_string(i), coin(i)});
    sprites.push_back({"ball", ball()});
    sprites.push_back({"spark", spark()});
    sprites.push_back({"flag_0", flag(0)});
    sprites.push_back({"flag_1", flag(1)});
    sprites.push_back({"platform", platform()});
    sprites.push_back({"sky", sky()});
    sprites.push_back({"cloud", cloud()});
    std::vector<std::pair<std::string, const Image*>> items;
    for (auto& s : sprites) items.push_back({s.first, &s.second});
    Image atlas_img;
    Atlas atlas;
    if (!pack_atlas(items, 128, atlas_img, atlas)) { std::fprintf(stderr, "atlas too small\n"); return 1; }
    auto qoi = encode_qoi(atlas_img);
    FILE* f = std::fopen((root + "/textures/sprites.qoi").c_str(), "wb");
    std::fwrite(qoi.data(), 1, qoi.size(), f);
    std::fclose(f);
    f = std::fopen((root + "/textures/sprites.atlas").c_str(), "w");
    for (size_t i = 1; i < atlas.regions.size(); i++) {
        const auto& r = atlas.regions[i];
        std::fprintf(f, "%s %d %d %d %d\n", r.name.c_str(), r.rect.x, r.rect.y, r.rect.w, r.rect.h);
    }
    std::fclose(f);

    write_wav(root + "/audio/jump.wav", render_sfx(0.18f, [](float t, float d) { return square(t * (300 + 900 * t / d)) * 0.5f * (1 - t / d); }), 2, 44100);
    write_wav(root + "/audio/coin.wav", render_sfx(0.25f, [](float t, float d) { return square(t * (t < 0.07f ? 988.0f : 1319.0f)) * 0.4f * (1 - t / d); }), 2, 44100);
    write_wav(root + "/audio/land.wav", render_sfx(0.12f, [](float t, float d) {
        float noise = float((uint32_t(t * 44100) * 2654435761u) >> 20 & 0xFFF) / 2048.0f - 1;
        return noise * 0.35f * (1 - t / d) + std::sin(t * 2 * 3.14159f * 90) * 0.4f * (1 - t / d); }), 2, 44100);
    write_wav(root + "/audio/bounce.wav", render_sfx(0.15f, [](float t, float d) { return std::sin(t * 2 * 3.14159f * (220 + 440 * (1 - t / d))) * 0.6f * (1 - t / d); }), 2, 44100);
    write_wav(root + "/audio/win.wav", render_sfx(0.9f, [](float t, float d) {
        static const float notes[] = {523.25f, 659.25f, 783.99f, 1046.5f};
        int i = std::min(3, int(t / 0.15f));
        return square(t * notes[i]) * 0.35f * (i == 3 ? 1 - (t - 0.45f) / (d - 0.45f) : 1.0f); }), 2, 44100);
    int ls, le;
    auto music = render_music(&ls, &le);
    write_wav(root + "/music/theme.wav", music, 2, 44100, ls, le);
    std::printf("assets written to %s (%zu sprites, atlas %dx%d, music %.1fs)\n", root.c_str(), sprites.size(), atlas_img.width, atlas_img.height, music.size() / 2 / 44100.0);
    return 0;
}
