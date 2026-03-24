# Trackmaker

Trackmaker is a powerful, procedural 3D track and road generation editor for Windows. 
It allows level designers and modders to create complex, sweeping 3D meshes along Bezier splines by extruding 2D cross-section templates.

The editor provides a modern UI, 3D manipulation gizmos, and seamless export to game-ready formats like `OBJ` for meshes and `JSON` for AI pathfinding.

**Type:** 3D Procedural Editor | **Platform:** Windows | **Language:** C++17

---

## Features

- **Bezier Spline Editing:** Place Anchor Points and adjust their tangents to create smooth, looping curves.
- **Template Extrusion:** Load `.tracktemplate` cross-sections (e.g., roads, tunnels, guardrails) and automatically extrude them along the spline.
- **3D Gizmo Controls:** Translate, rotate, and scale nodes in both Local and World space directly within the 3D viewport.
- **Advanced Track Tools:** 
  - **Subdivide:** Insert nodes exactly halfway along a curve without losing the original shape.
  - **Simplify:** Automatically remove redundant straight-line nodes to optimize performance.
- **Smart Snapping:** Grid snapping for position, rotation, and scale, plus magnetic ground snapping (Y=0).
- **Non-Destructive Workflow:** Full Undo/Redo history stack.
- **Game-Ready Export:** Export the final generated geometry to `.obj` and the curve data to `.json` for AI waypoints.

---

## Installation & Building

This project uses **CMake** to manage dependencies and build the executable. The main rendering backend is driven by **Raylib**.

### Prerequisites
- C++17 compatible compiler (MSVC, MinGW, or GCC/Clang).
- CMake (3.15 or higher).

### Build Steps

1. Clone the repository:
   ```bash
   git clone https://github.com/yourusername/Trackmaker.git
   cd Trackmaker
   ```

2. Create a build directory and generate the project files:
   ```bash
   mkdir build
   cd build
   cmake ..
   ```

3. Compile the executable:
   ```bash
   cmake --build . --config Release
   ```

4. Run the editor:
   ```bash
   ./Release/Trackmaker.exe
   ```
   *(Note: Ensure the `assets` folder is in the working directory or parent directories so the program can load templates and textures).*

---

## Controls & Usage

### Camera Controls
- **Orbit Camera:** Click and drag `Left Mouse Button` on the background.
- **Pan Camera:** Click and drag `Right Mouse Button` or `Middle Mouse Button`.
- **Zoom:** Use the `Mouse Scroll Wheel`.
- **ViewCube:** Use the 3D cube in the top-right corner to instantly snap the camera to orthographic axis views.

### Editor Shortcuts
- **W** - Translate mode
- **E** - Rotate mode
- **R** - Scale mode
- **Q** - Toggle Local / World space for Gizmo
- **Left Shift (Hold)** - Enable value snapping (translation, rotation, scale)
- **Ctrl + Z** - Undo last action
- **Ctrl + Y** - Redo last action
- **Ctrl + D** - Duplicate selected node(s)
- **Ctrl + Left Click** - Multi-select nodes
- **Double Left Click (List)** - Focus camera on the selected node

---

## File Formats

- **`.track`**: Native save file for Trackmaker. Stores spline node positions, bezier tangents, scales, and rotation banking.
- **`.tracktemplate`**: A proprietary text format defining the 2D cross-section profile of a track piece. It defines vertices, normals, UVs, and quad face indices.
- **`.obj`**: Standard 3D model format. Exported meshes are unindexed (flat) to bypass 16-bit vertex limits on massive tracks.
- **`.json`**: Exports the track curve as a dense array of sampled 3D coordinates and forward vectors. Ideal for writing AI driving paths or bot logic.

---

## Credits & Acknowledgments

This project is built upon the foundation of the original Mac + Windows Trackmaker engine and relies on several fantastic open-source libraries.

- **Original Trackmaker & Core Math Engine:** cochrane (Author of the original track generation algorithms found in `/src/shared/`).
- **Raylib:** A simple and easy-to-use library to enjoy videogames programming by Ramon Santamaria.
- **Dear ImGui:** Bloat-free Graphical User interface for C++ with minimal dependencies by Omar Cornut.
- **ImGuizmo:** Immediate mode 3D gizmo for scene manipulation by Cedric Guillemet.
- **rlImGui:** Raylib ImGui integration backend by the Raylib-Extras community.
- **Icons:** FontAwesome 6 (Included via rlImGui extras).

---

## License

Please refer to the `LICENSE` file in the root directory. Libraries used (Raylib, ImGui, etc.) retain their respective original licenses (zlib, MIT).