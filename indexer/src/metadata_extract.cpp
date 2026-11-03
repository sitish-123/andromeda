#include "extract.h"

#include <exiv2/exiv2.hpp>
#include <taglib/fileref.h>
#include <taglib/tpropertymap.h>

#include <algorithm>
#include <array>
#include <string_view>

namespace andromeda::index {

  namespace {
    void append(std::string& text, std::string_view value) {
      if (value.empty() || text.size() + value.size() + 1 > kMaxText) {
        return;
      }
      text += text.empty() ? "" : "\n";
      text += value;
    }
  } // namespace

  std::optional<std::string> photoText(const std::string& path) {
    // Who, what and when; numeric shooting data (exposure, focal length) is left out as search noise.
    constexpr std::array<std::string_view, 7> kExif = {
        "Exif.Image.Make",   "Exif.Image.Model",        "Exif.Photo.LensModel",        "Exif.Photo.DateTimeOriginal",
        "Exif.Image.Artist", "Exif.Image.ImageDescription", "Exif.Photo.UserComment",
    };
    constexpr std::array<std::string_view, 4> kXmp = {"Xmp.dc.subject", "Xmp.dc.description", "Xmp.dc.title",
                                                      "Xmp.photoshop.City"};
    constexpr std::array<std::string_view, 4> kIptc = {"Iptc.Application2.Keywords", "Iptc.Application2.Caption",
                                                       "Iptc.Application2.City", "Iptc.Application2.CountryName"};
    try {
      auto image = Exiv2::ImageFactory::open(path);
      image->readMetadata();
      std::string text;
      for (const std::string_view key : kExif) {
        const auto it = image->exifData().findKey(Exiv2::ExifKey(std::string(key)));
        if (it != image->exifData().end()) {
          append(text, it->toString());
        }
      }
      for (const std::string_view key : kXmp) {
        const auto it = image->xmpData().findKey(Exiv2::XmpKey(std::string(key)));
        if (it != image->xmpData().end()) {
          append(text, it->toString());
        }
      }
      for (const auto& datum : image->iptcData()) {
        if (std::ranges::find(kIptc, datum.key()) != kIptc.end()) {
          append(text, datum.toString());
        }
      }
      return text.empty() ? std::nullopt : std::optional(std::move(text));
    } catch (...) {
      // A broken or unsupported file: no metadata.
      return std::nullopt;
    }
  }

  std::optional<std::string> audioText(const std::string& path) {
    constexpr std::array<const char*, 8> kTags = {"TITLE", "ARTIST", "ALBUM", "ALBUMARTIST",
                                                  "COMPOSER", "GENRE", "DATE", "COMMENT"};
    const TagLib::FileRef file(path.c_str(), false);
    if (file.isNull()) {
      return std::nullopt;
    }
    const TagLib::PropertyMap tags = file.properties();
    std::string text;
    for (const char* key : kTags) {
      if (const auto it = tags.find(key); it != tags.end()) {
        for (const TagLib::String& value : it->second) {
          append(text, value.to8Bit(true));
        }
      }
    }
    return text.empty() ? std::nullopt : std::optional(std::move(text));
  }

} // namespace andromeda::index
