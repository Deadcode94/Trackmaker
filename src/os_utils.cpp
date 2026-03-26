#include "os_utils.h"

#if defined(_WIN32)
// 1. Trick the Windows API (included by pfd) by renaming the functions
// so that they don't occupy the names used by Raylib.
#define Rectangle WinRectangle
#define CloseWindow WinCloseWindow
#define ShowCursor WinShowCursor
#endif

#include "portable-file-dialogs.h"

#if defined(_WIN32)
// 2. Restore the names to unlock them for Raylib
#undef Rectangle
#undef CloseWindow
#undef ShowCursor
// 3. Remove the actual Windows macros that overlap with Raylib
#undef LoadImage
#undef DrawText
#undef DrawTextEx
#undef PlaySound
#endif

#include "raylib.h"
#include <filesystem>

namespace OSUtils {

    // Helper to get the absolute path to the project's assets folder
    std::string GetAbsoluteAssetPath(const std::string& subfolder) {
        // GetApplicationDirectory() is provided by Raylib and is 100% cross-platform
        std::filesystem::path basePath(GetApplicationDirectory());
        
        for (int i = 0; i < 5; ++i) {
            std::filesystem::path testPath = basePath / "assets";
            if (std::filesystem::exists(testPath) && std::filesystem::is_directory(testPath)) {
                // ".make_preferred()" converts path separators to "\\" on Windows and "/" on Mac/Linux
                return (testPath / subfolder).make_preferred().string();
            }
            
            if (!basePath.has_parent_path()) break;
            basePath = basePath.parent_path();
        }
        
        // Fallback: returns the executable directory if it doesn't find the assets folder
        return GetApplicationDirectory();
    }

    std::string OpenImageFileDialog() {
        auto f = pfd::open_file("Open Image File", GetAbsoluteAssetPath("textures"),
                                { "Image Files", "*.png *.jpg *.bmp *.tga", "All Files", "*" });
        return f.result().empty() ? "" : f.result()[0];
    }

    std::string OpenTrackTemplateDialog() {
        auto f = pfd::open_file("Open Track Template", GetAbsoluteAssetPath("tracktemplates"),
                                { "Track Templates", "*.tracktemplate", "All Files", "*" });
        return f.result().empty() ? "" : f.result()[0];
    }


    std::string SaveTrackFileDialog() {
        auto f = pfd::save_file("Save Track File", GetAbsoluteAssetPath("tracks") + "/new_track.track",
                                { "Track Files", "*.track", "All Files", "*" });
        return f.result();
    }

    std::string OpenTrackFileDialog() {
        auto f = pfd::open_file("Open Track File", GetAbsoluteAssetPath("tracks"),
                                { "Track Files", "*.track", "All Files", "*" });
        return f.result().empty() ? "" : f.result()[0];
    }

    std::string SaveObjFileDialog() {
        auto f = pfd::save_file("Export OBJ", GetAbsoluteAssetPath("export") + "/export.obj",
                                { "OBJ Files", "*.obj", "All Files", "*" });
        return f.result();
    }

    std::string SaveJsonFileDialog() {
        auto f = pfd::save_file("Export AI Waypoints (JSON)", GetAbsoluteAssetPath("export") + "/waypoints.json",
                                { "JSON Files", "*.json", "All Files", "*" });
        return f.result();
    }

}
