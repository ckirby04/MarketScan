#include "screenshot.hpp"

#include <algorithm>
#include <fstream>
#include <vector>

#include <GLFW/glfw3.h>

#include "marketscan/util/logging.hpp"

namespace marketscan::app {
namespace {

void put_u32(std::vector<unsigned char>& out, std::uint32_t value) {
    out.push_back(static_cast<unsigned char>(value & 0xFFU));
    out.push_back(static_cast<unsigned char>((value >> 8) & 0xFFU));
    out.push_back(static_cast<unsigned char>((value >> 16) & 0xFFU));
    out.push_back(static_cast<unsigned char>((value >> 24) & 0xFFU));
}

void put_u16(std::vector<unsigned char>& out, std::uint16_t value) {
    out.push_back(static_cast<unsigned char>(value & 0xFFU));
    out.push_back(static_cast<unsigned char>((value >> 8) & 0xFFU));
}

}  // namespace

bool capture_back_buffer(const std::filesystem::path& path,
                         std::int32_t width,
                         std::int32_t height) {
    if (width <= 0 || height <= 0) {
        log::error("screenshot", "refusing to capture a zero-sized framebuffer");
        return false;
    }

    const auto w = static_cast<std::size_t>(width);
    const auto h = static_cast<std::size_t>(height);

    // BMP rows are padded to four bytes and stored bottom-up, which is the same
    // order glReadPixels returns them in -- so the rows go straight out with no
    // flip.
    const std::size_t stride = ((w * 3U) + 3U) & ~std::size_t{3U};

    std::vector<unsigned char> pixels(w * h * 3U);
    glReadBuffer(GL_BACK);
    // Default is 4; the read is tightly packed and the padding is added below.
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    // RGB and swapped by hand rather than GL_BGR: that token is OpenGL 1.2 and
    // the header GLFW pulls in on Windows is 1.1, so asking for it is a build
    // error on the one platform this is built on.
    glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
    for (std::size_t i = 0; i + 2 < pixels.size(); i += 3U) {
        std::swap(pixels[i], pixels[i + 2U]);
    }

    const auto image_bytes = static_cast<std::uint32_t>(stride * h);
    constexpr std::uint32_t kHeaderBytes = 14U + 40U;

    std::vector<unsigned char> header;
    header.reserve(kHeaderBytes);
    header.push_back('B');
    header.push_back('M');
    put_u32(header, kHeaderBytes + image_bytes);
    put_u16(header, 0);
    put_u16(header, 0);
    put_u32(header, kHeaderBytes);

    put_u32(header, 40U);  // BITMAPINFOHEADER
    put_u32(header, static_cast<std::uint32_t>(width));
    put_u32(header, static_cast<std::uint32_t>(height));
    put_u16(header, 1);   // planes
    put_u16(header, 24);  // bits per pixel
    put_u32(header, 0);   // BI_RGB, uncompressed
    put_u32(header, image_bytes);
    put_u32(header, 2835U);  // 72 dpi, in pixels per metre
    put_u32(header, 2835U);
    put_u32(header, 0);  // palette entries
    put_u32(header, 0);  // important colours

    std::error_code code;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), code);
    }

    std::ofstream file(path, std::ios::binary);
    if (!file) {
        log::error("screenshot", "could not open " + path.string());
        return false;
    }
    file.write(reinterpret_cast<const char*>(header.data()),
               static_cast<std::streamsize>(header.size()));

    const std::vector<unsigned char> padding(stride - (w * 3U), 0);
    for (std::size_t row = 0; row < h; ++row) {
        file.write(reinterpret_cast<const char*>(pixels.data() + (row * w * 3U)),
                   static_cast<std::streamsize>(w * 3U));
        if (!padding.empty()) {
            file.write(reinterpret_cast<const char*>(padding.data()),
                       static_cast<std::streamsize>(padding.size()));
        }
    }
    if (!file) {
        log::error("screenshot", "could not write " + path.string());
        return false;
    }
    file.close();

    log::info("screenshot",
              "wrote " + path.string() + " (" + std::to_string(width) + "x" +
                  std::to_string(height) + ")");
    return true;
}

}  // namespace marketscan::app
