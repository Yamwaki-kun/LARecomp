#include "xcompress.h"

#include <rex/system/lzx.h>

namespace mc::modloader {

namespace {

// XMemCreateDecompressionContext in the game (sub_8244FF98) asks for a
// 128 KiB window; every chunk but the last decodes to one 32 KiB LZX frame.
constexpr uint32_t kWindowSize = 0x20000;
constexpr size_t kFrameOutput = 0x8000;

// Drops the chunk headers and leaves the bare LZX bitstream libmspack reads.
// Each chunk ends on a frame boundary, where the decoder realigns itself, so
// the payloads can simply be laid end to end. Returns false on a stream that
// is cut short or does not add up to `dst_len`.
bool StripChunkHeaders(const uint8_t* src, size_t src_len, size_t dst_len,
                       std::vector<uint8_t>& out) {
    out.clear();
    out.reserve(src_len);

    size_t offset = 0;
    size_t produced = 0;
    while (offset < src_len) {
        size_t output = kFrameOutput;
        size_t length = 0;
        if (src[offset] == 0xFF) {
            if (offset + 5 > src_len) return false;
            output = (size_t(src[offset + 1]) << 8) | src[offset + 2];
            length = (size_t(src[offset + 3]) << 8) | src[offset + 4];
            offset += 5;
        } else {
            if (offset + 2 > src_len) return false;
            length = (size_t(src[offset]) << 8) | src[offset + 1];
            offset += 2;
        }
        if (length == 0) break;
        if (offset + length > src_len) return false;

        out.insert(out.end(), src + offset, src + offset + length);
        offset += length;
        produced += output;
    }
    return !out.empty() && produced >= dst_len;
}

}  // namespace

bool LzxDecompress(const uint8_t* src, size_t src_len, std::vector<uint8_t>& dst,
                   size_t dst_len) {
    if (!src || !src_len || !dst_len) return false;

    std::vector<uint8_t> bitstream;
    if (!StripChunkHeaders(src, src_len, dst_len, bitstream)) return false;

    dst.assign(dst_len, 0);
    return lzx_decompress(bitstream.data(), bitstream.size(), dst.data(), dst_len, kWindowSize,
                          nullptr, 0) == 0;
}

}  // namespace mc::modloader
