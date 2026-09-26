#include "system/xkb_layout_catalog.h"

#include "core/log.h"

#include <algorithm>
#include <libxml/parser.h>
#include <libxml/tree.h>

namespace xkb {

  namespace {

    constexpr Logger kLog("xkb-catalog");

    std::string localName(const xmlNode* node) {
      if (node == nullptr || node->name == nullptr) {
        return {};
      }
      return reinterpret_cast<const char*>(node->name);
    }

    std::string nodeText(xmlNode* node) {
      xmlChar* content = xmlNodeGetContent(node);
      if (content == nullptr) {
        return {};
      }
      std::string out(reinterpret_cast<const char*>(content));
      xmlFree(content);
      return out;
    }

    xmlNode* firstChildElement(xmlNode* node, std::string_view name) {
      if (node == nullptr) {
        return nullptr;
      }
      for (xmlNode* child = node->children; child != nullptr; child = child->next) {
        if (child->type == XML_ELEMENT_NODE && localName(child) == name) {
          return child;
        }
      }
      return nullptr;
    }

    std::vector<xmlNode*> childElements(xmlNode* node, std::string_view name) {
      std::vector<xmlNode*> out;
      if (node == nullptr) {
        return out;
      }
      for (xmlNode* child = node->children; child != nullptr; child = child->next) {
        if (child->type == XML_ELEMENT_NODE && localName(child) == name) {
          out.push_back(child);
        }
      }
      return out;
    }

    std::string configName(xmlNode* configItem) { return nodeText(firstChildElement(configItem, "name")); }
    std::string configDescription(xmlNode* configItem) {
      return nodeText(firstChildElement(configItem, "description"));
    }

    Layout parseLayout(xmlNode* layoutNode) {
      Layout layout;
      xmlNode* configItem = firstChildElement(layoutNode, "configItem");
      layout.name = configName(configItem);
      layout.description = configDescription(configItem);
      for (xmlNode* variantNode : childElements(firstChildElement(layoutNode, "variantList"), "variant")) {
        xmlNode* variantConfig = firstChildElement(variantNode, "configItem");
        layout.variants.push_back({.name = configName(variantConfig), .description = configDescription(variantConfig)});
      }
      return layout;
    }

    OptionGroup parseOptionGroup(xmlNode* groupNode) {
      OptionGroup group;
      xmlNode* configItem = firstChildElement(groupNode, "configItem");
      group.name = configName(configItem);
      group.description = configDescription(configItem);
      const xmlChar* allowMultiple = xmlGetProp(groupNode, reinterpret_cast<const xmlChar*>("allowMultipleSelection"));
      group.allowMultiple =
          allowMultiple != nullptr && xmlStrcmp(allowMultiple, reinterpret_cast<const xmlChar*>("true")) == 0;
      if (allowMultiple != nullptr) {
        xmlFree(const_cast<xmlChar*>(allowMultiple));
      }
      for (xmlNode* optionNode : childElements(groupNode, "option")) {
        xmlNode* optionConfig = firstChildElement(optionNode, "configItem");
        group.options.push_back({.name = configName(optionConfig), .description = configDescription(optionConfig)});
      }
      return group;
    }

  } // namespace

  Catalog loadCatalog(const std::string& path) {
    Catalog catalog;
    xmlDocPtr doc = xmlReadFile(path.c_str(), nullptr, XML_PARSE_NOBLANKS);
    if (doc == nullptr) {
      kLog.warn("could not read {}", path);
      return catalog;
    }
    xmlNode* root = xmlDocGetRootElement(doc);
    xmlNode* layoutList = firstChildElement(root, "layoutList");
    for (xmlNode* layoutNode : childElements(layoutList, "layout")) {
      catalog.layouts.push_back(parseLayout(layoutNode));
    }
    std::ranges::sort(catalog.layouts, {}, &Layout::description);

    xmlNode* optionList = firstChildElement(root, "optionList");
    for (xmlNode* groupNode : childElements(optionList, "group")) {
      catalog.optionGroups.push_back(parseOptionGroup(groupNode));
    }
    xmlFreeDoc(doc);
    return catalog;
  }

  const Layout* findLayout(const Catalog& catalog, std::string_view name) {
    const auto it = std::ranges::find(catalog.layouts, name, &Layout::name);
    return it != catalog.layouts.end() ? &*it : nullptr;
  }

  const OptionGroup* findOptionGroup(const Catalog& catalog, std::string_view name) {
    const auto it = std::ranges::find(catalog.optionGroups, name, &OptionGroup::name);
    return it != catalog.optionGroups.end() ? &*it : nullptr;
  }

} // namespace xkb
