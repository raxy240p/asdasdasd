// module/render/icon_cache.hpp — on-demand PNG -> ID3D11ShaderResourceView.
// Reads are plain file opens — not EXECUTE opens, so the Trusted-Launch gate
// (Appendix A.4) does not touch them.
#pragma once
#include <d3d11.h>
#include <filesystem>
#include <string>
#include <unordered_map>

namespace cheat::render {

class IconCache {
public:
    explicit IconCache(std::filesystem::path dir);
    ~IconCache();

    // nullptr if the file is missing or malformed.
    ID3D11ShaderResourceView* Get(const std::string& key);

    void Bind(ID3D11Device* dev); // called once by DX11 bringup

private:
    ID3D11Device*                                          dev_ = nullptr;
    std::filesystem::path                                  dir_;
    std::unordered_map<std::string, ID3D11ShaderResourceView*> cache_;

    ID3D11ShaderResourceView* Load(const std::string& key);
};

} // namespace cheat::render
