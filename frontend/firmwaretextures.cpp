#include "frontend/firmwaretextures.h"

#include "core/graphics.h"
#include "luminoveau.h"

#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/RenderInterface.h>

#include <charconv>
#include <map>
#include <string_view>
#include <memory>

namespace editor::fwtex {

namespace {

const hx::Graphics *g_gfx = nullptr;
unsigned g_generation = 0;

// putImage: pixels waiting for RmlUi to load them (by source), and each key's current source.
struct Pending {
    int width = 0, height = 0;
    std::vector<std::uint8_t> rgba;
};
std::map<std::string, Pending> g_pending;
// Per key: the newest source and the one before (an <img> may still show that one until the data model updates it).
std::map<std::string, std::pair<std::string, std::string>> g_current;
unsigned g_imageCounter = 0;

// "fw:<generation>/<index>" -> index, when it is ours and current. The colon matters: RmlUi joins any source
// without one (before the first slash) to the document's folder, so "fw/..." would arrive as "ui/fw/...".
// "fw:<generation>/<index>[/f]"; /f = frameless.
bool parse(const Rml::String &s, std::uint32_t &index, bool &frameless) {
    if (s.rfind("fw:", 0) != 0) return false;
    const char *p = s.data() + 3, *end = s.data() + s.size();
    unsigned gen = 0;
    auto r = std::from_chars(p, end, gen);
    if (r.ec != std::errc() || r.ptr == end || *r.ptr != '/' || gen != g_generation) return false;
    r = std::from_chars(r.ptr + 1, end, index);
    if (r.ec != std::errc()) return false;
    frameless = std::string_view(r.ptr, std::size_t(end - r.ptr)) == "/f";
    return r.ptr == end || frameless;
}

// Forwards everything to the engine's interface, except LoadTexture for our sources.
class FirmwareRenderInterface : public Rml::RenderInterface {
public:
    explicit FirmwareRenderInterface(Rml::RenderInterface *wrapped) : w_(wrapped) {}

    Rml::TextureHandle LoadTexture(Rml::Vector2i &dimensions, const Rml::String &source) override {
        if (const auto it = g_pending.find(source); it != g_pending.end()) {
            dimensions = Rml::Vector2i(it->second.width, it->second.height);
            const Rml::TextureHandle t =
                w_->GenerateTexture(Rml::Span<const Rml::byte>(it->second.rgba.data(), it->second.rgba.size()), dimensions);
            g_pending.erase(it);  // RmlUi keeps the texture by its source from here on
            return t;
        }
        std::uint32_t index = 0;
        bool frameless = false;
        if (g_gfx && parse(source, index, frameless))
            if (const hx::GfxImage *img = g_gfx->byIndex(index)) {
                const int crop = frameless ? 3 : 0;
                const std::vector<std::uint8_t> rgba = g_gfx->rgbaOnDark(*img, crop);
                if (rgba.empty()) return w_->LoadTexture(dimensions, source);
                dimensions = Rml::Vector2i(img->width - 2 * crop, img->height - 2 * crop);
                return w_->GenerateTexture(Rml::Span<const Rml::byte>(rgba.data(), rgba.size()), dimensions);
            }
        return w_->LoadTexture(dimensions, source);
    }

    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices,
                                                Rml::Span<const int> indices) override {
        return w_->CompileGeometry(vertices, indices);
    }
    void RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation,
                        Rml::TextureHandle texture) override {
        w_->RenderGeometry(geometry, translation, texture);
    }
    void ReleaseGeometry(Rml::CompiledGeometryHandle geometry) override { w_->ReleaseGeometry(geometry); }
    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i dimensions) override {
        return w_->GenerateTexture(source, dimensions);
    }
    void ReleaseTexture(Rml::TextureHandle texture) override { w_->ReleaseTexture(texture); }
    void EnableScissorRegion(bool enable) override { w_->EnableScissorRegion(enable); }
    void SetScissorRegion(Rml::Rectanglei region) override { w_->SetScissorRegion(region); }
    void EnableClipMask(bool enable) override { w_->EnableClipMask(enable); }
    void RenderToClipMask(Rml::ClipMaskOperation operation, Rml::CompiledGeometryHandle geometry,
                          Rml::Vector2f translation) override {
        w_->RenderToClipMask(operation, geometry, translation);
    }
    void SetTransform(const Rml::Matrix4f *transform) override { w_->SetTransform(transform); }
    Rml::LayerHandle PushLayer() override { return w_->PushLayer(); }
    void CompositeLayers(Rml::LayerHandle source, Rml::LayerHandle destination, Rml::BlendMode blend,
                         Rml::Span<const Rml::CompiledFilterHandle> filters) override {
        w_->CompositeLayers(source, destination, blend, filters);
    }
    void PopLayer() override { w_->PopLayer(); }
    Rml::TextureHandle SaveLayerAsTexture() override { return w_->SaveLayerAsTexture(); }
    Rml::CompiledFilterHandle SaveLayerAsMaskImage() override { return w_->SaveLayerAsMaskImage(); }
    Rml::CompiledFilterHandle CompileFilter(const Rml::String &name, const Rml::Dictionary &parameters) override {
        return w_->CompileFilter(name, parameters);
    }
    void ReleaseFilter(Rml::CompiledFilterHandle filter) override { w_->ReleaseFilter(filter); }
    Rml::CompiledShaderHandle CompileShader(const Rml::String &name, const Rml::Dictionary &parameters) override {
        return w_->CompileShader(name, parameters);
    }
    void RenderShader(Rml::CompiledShaderHandle shader, Rml::CompiledGeometryHandle geometry,
                      Rml::Vector2f translation, Rml::TextureHandle texture) override {
        w_->RenderShader(shader, geometry, translation, texture);
    }
    void ReleaseShader(Rml::CompiledShaderHandle shader) override { w_->ReleaseShader(shader); }

private:
    Rml::RenderInterface *w_;
};

// Outlives RmlUi: the context holds it for the whole session.
std::unique_ptr<FirmwareRenderInterface> g_interface;

} // namespace

void installHook() {
    RmlUI::SetRenderInterfaceHook([](Rml::RenderInterface *backend) -> Rml::RenderInterface * {
        if (!backend) return nullptr;
        g_interface = std::make_unique<FirmwareRenderInterface>(backend);
        return g_interface.get();
    });
}

void setGraphics(const hx::Graphics *gfx) {
    g_gfx = gfx;
    ++g_generation;
}

std::string source(std::uint32_t imageIndex, bool frameless) {
    if (!g_gfx) return {};
    return "fw:" + std::to_string(g_generation) + "/" + std::to_string(imageIndex) + (frameless ? "/f" : "");
}

std::string putImage(const std::string &key, int width, int height, std::vector<std::uint8_t> rgba) {
    if (width <= 0 || height <= 0 || rgba.size() != std::size_t(width) * std::size_t(height) * 4) return {};
    // The picture before the previous one: its texture (loaded) or its pixels (never shown) go.
    auto &[newest, older] = g_current[key];
    if (!older.empty()) {
        g_pending.erase(older);
        Rml::ReleaseTexture(older);
    }
    const std::string src = "img:" + key + "/" + std::to_string(++g_imageCounter);
    g_pending[src] = {width, height, std::move(rgba)};
    older = newest;
    newest = src;
    return src;
}

} // namespace editor::fwtex
