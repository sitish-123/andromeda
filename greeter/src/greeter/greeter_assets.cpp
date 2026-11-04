#include "greeter/greeter_assets.h"

#include <array>
#include <system_error>
#include <unistd.h>

namespace greeter {

  namespace {

    std::filesystem::path installedRoot() {
      const std::filesystem::path installed(NOCTALIA_GREETER_INSTALLED_ASSETS_DIR);
      if (installed.is_absolute()) {
        return installed;
      }
      std::array<char, 4096> buffer{};
      const ssize_t count = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
      if (count <= 0) {
        return installed;
      }
      return std::filesystem::path(std::string(buffer.data(), static_cast<std::size_t>(count))).parent_path()
                 .parent_path()
          / installed;
    }

    const std::filesystem::path& assetsRoot() {
      static const std::filesystem::path root = [] {
        std::error_code ec;
        const std::filesystem::path installed = installedRoot();
        if (std::filesystem::exists(installed / "fonts" / "Outfit-Black.ttf", ec)) {
          return installed;
        }
        return std::filesystem::path(NOCTALIA_GREETER_ASSETS_DIR);
      }();
      return root;
    }

  } // namespace

  std::filesystem::path greeterAssetPath(std::string_view relativePath) { return assetsRoot() / relativePath; }

} // namespace greeter
