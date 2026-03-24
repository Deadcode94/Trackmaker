#pragma once
#include <string>

// A clean abstraction. No Windows.h or Raylib.h included here!
namespace OSUtils {
    std::string OpenTrackTemplateDialog();
    std::string SaveTrackFileDialog();
    std::string OpenTrackFileDialog();
    std::string SaveObjFileDialog();
    std::string SaveJsonFileDialog();
    std::string OpenImageFileDialog();
}
