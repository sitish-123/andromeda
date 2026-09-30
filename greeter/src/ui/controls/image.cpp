#include "ui/controls/image.h"
Image::Image() : Node(NodeType::Container) {}
void Image::setImagePath(const std::string&) {}
void Image::setFillMode(const std::string&) {}
LayoutSize Image::doMeasure(Renderer&, const LayoutConstraints& c) { return c.constrain({width(), height()}); }
void Image::doLayout(Renderer&) {}
