<<<<<<< HEAD
#include "os_utils.h"
#include <windows.h>
#include <commdlg.h>

namespace OSUtils {

    // Helper to get the absolute path to the project's assets folder
    std::string GetAbsoluteAssetPath(const std::string& subfolder) {
        char buffer[MAX_PATH];
        GetModuleFileNameA(NULL, buffer, MAX_PATH);
        std::string path(buffer);
        
        // Remove the executable name to get the current directory
        size_t lastSlash = path.find_last_of("\\/");
        if (lastSlash != std::string::npos) {
            path = path.substr(0, lastSlash);
        }

        // Save the executable directory to use as a failsafe fallback
        std::string exeDir = path;

        // Traverse up the directory tree (up to 5 levels) to find the "assets" folder
        for (int i = 0; i < 5; ++i) {
            std::string testPath = path + "\\assets";
            DWORD attrib = GetFileAttributesA(testPath.c_str());
            if (attrib != INVALID_FILE_ATTRIBUTES && (attrib & FILE_ATTRIBUTE_DIRECTORY)) {
                return testPath + "\\" + subfolder;
            }
            
            // Go up one level
            lastSlash = path.find_last_of("\\/");
            if (lastSlash != std::string::npos) {
                path = path.substr(0, lastSlash);
            } else {
                break;
            }
        }
        
        return exeDir; // Fallback to the executable's exact folder
    }

    std::string OpenImageFileDialog() {
        char filename[MAX_PATH] = { 0 };
        OPENFILENAMEA ofn;
        ZeroMemory(&ofn, sizeof(ofn));
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = NULL;
        // Filter specifically for common image formats supported by Raylib
        ofn.lpstrFilter = "Image Files\0*.png;*.jpg;*.bmp;*.tga\0All Files (*.*)\0*.*\0";
        ofn.lpstrFile = filename;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
        ofn.lpstrDefExt = "";
        
        std::string initDir = GetAbsoluteAssetPath("textures");
        ofn.lpstrInitialDir = initDir.c_str();

        if (GetOpenFileNameA(&ofn)) {
            return std::string(filename);
        }
        return "";
    }

    std::string OpenTrackTemplateDialog() {
        char filename[MAX_PATH] = { 0 };
        OPENFILENAMEA ofn;
        ZeroMemory(&ofn, sizeof(ofn));
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = NULL;
        ofn.lpstrFilter = "Track Templates (*.tracktemplate)\0*.tracktemplate\0All Files (*.*)\0*.*\0";
        ofn.lpstrFile = filename;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
        ofn.lpstrDefExt = "tracktemplate";
        
        std::string initDir = GetAbsoluteAssetPath("tracktemplates");
        ofn.lpstrInitialDir = initDir.c_str();

        if (GetOpenFileNameA(&ofn)) {
            return std::string(filename);
        }
        return "";
    }


    std::string SaveTrackFileDialog() {
        char filename[MAX_PATH] = { 0 };
        OPENFILENAMEA ofn;
        ZeroMemory(&ofn, sizeof(ofn));
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = NULL;
        ofn.lpstrFilter = "Track Files (*.track)\0*.track\0All Files (*.*)\0*.*\0";
        ofn.lpstrFile = filename;
        ofn.nMaxFile = MAX_PATH;
        // OFN_OVERWRITEPROMPT ensures Windows warns you before replacing a file
        ofn.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT;
        ofn.lpstrDefExt = "track";
        
        std::string initDir = GetAbsoluteAssetPath("tracks");
        ofn.lpstrInitialDir = initDir.c_str();

        if (GetSaveFileNameA(&ofn)) {
            return std::string(filename);
        }
        return "";
    }

    std::string OpenTrackFileDialog() {
        char filename[MAX_PATH] = { 0 };
        OPENFILENAMEA ofn;
        ZeroMemory(&ofn, sizeof(ofn));
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = NULL;
        // Filter specifically for your saved road layouts
        ofn.lpstrFilter = "Track Files (*.track)\0*.track\0All Files (*.*)\0*.*\0";
        ofn.lpstrFile = filename;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
        ofn.lpstrDefExt = "track";
        
        std::string initDir = GetAbsoluteAssetPath("tracks");
        ofn.lpstrInitialDir = initDir.c_str();

        if (GetOpenFileNameA(&ofn)) {
            return std::string(filename);
        }
        return "";
    }

    std::string SaveObjFileDialog() {
        char filename[MAX_PATH] = { 0 };
        OPENFILENAMEA ofn;
        ZeroMemory(&ofn, sizeof(ofn));
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = NULL;
        ofn.lpstrFilter = "OBJ Files (*.obj)\0*.obj\0All Files (*.*)\0*.*\0";
        ofn.lpstrFile = filename;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT;
        ofn.lpstrDefExt = "obj";

        std::string initDir = GetAbsoluteAssetPath("export");
        ofn.lpstrInitialDir = initDir.c_str();

        if (GetSaveFileNameA(&ofn)) {
            return std::string(filename);
        }
        return "";
    }

    std::string SaveJsonFileDialog() {
        char filename[MAX_PATH] = { 0 };
        OPENFILENAMEA ofn;
        ZeroMemory(&ofn, sizeof(ofn));
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = NULL;
        ofn.lpstrFilter = "JSON Files (*.json)\0*.json\0All Files (*.*)\0*.*\0";
        ofn.lpstrFile = filename;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT;
        ofn.lpstrDefExt = "json";

        std::string initDir = GetAbsoluteAssetPath("export");
        ofn.lpstrInitialDir = initDir.c_str();

        if (GetSaveFileNameA(&ofn)) {
            return std::string(filename);
        }
        return "";
    }

}
=======
#include "os_utils.h"
#include <windows.h>
#include <commdlg.h>

namespace OSUtils {

    // Helper to get the absolute path to the project's assets folder
    std::string GetAbsoluteAssetPath(const std::string& subfolder) {
        char buffer[MAX_PATH];
        GetModuleFileNameA(NULL, buffer, MAX_PATH);
        std::string path(buffer);
        
        // Remove the executable name to get the current directory
        size_t lastSlash = path.find_last_of("\\/");
        if (lastSlash != std::string::npos) {
            path = path.substr(0, lastSlash);
        }

        // Save the executable directory to use as a failsafe fallback
        std::string exeDir = path;

        // Traverse up the directory tree (up to 5 levels) to find the "assets" folder
        for (int i = 0; i < 5; ++i) {
            std::string testPath = path + "\\assets";
            DWORD attrib = GetFileAttributesA(testPath.c_str());
            if (attrib != INVALID_FILE_ATTRIBUTES && (attrib & FILE_ATTRIBUTE_DIRECTORY)) {
                return testPath + "\\" + subfolder;
            }
            
            // Go up one level
            lastSlash = path.find_last_of("\\/");
            if (lastSlash != std::string::npos) {
                path = path.substr(0, lastSlash);
            } else {
                break;
            }
        }
        
        return exeDir; // Fallback to the executable's exact folder
    }

    std::string OpenImageFileDialog() {
        char filename[MAX_PATH] = { 0 };
        OPENFILENAMEA ofn;
        ZeroMemory(&ofn, sizeof(ofn));
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = NULL;
        // Filter specifically for common image formats supported by Raylib
        ofn.lpstrFilter = "Image Files\0*.png;*.jpg;*.bmp;*.tga\0All Files (*.*)\0*.*\0";
        ofn.lpstrFile = filename;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
        ofn.lpstrDefExt = "";
        
        std::string initDir = GetAbsoluteAssetPath("textures");
        ofn.lpstrInitialDir = initDir.c_str();

        if (GetOpenFileNameA(&ofn)) {
            return std::string(filename);
        }
        return "";
    }

    std::string OpenTrackTemplateDialog() {
        char filename[MAX_PATH] = { 0 };
        OPENFILENAMEA ofn;
        ZeroMemory(&ofn, sizeof(ofn));
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = NULL;
        ofn.lpstrFilter = "Track Templates (*.tracktemplate)\0*.tracktemplate\0All Files (*.*)\0*.*\0";
        ofn.lpstrFile = filename;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
        ofn.lpstrDefExt = "tracktemplate";
        
        std::string initDir = GetAbsoluteAssetPath("tracktemplates");
        ofn.lpstrInitialDir = initDir.c_str();

        if (GetOpenFileNameA(&ofn)) {
            return std::string(filename);
        }
        return "";
    }


    std::string SaveTrackFileDialog() {
        char filename[MAX_PATH] = { 0 };
        OPENFILENAMEA ofn;
        ZeroMemory(&ofn, sizeof(ofn));
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = NULL;
        ofn.lpstrFilter = "Track Files (*.track)\0*.track\0All Files (*.*)\0*.*\0";
        ofn.lpstrFile = filename;
        ofn.nMaxFile = MAX_PATH;
        // OFN_OVERWRITEPROMPT ensures Windows warns you before replacing a file
        ofn.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT;
        ofn.lpstrDefExt = "track";
        
        std::string initDir = GetAbsoluteAssetPath("tracks");
        ofn.lpstrInitialDir = initDir.c_str();

        if (GetSaveFileNameA(&ofn)) {
            return std::string(filename);
        }
        return "";
    }

    std::string OpenTrackFileDialog() {
        char filename[MAX_PATH] = { 0 };
        OPENFILENAMEA ofn;
        ZeroMemory(&ofn, sizeof(ofn));
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = NULL;
        // Filter specifically for your saved road layouts
        ofn.lpstrFilter = "Track Files (*.track)\0*.track\0All Files (*.*)\0*.*\0";
        ofn.lpstrFile = filename;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
        ofn.lpstrDefExt = "track";
        
        std::string initDir = GetAbsoluteAssetPath("tracks");
        ofn.lpstrInitialDir = initDir.c_str();

        if (GetOpenFileNameA(&ofn)) {
            return std::string(filename);
        }
        return "";
    }

    std::string SaveObjFileDialog() {
        char filename[MAX_PATH] = { 0 };
        OPENFILENAMEA ofn;
        ZeroMemory(&ofn, sizeof(ofn));
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = NULL;
        ofn.lpstrFilter = "OBJ Files (*.obj)\0*.obj\0All Files (*.*)\0*.*\0";
        ofn.lpstrFile = filename;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT;
        ofn.lpstrDefExt = "obj";

        std::string initDir = GetAbsoluteAssetPath("export");
        ofn.lpstrInitialDir = initDir.c_str();

        if (GetSaveFileNameA(&ofn)) {
            return std::string(filename);
        }
        return "";
    }

    std::string SaveJsonFileDialog() {
        char filename[MAX_PATH] = { 0 };
        OPENFILENAMEA ofn;
        ZeroMemory(&ofn, sizeof(ofn));
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = NULL;
        ofn.lpstrFilter = "JSON Files (*.json)\0*.json\0All Files (*.*)\0*.*\0";
        ofn.lpstrFile = filename;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT;
        ofn.lpstrDefExt = "json";

        std::string initDir = GetAbsoluteAssetPath("export");
        ofn.lpstrInitialDir = initDir.c_str();

        if (GetSaveFileNameA(&ofn)) {
            return std::string(filename);
        }
        return "";
    }

}
>>>>>>> 2147e2d76ea80437e46a4f8ad037ef57b7cffbbc
