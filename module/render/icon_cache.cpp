// module/render/icon_cache.cpp — stb_image into a DX11 texture.
#include "icon_cache.hpp"

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

namespace cheat::render {

IconCache::IconCache(std::filesystem::path dir) : dir_(std::move(dir)) {}

IconCache::~IconCache() {
    for (auto& [_, srv] : cache_) if (srv) srv->Release();
}

void IconCache::Bind(ID3D11Device* dev) { dev_ = dev; }

ID3D11ShaderResourceView* IconCache::Get(const std::string& key) {
    if (!dev_) return nullptr;
    auto it = cache_.find(key);
    if (it != cache_.end()) return it->second;
    auto* srv = Load(key);
    // Only cache a successful load. If the PNG is missing right now (icon
    // pack still staging) a later Get() will retry instead of returning a
    // permanent null.
    if (srv) cache_.emplace(key, srv);
    return srv;
}

ID3D11ShaderResourceView* IconCache::Load(const std::string& key) {
    auto p = (dir_ / (key + ".png")).string();
    int w = 0, h = 0, ch = 0;
    unsigned char* pixels = stbi_load(p.c_str(), &w, &h, &ch, 4);
    if (!pixels) return nullptr;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA sr{ pixels, static_cast<UINT>(w * 4), 0 };
    ID3D11Texture2D* tex = nullptr;
    HRESULT hr = dev_->CreateTexture2D(&td, &sr, &tex);
    stbi_image_free(pixels);
    if (FAILED(hr)) return nullptr;

    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = td.Format;
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    ID3D11ShaderResourceView* srv = nullptr;
    dev_->CreateShaderResourceView(tex, &sd, &srv);
    tex->Release();
    return srv;
}

} // namespace cheat::render
