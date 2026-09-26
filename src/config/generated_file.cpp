#include "config/generated_file.h"

#include <fstream>
#include <sstream>
#include <system_error>

namespace umbriel {

  std::string
  editGeneratedToml(std::string_view existing, std::string_view header, const std::function<void(toml::table&)>& edit) {
    toml::table root;
    if (!existing.empty()) {
      try {
        root = toml::parse(existing);
      } catch (const toml::parse_error&) {
        // A hand-damaged file is replaced rather than kept half-parsed.
      }
    }
    edit(root);
    std::ostringstream out;
    out << header << root << '\n';
    return std::move(out).str();
  }

  bool rewriteGeneratedToml(
      const std::filesystem::path& file, std::string_view header, const std::function<void(toml::table&)>& edit
  ) {
    std::string existing;
    if (std::ifstream in(file); in) {
      existing.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    const std::string document = editGeneratedToml(existing, header, edit);
    std::filesystem::path temporary = file;
    temporary += ".tmp";
    {
      std::ofstream out(temporary, std::ios::trunc);
      out << document;
      if (!out.flush()) {
        return false;
      }
    }
    std::error_code error;
    std::filesystem::rename(temporary, file, error);
    return !error;
  }

  toml::table& generatedTable(toml::table& root, std::initializer_list<std::string_view> path) {
    toml::table* table = &root;
    for (const std::string_view key : path) {
      if ((*table)[key].as_table() == nullptr) {
        table->insert_or_assign(key, toml::table{});
      }
      table = (*table)[key].as_table();
    }
    return *table;
  }

} // namespace umbriel
