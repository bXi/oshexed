#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace hx {
class Graphics;
}

// Serves images from the user's firmware (rec 8/9) to RmlUi: `<img src="fw:<generation>/<image index>">`.
// The engine's own LoadTexture reads no files, so a wrapper render interface (installed through
// RmlUI::SetRenderInterfaceHook, as lumistunts does for its thumbnails) decodes the image and hands the pixels
// to the backend's GenerateTexture. RmlUi owns and releases the textures.
namespace editor::fwtex {

// Once, before Window::InitWindow (a context binds to the render interface that exists when it's created).
void installHook();
// The firmware's graphics, or nullptr. Each call starts a new generation, so sources from an earlier firmware
// never pick up a cached texture of the same index.
void setGraphics(const hx::Graphics *gfx);
// "" when no graphics are loaded. frameless: crop off the 2 px frame ring block icons carry.
std::string source(std::uint32_t imageIndex, bool frameless = false);

// Our own pictures drawn in code (e.g. the EQ curve): premultiplied RGBA, width * height * 4 bytes. Returns the
// `<img src>` for it ("img:<key>/<n>", new each call so RmlUi loads the new pixels) and releases the texture of the
// key's previous image.
std::string putImage(const std::string &key, int width, int height, std::vector<std::uint8_t> rgba);

} // namespace editor::fwtex
