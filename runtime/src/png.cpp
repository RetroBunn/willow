#include "png.hpp"

#include <algorithm>
#include <cstdio>

namespace willow {

static uint32_t crc32(const uint8_t *data, size_t n, uint32_t crc = 0)
{
    static uint32_t table[256];
    if (!table[1]) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    }
    crc = ~crc;
    for (size_t i = 0; i < n; i++) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

static void put32(std::vector<uint8_t> &out, uint32_t v)
{
    for (int s = 24; s >= 0; s -= 8) out.push_back(uint8_t(v >> s));
}

static void chunk(std::vector<uint8_t> &out, const char *type, const std::vector<uint8_t> &body)
{
    put32(out, uint32_t(body.size()));
    size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), body.begin(), body.end());
    put32(out, crc32(out.data() + start, out.size() - start));
}

bool write_png(const std::filesystem::path &path, int width, int height, const std::vector<uint8_t> &rgb)
{
    std::vector<uint8_t> raw;  // each row is prefixed with filter type 0
    for (int y = 0; y < height; y++) {
        raw.push_back(0);
        raw.insert(raw.end(), rgb.begin() + size_t(y) * width * 3, rgb.begin() + size_t(y + 1) * width * 3);
    }
    std::vector<uint8_t> z = {0x78, 0x01};  // zlib header, then stored blocks of up to 65535 bytes
    for (size_t pos = 0; pos < raw.size() || pos == 0;) {
        size_t n = std::min<size_t>(65535, raw.size() - pos);
        bool last = pos + n == raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(uint8_t(n));
        z.push_back(uint8_t(n >> 8));
        z.push_back(uint8_t(~n));
        z.push_back(uint8_t(~n >> 8));
        z.insert(z.end(), raw.begin() + long(pos), raw.begin() + long(pos + n));
        pos += n;
        if (last) break;
    }
    uint32_t a = 1, b = 0;  // Adler-32 of the uncompressed data
    for (uint8_t c : raw) a = (a + c) % 65521, b = (b + a) % 65521;
    put32(z, b << 16 | a);

    std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    std::vector<uint8_t> ihdr;
    put32(ihdr, uint32_t(width));
    put32(ihdr, uint32_t(height));
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});  // 8-bit, RGB
    chunk(out, "IHDR", ihdr);
    chunk(out, "IDAT", z);
    chunk(out, "IEND", {});
    std::FILE *f = std::fopen(path.string().c_str(), "wb");
    if (!f) return false;
    bool ok = std::fwrite(out.data(), 1, out.size(), f) == out.size();
    std::fclose(f);
    return ok;
}

}  // namespace willow
