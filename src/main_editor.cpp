#include "raylib.h"
#include "raymath.h"
#include "imgui.h"
#include "rlImGui.h"
#include "rlgl.h"
#include "ImGuizmo.h"
#include <fstream>
#include <sstream>
#include <cstdio>
#include <vector>
#include <string>
#include <exception>
#include <deque>
#include <algorithm>

// Clean OS utilities (No Windows.h here!)
#include "os_utils.h"

// Trackmaker core headers
#include "shared/Vec4.h"
#include "shared/AnchorPoint.h"
#include "shared/Track.h"
#include "shared/Template.h"
#include "shared/FileWriter.h"


// --- GLOBAL STATE ---
std::vector<AnchorPoint> trackPoints;
const size_t NO_SELECTION = static_cast<size_t>(-1);
size_t selectedPointIndex = NO_SELECTION;
std::vector<size_t> selectedPointIndices;
std::string currentTemplatePath = "";
bool needsMeshRebuild = false;
std::string lastMeshError = "";

// --- GIZMO STATE ---
ImGuizmo::OPERATION currentGizmoOperation = ImGuizmo::TRANSLATE;
ImGuizmo::MODE currentGizmoMode = ImGuizmo::LOCAL;

// --- SNAP STATE ---
bool useSnap = false;
float snapTranslation = 1.0f; // Meters
float snapRotation = 15.0f;   // Degrees
float snapScale = 0.5f;       // Multiplier
bool snapToGround = true;     // Magnetic ground snap

// --- EDITOR PERFORMANCE STATE ---
bool realtimeMeshRebuild = false; // Toggle real-time 3D mesh generation on drag

// --- UNDO/REDO STATE ---
struct EditorState {
    std::vector<AnchorPoint> points;
    size_t selectedIndex;
    std::vector<size_t> selectedIndices;
};
std::deque<EditorState> undoHistory;
int historyIndex = -1;
const size_t MAX_HISTORY_STATES = 100;

// The Raylib model that holds the solid track mesh
Model currentTrackModel = { 0 };

// --- APPEARANCE & TEXTURE STATE ---
Texture2D currentTrackTexture = { 0 };
bool showTexture = true;
float globalUvScale[2] = { 1.0f, 1.0f }; // U and V scaling multipliers
bool showSolidMesh = true;
bool showWireframe = true;
bool showTrackCurve = true;
bool showTrackNodes = true;


// --- UTILITY: BEZIER MATH FOR HYBRID VIEW ---
Vector3 EvaluateCubicBezier(Vector3 p0, Vector3 p1, Vector3 p2, Vector3 p3, float t) {
    float u = 1.0f - t;
    float tt = t * t;
    float uu = u * u;
    float uuu = uu * u;
    float ttt = tt * t;

    Vector3 p = Vector3Scale(p0, uuu);
    p = Vector3Add(p, Vector3Scale(p1, 3 * uu * t));
    p = Vector3Add(p, Vector3Scale(p2, 3 * u * tt));
    p = Vector3Add(p, Vector3Scale(p3, ttt));
    return p;
}


// --- UNDO/REDO LOGIC ---
bool AreStatesEqual(const EditorState& a, const EditorState& b) {
    if (a.points.size() != b.points.size()) return false;
    for (size_t i = 0; i < a.points.size(); ++i) {
        float pA[3], pB[3];
        a.points[i].getPosition(pA[0], pA[1], pA[2]);
        b.points[i].getPosition(pB[0], pB[1], pB[2]);
        
        if (pA[0] != pB[0] || pA[1] != pB[1] || pA[2] != pB[2]) return false;
        if (a.points[i].getDirection() != b.points[i].getDirection()) return false;
        if (a.points[i].getInclination() != b.points[i].getInclination()) return false;
        if (a.points[i].getBank() != b.points[i].getBank()) return false;
        if (a.points[i].getScaleWidth() != b.points[i].getScaleWidth()) return false;
        if (a.points[i].getScaleHeight() != b.points[i].getScaleHeight()) return false;
        if (a.points[i].getPreviousControlPointDistanceFactor() != b.points[i].getPreviousControlPointDistanceFactor()) return false;
        if (a.points[i].getNextControlPointDistanceFactor() != b.points[i].getNextControlPointDistanceFactor()) return false;
    }
    return true;
}

void RecordState() {
    EditorState newState = { trackPoints, selectedPointIndex, selectedPointIndices };

    // Prevent saving identical consecutive states (e.g., clicking the gizmo without dragging)
    if (historyIndex >= 0 && historyIndex < static_cast<int>(undoHistory.size())) {
        if (AreStatesEqual(undoHistory[historyIndex], newState)) {
            // Just update the selection index if it changed, without creating a new state
            undoHistory[historyIndex].selectedIndex = selectedPointIndex;
            undoHistory[historyIndex].selectedIndices = selectedPointIndices;
            return;
        }
    }

    // If we have undone actions and make a new modification, clear the "future" (Redo) states
    if (historyIndex >= 0 && historyIndex < static_cast<int>(undoHistory.size()) - 1) {
        undoHistory.erase(undoHistory.begin() + historyIndex + 1, undoHistory.end());
    }

    undoHistory.push_back(newState);

    // Enforce the states limit
    if (undoHistory.size() > MAX_HISTORY_STATES) {
        undoHistory.pop_front();
    } else {
        historyIndex++;
    }
}

void Undo() {
    if (historyIndex > 0) {
        historyIndex--;
        trackPoints = undoHistory[historyIndex].points;
        selectedPointIndex = undoHistory[historyIndex].selectedIndex;
        selectedPointIndices = undoHistory[historyIndex].selectedIndices;
        needsMeshRebuild = true;
        TraceLog(LOG_INFO, "Undo performed. Reverted to state %d", historyIndex);
    }
}

void Redo() {
    if (historyIndex < static_cast<int>(undoHistory.size()) - 1) {
        historyIndex++;
        trackPoints = undoHistory[historyIndex].points;
        selectedPointIndex = undoHistory[historyIndex].selectedIndex;
        selectedPointIndices = undoHistory[historyIndex].selectedIndices;
        needsMeshRebuild = true;
        TraceLog(LOG_INFO, "Redo performed. Advanced to state %d", historyIndex);
    }
}


// --- MESH GENERATION (The Core Engine Link) ---
void RebuildTrackMesh() {
    needsMeshRebuild = false;

    if (trackPoints.size() < 2) return;
    if (currentTemplatePath.empty()) return;

    TraceLog(LOG_INFO, "Rebuilding solid 3D mesh in memory...");

    try {
        lastMeshError = ""; // Clear previous error

        // 1. Initialize the Template using the exact C-string signature
        Template trackTemplate(currentTemplatePath.c_str());

        // 2. Initialize the Track by passing the template reference
        Track track(trackTemplate);

        // 3. DIRECT MEMORY INJECTION: Copy UI points into the Track engine
        for (const AnchorPoint& uiPt : trackPoints) {

            // Create a new blank point inside the engine's memory vector
            AnchorPoint& enginePt = track.createAnchorPointAtEnd();

            // Extract values from our UI point
            float px, py, pz;
            uiPt.getPosition(px, py, pz);

            // Apply them directly to the newly created engine point
            enginePt.setPosition(px, py, pz);
            enginePt.setDirection(uiPt.getDirection());
            enginePt.setInclination(uiPt.getInclination());
            enginePt.setBank(uiPt.getBank());
            enginePt.setScaleWidth(uiPt.getScaleWidth());
            enginePt.setScaleHeight(uiPt.getScaleHeight());

            // NEW: Synchronize the Bezier tangent lengths to perfectly match the UI!
            enginePt.setNextControlPointDistanceFactor(uiPt.getNextControlPointDistanceFactor());
            enginePt.setPreviousControlPointDistanceFactor(uiPt.getPreviousControlPointDistanceFactor());
        }

        // --- ZERO DISK I/O: DIRECT MESH GENERATION ---
        std::vector<Vertex> trackVertices;
        std::vector<unsigned> trackIndices;
        track.fillBuffers(trackVertices, trackIndices);
        Mesh mesh = { 0 };
        
        if (trackVertices.empty() || trackIndices.empty()) {
            throw std::runtime_error("Track generator returned 0 vertices or faces. The .tracktemplate format might be invalid (e.g. expecting Quads 'q' instead of 'f', or unrecognized tokens).");
        }

        // We unroll the indexed mesh into a flat unindexed mesh to completely bypass 
        // the 16-bit unsigned short limit of Raylib's indices array for very large tracks.
        mesh.vertexCount = static_cast<int>(trackIndices.size());
        mesh.triangleCount = static_cast<int>(trackIndices.size() / 3);
        
        mesh.vertices = (float *)MemAlloc(mesh.vertexCount * 3 * sizeof(float));
        mesh.normals = (float *)MemAlloc(mesh.vertexCount * 3 * sizeof(float));
        mesh.texcoords = (float *)MemAlloc(mesh.vertexCount * 2 * sizeof(float));
        mesh.indices = nullptr; // Leave unindexed
        
        for (size_t i = 0; i < trackIndices.size(); i++) {
            unsigned int idx = trackIndices[i];
            
            mesh.vertices[i*3 + 0] = trackVertices[idx].position[0];
            mesh.vertices[i*3 + 1] = trackVertices[idx].position[1];
            mesh.vertices[i*3 + 2] = trackVertices[idx].position[2];

            mesh.normals[i*3 + 0] = trackVertices[idx].normal[0];
            mesh.normals[i*3 + 1] = trackVertices[idx].normal[1];
            mesh.normals[i*3 + 2] = trackVertices[idx].normal[2];

            mesh.texcoords[i*2 + 0] = trackVertices[idx].texCoord[0] * globalUvScale[0];
            
            // Raylib's internal LoadOBJ previously flipped the V coordinate automatically. 
            // We replicate that flip here (1.0f - V) to keep textures rendering correctly.
            mesh.texcoords[i*2 + 1] = (1.0f - trackVertices[idx].texCoord[1]) * globalUvScale[1];
        }
        
        if (currentTrackModel.meshCount > 0) {
            UnloadModel(currentTrackModel);
        }

        UploadMesh(&mesh, false); // Sends the raw arrays directly to VRAM
        currentTrackModel = LoadModelFromMesh(mesh);
        
        if (currentTrackModel.meshCount > 0) {
            if (currentTrackTexture.id != 0 && showTexture) {
                currentTrackModel.materials[0].maps[MATERIAL_MAP_DIFFUSE].texture = currentTrackTexture;
                currentTrackModel.materials[0].maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
            } else {
                currentTrackModel.materials[0].maps[MATERIAL_MAP_DIFFUSE].color = LIGHTGRAY;
            }
        }

        TraceLog(LOG_INFO, "In-memory mesh rebuild successful!");
    }
    catch (const std::exception& e) {
        lastMeshError = e.what();
        TraceLog(LOG_ERROR, "Failed to rebuild mesh: %s", e.what());
    }
}


// --- FILE SAVING LOGIC ---
void SaveTrackToFile(const std::string& path) {
    std::ofstream trackFile(path);
    if (!trackFile.is_open()) {
        TraceLog(LOG_ERROR, "Failed to open file for saving: %s", path.c_str());
        return;
    }

    for (const AnchorPoint& pt : trackPoints) {
        float px, py, pz;
        pt.getPosition(px, py, pz);

        // Write exactly as the original Mac version expects:
        // p X Y Z   Dir(Deg) Inc(Deg) Bank(Deg)   ScaleW ScaleH
        trackFile << "p " << px << " " << py << " " << pz << " "
            << (pt.getDirection() * RAD2DEG) << " "
            << (pt.getInclination() * RAD2DEG) << " "
            << (pt.getBank() * RAD2DEG) << " "
            << pt.getScaleWidth() << " " << pt.getScaleHeight() << "\n";
    }
    trackFile.close();
    TraceLog(LOG_INFO, "Track successfully saved to: %s", path.c_str());
}


// --- FILE LOADING LOGIC ---
void LoadTrackFromFile(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        TraceLog(LOG_ERROR, "Failed to open track file for reading: %s", path.c_str());
        return;
    }

    std::vector<AnchorPoint> loadedPoints;
    std::string line;

    // Parse the file line by line
    while (std::getline(file, line)) {
        if (line.empty()) continue;

        std::istringstream iss(line);
        char type;
        iss >> type;

        if (type == 'p') {
            AnchorPoint newPoint;
            float px, py, pz, dirDeg, incDeg, bankDeg, sw, sh;

            // Read all 8 values from the exact same line
            if (iss >> px >> py >> pz >> dirDeg >> incDeg >> bankDeg >> sw >> sh) {

                newPoint.setPosition(px, py, pz);

                // Convert the degrees from the text file into radians for the C++ engine
                newPoint.setDirection(dirDeg * DEG2RAD);
                newPoint.setInclination(incDeg * DEG2RAD);
                newPoint.setBank(bankDeg * DEG2RAD);

                newPoint.setScaleWidth(sw);
                newPoint.setScaleHeight(sh);

                // NEW: Force the legacy Mac engine default (0.5) for missing tangent data
                newPoint.setNextControlPointDistanceFactor(0.5f);
                newPoint.setPreviousControlPointDistanceFactor(0.5f);

                loadedPoints.push_back(newPoint);
            }
        }
    }
    file.close();

    // If we successfully loaded points, overwrite the current editor state
    if (!loadedPoints.empty()) {
        trackPoints = loadedPoints;
        selectedPointIndex = 0;
        selectedPointIndices.clear();
        selectedPointIndices.push_back(0);
        needsMeshRebuild = true;
        
        undoHistory.clear();
        historyIndex = -1;
        RecordState();
        
        TraceLog(LOG_INFO, "Track successfully loaded: %s", path.c_str());
    }
}


// --- OBJ EXPORT LOGIC ---
void ExportTrackToOBJ(const std::string& path) {
    if (trackPoints.size() < 2 || currentTemplatePath.empty()) {
        TraceLog(LOG_WARNING, "Cannot export: Track needs at least 2 points and a valid template.");
        return;
    }

    try {
        // Rebuild the engine track cleanly for export
        Template trackTemplate(currentTemplatePath.c_str());
        Track track(trackTemplate);

        for (const AnchorPoint& uiPt : trackPoints) {
            AnchorPoint& enginePt = track.createAnchorPointAtEnd();
            
            float px, py, pz;
            uiPt.getPosition(px, py, pz);
            
            enginePt.setPosition(px, py, pz);
            enginePt.setDirection(uiPt.getDirection());
            enginePt.setInclination(uiPt.getInclination());
            enginePt.setBank(uiPt.getBank());
            enginePt.setScaleWidth(uiPt.getScaleWidth());
            enginePt.setScaleHeight(uiPt.getScaleHeight());
            enginePt.setNextControlPointDistanceFactor(uiPt.getNextControlPointDistanceFactor());
            enginePt.setPreviousControlPointDistanceFactor(uiPt.getPreviousControlPointDistanceFactor());
        }

        // Export directly to the user's chosen path
        FileWriter writer(track);
        writer.write(OBJ, path.c_str());
        
        TraceLog(LOG_INFO, "OBJ Export successful: %s", path.c_str());

    } catch (const std::exception& e) {
        TraceLog(LOG_ERROR, "Failed to export OBJ: %s", e.what());
    }
}


// --- AI WAYPOINT EXPORT LOGIC ---
void ExportTrackToJSON(const std::string& path) {
    if (trackPoints.size() < 2) {
        TraceLog(LOG_WARNING, "Cannot export JSON: Track needs at least 2 points.");
        return;
    }

    std::ofstream outFile(path);
    if (!outFile.is_open()) {
        TraceLog(LOG_ERROR, "Failed to open file for saving JSON: %s", path.c_str());
        return;
    }

    outFile << "{\n";
    outFile << "  \"waypoints\": [\n";

    bool first = true;
    int waypointIndex = 0;
    const int steps = 20; // Resolution: 20 dense waypoints per Anchor Segment

    for (size_t i = 0; i < trackPoints.size() - 1; i++) {
        AnchorPoint& pt = trackPoints[i];
        AnchorPoint& nextPt = trackPoints[i + 1];

        float px, py, pz; pt.getPosition(px, py, pz);
        Vector3 pos = { px, py, pz };

        float nx, ny, nz; nextPt.getPosition(nx, ny, nz);
        Vector3 nextPos = { nx, ny, nz };

        matrix m1 = pt.getMatrix();
        Vector3 forward = { m1.x.x, m1.x.y, m1.x.z };
        
        matrix m2 = nextPt.getMatrix();
        Vector3 backward = { -m2.x.x, -m2.x.y, -m2.x.z };

        float length = Vector3Distance(pos, nextPos);

        Vector3 p1 = Vector3Add(pos, Vector3Scale(forward, length * pt.getNextControlPointDistanceFactor()));
        Vector3 p2 = Vector3Add(nextPos, Vector3Scale(backward, length * nextPt.getPreviousControlPointDistanceFactor()));

        // We don't include the last point of the segment (step == steps) to avoid duplicates with the next segment
        for (int step = 0; step < steps; step++) {
            float t = (float)step / (float)steps;
            
            // Evaluate slightly ahead to calculate the exact true forward vector
            float tNext = (float)(step + 1) / (float)steps;

            Vector3 currentCurvePt = EvaluateCubicBezier(pos, p1, p2, nextPos, t);
            Vector3 nextCurvePt = EvaluateCubicBezier(pos, p1, p2, nextPos, tNext);
            
            Vector3 dir = Vector3Normalize(Vector3Subtract(nextCurvePt, currentCurvePt));

            if (!first) outFile << ",\n";
            outFile << "    {\n";
            outFile << "      \"id\": " << waypointIndex++ << ",\n";
            outFile << "      \"position\": { \"x\": " << currentCurvePt.x << ", \"y\": " << currentCurvePt.y << ", \"z\": " << currentCurvePt.z << " },\n";
            outFile << "      \"forward\": { \"x\": " << dir.x << ", \"y\": " << dir.y << ", \"z\": " << dir.z << " }\n";
            outFile << "    }";
            first = false;
        }
    }

    // Add the very last Anchor Point to close the file
    AnchorPoint& lastPt = trackPoints.back();
    float lx, ly, lz; lastPt.getPosition(lx, ly, lz);
    matrix m_last = lastPt.getMatrix();
    
    if (!first) outFile << ",\n";
    outFile << "    {\n";
    outFile << "      \"id\": " << waypointIndex++ << ",\n";
    outFile << "      \"position\": { \"x\": " << lx << ", \"y\": " << ly << ", \"z\": " << lz << " },\n";
    outFile << "      \"forward\": { \"x\": " << m_last.x.x << ", \"y\": " << m_last.x.y << ", \"z\": " << m_last.x.z << " }\n";
    outFile << "    }\n";

    outFile << "  ]\n";
    outFile << "}\n";
    
    outFile.close();
    TraceLog(LOG_INFO, "AI Waypoints (JSON) exported successfully to: %s", path.c_str());
}


// --- FEATURE: SUBDIVIDE SPLINE ---
// Inserts a new node exactly halfway along the curve between two selected nodes
void SubdivideSegment(size_t segmentStartIndex) {
    if (segmentStartIndex >= trackPoints.size() - 1) return;

    AnchorPoint& p0 = trackPoints[segmentStartIndex];
    AnchorPoint& p1 = trackPoints[segmentStartIndex + 1];

    // Get positions
    float pos0[3], pos1[3];
    p0.getPosition(pos0[0], pos0[1], pos0[2]);
    p1.getPosition(pos1[0], pos1[1], pos1[2]);
    Vector3 v0 = { pos0[0], pos0[1], pos0[2] };
    Vector3 v1 = { pos1[0], pos1[1], pos1[2] };

    // Calculate forward vectors
    matrix m0 = p0.getMatrix();
    matrix m1 = p1.getMatrix();
    Vector3 fwd0 = { m0.x.x, m0.x.y, m0.x.z };
    Vector3 bwd1 = { -m1.x.x, -m1.x.y, -m1.x.z };

    // Find control points based on distance factors
    float length = Vector3Distance(v0, v1);
    Vector3 ctrl1 = Vector3Add(v0, Vector3Scale(fwd0, length * p0.getNextControlPointDistanceFactor()));
    Vector3 ctrl2 = Vector3Add(v1, Vector3Scale(bwd1, length * p1.getPreviousControlPointDistanceFactor()));

    // De Casteljau's algorithm at t = 0.5
    Vector3 P0 = v0;
    Vector3 P1 = ctrl1;
    Vector3 P2 = ctrl2;
    Vector3 P3 = v1;

    Vector3 P01 = Vector3Lerp(P0, P1, 0.5f);
    Vector3 P12 = Vector3Lerp(P1, P2, 0.5f);
    Vector3 P23 = Vector3Lerp(P2, P3, 0.5f);

    Vector3 P012 = Vector3Lerp(P01, P12, 0.5f);
    Vector3 P123 = Vector3Lerp(P12, P23, 0.5f);

    Vector3 P0123 = Vector3Lerp(P012, P123, 0.5f); // Exactly the midpoint on the curve
    Vector3 midPos = P0123;

    // Create the new intermediate point
    AnchorPoint midPt;
    midPt.setPosition(midPos.x, midPos.y, midPos.z);
    
    // Linearly interpolate other properties (banking, scale, etc.)
    midPt.setBank((p0.getBank() + p1.getBank()) * 0.5f);
    midPt.setScaleWidth((p0.getScaleWidth() + p1.getScaleWidth()) * 0.5f);
    midPt.setScaleHeight((p0.getScaleHeight() + p1.getScaleHeight()) * 0.5f);
    
    // Evaluate exact tangent at t = 0.5 to set the new rotation
    Vector3 tangent = Vector3Subtract(P123, P012);
    Vector3 forwardDir = Vector3Normalize(tangent);
    
    float newDir = atan2f(-forwardDir.z, forwardDir.x);
    float yClamp = forwardDir.y;
    if (yClamp > 1.0f) yClamp = 1.0f;
    if (yClamp < -1.0f) yClamp = -1.0f;
    float newInc = asinf(yClamp);
    midPt.setDirection(newDir);
    midPt.setInclination(newInc);

    // Calculate exact Bezier tangent factors for the new subdivision to preserve the curve shape
    float L_left = Vector3Distance(P0, midPos);
    float L_right = Vector3Distance(midPos, P3);
    float tangent_len = Vector3Distance(P012, P123);

    if (L_left > 0.001f) {
        p0.setNextControlPointDistanceFactor((length / (2.0f * L_left)) * p0.getNextControlPointDistanceFactor());
        midPt.setPreviousControlPointDistanceFactor(tangent_len / (2.0f * L_left));
    } else {
        p0.setNextControlPointDistanceFactor(0.5f);
        midPt.setPreviousControlPointDistanceFactor(0.5f);
    }

    if (L_right > 0.001f) {
        midPt.setNextControlPointDistanceFactor(tangent_len / (2.0f * L_right));
        p1.setPreviousControlPointDistanceFactor((length / (2.0f * L_right)) * p1.getPreviousControlPointDistanceFactor());
    } else {
        midPt.setNextControlPointDistanceFactor(0.5f);
        p1.setPreviousControlPointDistanceFactor(0.5f);
    }

    // Insert into vector
    trackPoints.insert(trackPoints.begin() + segmentStartIndex + 1, midPt);
    
    TraceLog(LOG_INFO, "Spline subdivided at index %zu", segmentStartIndex);
    
    needsMeshRebuild = true;
    RecordState();
}

// --- FEATURE: SIMPLIFY TRACK ---
// Removes redundant nodes that form a perfectly straight line with no property variations
void SimplifyTrack() {
    if (trackPoints.size() < 3) return;

    size_t initialSize = trackPoints.size();
    std::vector<size_t> nodesToRemove;

    // We skip the first and last nodes to preserve the end caps
    for (size_t i = 1; i < trackPoints.size() - 1; i++) {
        AnchorPoint& pPrev = trackPoints[i - 1];
        AnchorPoint& pCurr = trackPoints[i];
        AnchorPoint& pNext = trackPoints[i + 1];

        float posPrev[3], posCurr[3], posNext[3];
        pPrev.getPosition(posPrev[0], posPrev[1], posPrev[2]);
        pCurr.getPosition(posCurr[0], posCurr[1], posCurr[2]);
        pNext.getPosition(posNext[0], posNext[1], posNext[2]);

        Vector3 vPrev = { posPrev[0], posPrev[1], posPrev[2] };
        Vector3 vCurr = { posCurr[0], posCurr[1], posCurr[2] };
        Vector3 vNext = { posNext[0], posNext[1], posNext[2] };

        // Calculate direction vectors between the three nodes
        Vector3 dir1 = Vector3Normalize(Vector3Subtract(vCurr, vPrev));
        Vector3 dir2 = Vector3Normalize(Vector3Subtract(vNext, vCurr));

        // Check collinearity (1.0 means perfectly straight)
        float dot = Vector3DotProduct(dir1, dir2);

        // Ensure all visual properties are practically identical
        bool propertiesMatch = (fabsf(pPrev.getBank() - pCurr.getBank()) < 0.01f) &&
                               (fabsf(pCurr.getBank() - pNext.getBank()) < 0.01f) &&
                               (fabsf(pPrev.getScaleWidth() - pCurr.getScaleWidth()) < 0.01f) &&
                               (fabsf(pCurr.getScaleWidth() - pNext.getScaleWidth()) < 0.01f) &&
                               (fabsf(pPrev.getScaleHeight() - pCurr.getScaleHeight()) < 0.01f) &&
                               (fabsf(pCurr.getScaleHeight() - pNext.getScaleHeight()) < 0.01f);

        // If the path is straight and properties didn't change, the node is redundant
        if (dot > 0.999f && propertiesMatch) {
            nodesToRemove.push_back(i);
        }
    }

    // Erase from back to front to avoid invalidating indices
    for (auto it = nodesToRemove.rbegin(); it != nodesToRemove.rend(); ++it) {
        trackPoints.erase(trackPoints.begin() + *it);
    }

    if (!nodesToRemove.empty()) {
        selectedPointIndices.clear();
        selectedPointIndex = NO_SELECTION;
        needsMeshRebuild = true;
        RecordState();
        TraceLog(LOG_INFO, "Track simplified. Removed %zu redundant nodes.", initialSize - trackPoints.size());
    } else {
        TraceLog(LOG_INFO, "Track simplified. No redundant nodes found.");
    }
}


// --- GUI RENDERING ---
void RenderTrackmakerGUI(Camera3D& camera) {
    // Initialize default point if empty
    if (trackPoints.empty()) {
        AnchorPoint initialPoint;
        initialPoint.setPosition(0.0f, 0.0f, 0.0f);

        // NEW: Ensure brand new tracks also use the Mac engine default
        initialPoint.setNextControlPointDistanceFactor(0.5f);
        initialPoint.setPreviousControlPointDistanceFactor(0.5f);

        trackPoints.push_back(initialPoint);
        selectedPointIndex = 0;
        selectedPointIndices.clear();
        selectedPointIndices.push_back(0);
        
        undoHistory.clear();
        historyIndex = -1;
        RecordState();
    }

    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(400, 720), ImGuiCond_FirstUseEver);

    ImGui::Begin("Trackmaker Inspector", nullptr, ImGuiWindowFlags_MenuBar);

    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("New Track")) {
                trackPoints.clear();
                if (currentTrackModel.meshCount > 0) UnloadModel(currentTrackModel);
                currentTrackModel = { 0 };
                
                undoHistory.clear();
                historyIndex = -1;
            }

            // NEW: The Open Button
            if (ImGui::MenuItem("Open Track...")) {
                std::string openPath = OSUtils::OpenTrackFileDialog();
                if (!openPath.empty()) {
                    LoadTrackFromFile(openPath);
                }
            }

            // NEW: The Save As Button
            if (ImGui::MenuItem("Save Track As...")) {
                std::string savePath = OSUtils::SaveTrackFileDialog();
                if (!savePath.empty()) {
                    SaveTrackToFile(savePath);
                }
            }

            ImGui::Separator();
            
            // NEW: The Final Export Button
            if (ImGui::MenuItem("Export to OBJ...")) {
                std::string exportPath = OSUtils::SaveObjFileDialog();
                if (!exportPath.empty()) {
                    ExportTrackToOBJ(exportPath);
                }
            }

            // NEW: Export AI Waypoints JSON
            if (ImGui::MenuItem("Export AI Waypoints (JSON)...")) {
                std::string exportPath = OSUtils::SaveJsonFileDialog();
                if (!exportPath.empty()) {
                    ExportTrackToJSON(exportPath);
                }
            }

            ImGui::Separator();
            if (ImGui::MenuItem("Exit")) { CloseWindow(); }
            ImGui::EndMenu();
        }
        
        if (ImGui::BeginMenu("Edit")) {
            if (ImGui::MenuItem("Undo", "Ctrl+Z", false, historyIndex > 0)) {
                Undo();
            }
            if (ImGui::MenuItem("Redo", "Ctrl+Y", false, historyIndex < static_cast<int>(undoHistory.size()) - 1)) {
                Redo();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Simplify Track")) {
                SimplifyTrack();
            }
            ImGui::EndMenu();
        }
        
        if (ImGui::BeginMenu("Options")) {
            ImGui::MenuItem("Real-time 3D Mesh Updates", nullptr, &realtimeMeshRebuild);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Rebuilds the solid 3D mesh continuously while dragging.\nMay cause lag on large tracks.");
            ImGui::EndMenu();
        }
        
        ImGui::EndMenuBar();
    }

    // Template Loading Section
    ImGui::Text("Template File:");
    ImGui::TextDisabled("%s", currentTemplatePath.empty() ? "None loaded" : currentTemplatePath.c_str());
    if (ImGui::Button("Load .tracktemplate...", ImVec2(-FLT_MIN, 0))) {
        std::string path = OSUtils::OpenTrackTemplateDialog();
        if (!path.empty()) {
            currentTemplatePath = path;
            needsMeshRebuild = true;
        }
    }

    if (!lastMeshError.empty()) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
        ImGui::TextWrapped("Template Error:\n%s", lastMeshError.c_str());
        ImGui::PopStyleColor();
    }

    ImGui::Separator();

    // Anchor Points List
    ImGui::Text("Anchor Points");
    if (ImGui::BeginListBox("##PointsList", ImVec2(-FLT_MIN, 150))) {
        for (size_t i = 0; i < trackPoints.size(); i++) {
            std::string label = "Point " + std::to_string(i);
            bool is_selected = std::find(selectedPointIndices.begin(), selectedPointIndices.end(), i) != selectedPointIndices.end();
            
            if (ImGui::Selectable(label.c_str(), is_selected, ImGuiSelectableFlags_AllowDoubleClick)) {
                if (ImGui::GetIO().KeyCtrl) {
                    if (is_selected) {
                        selectedPointIndices.erase(std::remove(selectedPointIndices.begin(), selectedPointIndices.end(), i), selectedPointIndices.end());
                        if (selectedPointIndex == i) selectedPointIndex = selectedPointIndices.empty() ? NO_SELECTION : selectedPointIndices.back();
                    } else {
                        selectedPointIndices.push_back(i);
                        selectedPointIndex = i;
                    }
                } else {
                    selectedPointIndices.clear();
                    selectedPointIndices.push_back(i);
                    selectedPointIndex = i;
                }

                if (ImGui::IsMouseDoubleClicked(0)) {
                    float px, py, pz;
                    trackPoints[i].getPosition(px, py, pz);
                    Vector3 newTarget = { px, py, pz };
                    
                    // Keep the current camera distance and angle relative to the new target
                    Vector3 offset = Vector3Subtract(camera.position, camera.target);
                    camera.target = newTarget;
                    camera.position = Vector3Add(camera.target, offset);
                }
            }
        }
        ImGui::EndListBox();
    }

    if (ImGui::Button("Add Point", ImVec2(100, 0))) {
        AnchorPoint newPt = trackPoints.back();
        float px, py, pz;
        newPt.getPosition(px, py, pz);
        newPt.setPosition(px + 50.0f, py, pz); // Offset to make it visible
        trackPoints.push_back(newPt);
        selectedPointIndex = trackPoints.size() - 1ull;
        selectedPointIndices.clear();
        selectedPointIndices.push_back(selectedPointIndex);
        needsMeshRebuild = true;
        RecordState();
    }
    ImGui::SameLine();
    if (ImGui::Button("Remove", ImVec2(100, 0))) {
        if (!selectedPointIndices.empty() && trackPoints.size() > 1) {
            size_t numToDelete = std::min(selectedPointIndices.size(), trackPoints.size() - 1);
            std::vector<size_t> indicesToDelete = selectedPointIndices;
            indicesToDelete.resize(numToDelete);
            std::sort(indicesToDelete.rbegin(), indicesToDelete.rend());
            
            for (size_t idx : indicesToDelete) {
                if (idx < trackPoints.size()) trackPoints.erase(trackPoints.begin() + idx);
            }
            selectedPointIndices.clear();
            selectedPointIndex = NO_SELECTION;
            
            if (!trackPoints.empty()) {
                selectedPointIndex = trackPoints.size() - 1;
                selectedPointIndices.push_back(selectedPointIndex);
            }
            needsMeshRebuild = true;
            RecordState();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Subdivide", ImVec2(80, 0))) {
        if (selectedPointIndex != NO_SELECTION && selectedPointIndex < trackPoints.size() - 1) {
            SubdivideSegment(selectedPointIndex);
            // Auto-select the newly created point
            selectedPointIndex++;
            selectedPointIndices.clear();
            selectedPointIndices.push_back(selectedPointIndex);
        } else {
            TraceLog(LOG_WARNING, "Cannot subdivide: Select a valid point that is not the very last one.");
        }
    }

    ImGui::Separator();

    // Gizmo Tools Section
    ImGui::Text("Gizmo Tools");
    if (ImGui::RadioButton("Translate (W)", currentGizmoOperation == ImGuizmo::TRANSLATE)) currentGizmoOperation = ImGuizmo::TRANSLATE; ImGui::SameLine();
    if (ImGui::RadioButton("Rotate (E)", currentGizmoOperation == ImGuizmo::ROTATE)) currentGizmoOperation = ImGuizmo::ROTATE; ImGui::SameLine();
    if (ImGui::RadioButton("Scale (R)", currentGizmoOperation == ImGuizmo::SCALE)) currentGizmoOperation = ImGuizmo::SCALE;
    if (ImGui::RadioButton("Local (Q)", currentGizmoMode == ImGuizmo::LOCAL)) currentGizmoMode = ImGuizmo::LOCAL; ImGui::SameLine();
    if (ImGui::RadioButton("World (Q)", currentGizmoMode == ImGuizmo::WORLD)) currentGizmoMode = ImGuizmo::WORLD;
    
    ImGui::Spacing();
    ImGui::Checkbox("Enable Snapping (Hold SHIFT)", &useSnap);
    ImGui::SameLine();
    ImGui::Checkbox("Magnetic Ground (Y=0)", &snapToGround);
    if (useSnap) {
        ImGui::SetNextItemWidth(100);
        ImGui::DragFloat("Pos Step", &snapTranslation, 0.1f, 0.1f, 100.0f, "%.1fm");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100);
        ImGui::DragFloat("Rot Step", &snapRotation, 1.0f, 1.0f, 180.0f, "%.0f°");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100);
        ImGui::DragFloat("Scl Step", &snapScale, 0.05f, 0.05f, 10.0f, "%.2fx");
    }
    
    ImGui::Separator();

    // Appearance & Texture Section
    ImGui::Text("Appearance");
    if (ImGui::Button("Load Texture...", ImVec2(-FLT_MIN, 0))) {
        std::string imgPath = OSUtils::OpenImageFileDialog();
        if (!imgPath.empty()) {
            if (currentTrackTexture.id != 0) UnloadTexture(currentTrackTexture);
            currentTrackTexture = LoadTexture(imgPath.c_str());
            needsMeshRebuild = true;
        }
    }
    if (ImGui::Checkbox("Show Texture Preview", &showTexture)) {
        needsMeshRebuild = true;
    }
    if (ImGui::DragFloat2("Global UV Scale", globalUvScale, 0.05f, 0.01f, 100.0f)) {
        // UV Scale is applied during mesh generation
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) { needsMeshRebuild = true; }
    ImGui::Separator();

    // Visibility Section
    ImGui::Text("Visibility");
    ImGui::Checkbox("Solid Mesh", &showSolidMesh); ImGui::SameLine();
    ImGui::Checkbox("Wireframe", &showWireframe);
    ImGui::Checkbox("Track Nodes", &showTrackNodes); ImGui::SameLine();
    ImGui::Checkbox("Track Curve", &showTrackCurve);
    ImGui::Separator();

    // Camera Tools Section
    ImGui::Text("Camera Controls");
    if (ImGui::Button("Reset View")) {
        camera.position = Vector3{ 800.0f, 500.0f, 800.0f };
        camera.target = Vector3{ 200.0f, 150.0f, -250.0f };
        camera.up = Vector3{ 0.0f, 1.0f, 0.0f };
        camera.fovy = 45.0f;
        camera.projection = CAMERA_PERSPECTIVE;
    }
    ImGui::SameLine();
    bool isOrtho = (camera.projection == CAMERA_ORTHOGRAPHIC);
    if (ImGui::Checkbox("Orthographic", &isOrtho)) {
        camera.projection = isOrtho ? CAMERA_ORTHOGRAPHIC : CAMERA_PERSPECTIVE;
        if (isOrtho) camera.fovy = Vector3Distance(camera.position, camera.target) * 0.8f; 
        else camera.fovy = 45.0f;
    }
    ImGui::Separator();

// Inspector
    if (selectedPointIndex != NO_SELECTION && selectedPointIndex < trackPoints.size()) {
        AnchorPoint& pt = trackPoints[selectedPointIndex];
        
        float px, py, pz;
        pt.getPosition(px, py, pz);
        float pos[3] = { px, py, pz };
        float dir = pt.getDirection() * RAD2DEG;
        float inc = pt.getInclination() * RAD2DEG;
        float bnk = pt.getBank() * RAD2DEG;
        float sw = pt.getScaleWidth();
        float sh = pt.getScaleHeight();
        float prevFac = pt.getPreviousControlPointDistanceFactor();
        float nextFac = pt.getNextControlPointDistanceFactor();

        float oldPos[3] = { pos[0], pos[1], pos[2] };
        float oldDir = dir;
        float oldInc = inc;
        float oldBank = bnk;
        float oldSW = sw;
        float oldSH = sh;
        float oldPrevFac = prevFac;
        float oldNextFac = nextFac;
        
        // Calculate dynamic drag speeds based on snap settings
        float posSpeed = (useSnap && snapTranslation >= 0.1f) ? snapTranslation : 1.0f;
        float rotSpeed = (useSnap && snapRotation >= 1.0f) ? snapRotation : 1.0f;
        float sclSpeed = (useSnap && snapScale >= 0.05f) ? snapScale : 0.05f;
        bool modPos = false, modRot = false, modScale = false, modTan = false;

        // Helper lambda for UI Reset Buttons
        auto ResetButton = [](const char* id, float& val, float resetTo = 0.0f) {
            ImGui::SameLine();
            if (ImGui::Button(id)) {
                val = resetTo;
                return true;
            }
            return false;
        };

        ImGui::Text("Transform");
        if (ImGui::DragFloat3("Position", pos, posSpeed)) {
            if (useSnap) {
                pos[0] = roundf(pos[0] / snapTranslation) * snapTranslation;
                pos[1] = roundf(pos[1] / snapTranslation) * snapTranslation;
                pos[2] = roundf(pos[2] / snapTranslation) * snapTranslation;
            }
            if (snapToGround && fabsf(pos[1]) < 5.0f) {
                pos[1] = 0.0f;
            }
            pt.setPosition(pos[0], pos[1], pos[2]);
            modPos = true;
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) { needsMeshRebuild = true; RecordState(); }

        ImGui::Spacing();
        ImGui::Text("Angles (Degrees)");
        if (ImGui::DragFloat("Direction", &dir, rotSpeed, -360.0f, 360.0f)) {
            if (useSnap) dir = roundf(dir / snapRotation) * snapRotation;
            pt.setDirection(dir * DEG2RAD);
            modRot = true;
        }
        if (ResetButton("0##dir", dir, 0.0f)) { pt.setDirection(0.0f); modRot = true; }
        if (ImGui::IsItemDeactivatedAfterEdit()) { needsMeshRebuild = true; RecordState(); }
        
        if (ImGui::DragFloat("Inclination", &inc, rotSpeed, -90.0f, 90.0f)) {
            if (useSnap) inc = roundf(inc / snapRotation) * snapRotation;
            pt.setInclination(inc * DEG2RAD);
            modRot = true;
        }
        if (ResetButton("0##inc", inc, 0.0f)) { pt.setInclination(0.0f); modRot = true; }
        if (ImGui::IsItemDeactivatedAfterEdit()) { needsMeshRebuild = true; RecordState(); }
        
        if (ImGui::DragFloat("Bank (Roll)", &bnk, rotSpeed, -180.0f, 180.0f)) {
            if (useSnap) bnk = roundf(bnk / snapRotation) * snapRotation;
            pt.setBank(bnk * DEG2RAD);
            modRot = true;
        }
        if (ResetButton("0##bnk", bnk, 0.0f)) { pt.setBank(0.0f); modRot = true; }
        if (ImGui::IsItemDeactivatedAfterEdit()) { needsMeshRebuild = true; RecordState(); }

        ImGui::Spacing();
        ImGui::Text("Scale");
        if (ImGui::DragFloat("Scale W", &sw, sclSpeed, 0.1f, 10.0f)) {
            if (useSnap) sw = roundf(sw / snapScale) * snapScale;
            pt.setScaleWidth(sw);
            modScale = true;
        }
        if (ResetButton("1##sw", sw, 1.0f)) { pt.setScaleWidth(1.0f); modScale = true; }
        if (ImGui::IsItemDeactivatedAfterEdit()) { needsMeshRebuild = true; RecordState(); }
        
        if (ImGui::DragFloat("Scale H", &sh, sclSpeed, 0.1f, 10.0f)) {
            if (useSnap) sh = roundf(sh / snapScale) * snapScale;
            pt.setScaleHeight(sh);
            modScale = true;
        }
        if (ResetButton("1##sh", sh, 1.0f)) { pt.setScaleHeight(1.0f); modScale = true; }
        if (ImGui::IsItemDeactivatedAfterEdit()) { needsMeshRebuild = true; RecordState(); }

        // NEW: Bezier Tangent Controls
        ImGui::Spacing();
        ImGui::Text("Bezier Tangents (Curve Force)");

        if (ImGui::DragFloat("Prev Tangent Length", &prevFac, 0.01f, 0.0f, 2.0f)) {
            pt.setPreviousControlPointDistanceFactor(prevFac);
            modTan = true;
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) { needsMeshRebuild = true; RecordState(); }

        if (ImGui::DragFloat("Next Tangent Length", &nextFac, 0.01f, 0.0f, 2.0f)) {
            pt.setNextControlPointDistanceFactor(nextFac);
            modTan = true;
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) { needsMeshRebuild = true; RecordState(); }
        
        // Apply deltas to other selected nodes
        if (modPos || modRot || modScale || modTan) {
            if (realtimeMeshRebuild) needsMeshRebuild = true;
            
            for (size_t idx : selectedPointIndices) {
                if (idx == selectedPointIndex || idx >= trackPoints.size()) continue;
                AnchorPoint& op = trackPoints[idx];
                if (modPos) { float ox, oy, oz; op.getPosition(ox, oy, oz); op.setPosition(ox + (pos[0]-oldPos[0]), oy + (pos[1]-oldPos[1]), oz + (pos[2]-oldPos[2])); }
                if (modRot) { op.setDirection(op.getDirection() + (dir-oldDir)*DEG2RAD); op.setInclination(op.getInclination() + (inc-oldInc)*DEG2RAD); op.setBank(op.getBank() + (bnk-oldBank)*DEG2RAD); }
                if (modScale) { float nsw = op.getScaleWidth()+(sw-oldSW), nsh = op.getScaleHeight()+(sh-oldSH); op.setScaleWidth(nsw<0.01f?0.01f:nsw); op.setScaleHeight(nsh<0.01f?0.01f:nsh); }
                if (modTan) { op.setPreviousControlPointDistanceFactor(op.getPreviousControlPointDistanceFactor() + (prevFac-oldPrevFac)); op.setNextControlPointDistanceFactor(op.getNextControlPointDistanceFactor() + (nextFac-oldNextFac)); }
            }
        }
    }

    ImGui::End();
}


// --- GIZMO RENDERING & LOGIC ---
void DrawGizmo(Matrix viewMat, Matrix projMat, bool isOrthographic) {
    if (selectedPointIndex == NO_SELECTION || selectedPointIndex >= trackPoints.size()) return;

    ImGuiIO& io = ImGui::GetIO();
    
    // Force ImGuizmo to draw into the background of the screen, not inside a floating window
    ImGuizmo::SetDrawlist(ImGui::GetBackgroundDrawList());
    ImGuizmo::SetOrthographic(isOrthographic);
    ImGuizmo::SetRect(0, 0, io.DisplaySize.x, io.DisplaySize.y);

    AnchorPoint& pt = trackPoints[selectedPointIndex];
    float px, py, pz;
    pt.getPosition(px, py, pz);

    // Convert Raylib's Row-Major struct to Column-Major float arrays for ImGuizmo
    float view[16] = {
        viewMat.m0, viewMat.m1, viewMat.m2, viewMat.m3,
        viewMat.m4, viewMat.m5, viewMat.m6, viewMat.m7,
        viewMat.m8, viewMat.m9, viewMat.m10, viewMat.m11,
        viewMat.m12, viewMat.m13, viewMat.m14, viewMat.m15
    };

    float proj[16] = {
        projMat.m0, projMat.m1, projMat.m2, projMat.m3,
        projMat.m4, projMat.m5, projMat.m6, projMat.m7,
        projMat.m8, projMat.m9, projMat.m10, projMat.m11,
        projMat.m12, projMat.m13, projMat.m14, projMat.m15
    };

    matrix rotMat = pt.getMatrix();
    float sw = pt.getScaleWidth();
    float sh = pt.getScaleHeight();

    // Build the matrix for ImGuizmo.
    // Trackmaker uses a Right-Handed system: X = Forward, Y = Up, Z = Right
    // The 3D visual standard expects: X (Red) = Right, Y (Green) = Up.
    // To keep the matrix Right-Handed and prevent mouse controls from inverting,
    // the Gizmo's Z axis (Blue) MUST point Backward (-Forward).
    float gizmoMatrix[16] = {
        rotMat.z.x * sw, rotMat.z.y * sw, rotMat.z.z * sw, 0.0f, // Gizmo X (Red)   = Right
        rotMat.y.x * sh, rotMat.y.y * sh, rotMat.y.z * sh, 0.0f, // Gizmo Y (Green) = Up
       -rotMat.x.x,     -rotMat.x.y,     -rotMat.x.z,      0.0f, // Gizmo Z (Blue)  = Backward
        px,              py,              pz,              1.0f  // Position
    };

    // Prepare snapping values
    float snapValues[3] = { 0.0f, 0.0f, 0.0f };
    bool shouldSnap = useSnap || IsKeyDown(KEY_LEFT_SHIFT);
    
    if (shouldSnap) {
        if (currentGizmoOperation == ImGuizmo::TRANSLATE) { snapValues[0] = snapTranslation; snapValues[1] = snapTranslation; snapValues[2] = snapTranslation; }
        else if (currentGizmoOperation == ImGuizmo::ROTATE) { snapValues[0] = snapRotation; snapValues[1] = snapRotation; snapValues[2] = snapRotation; }
        else if (currentGizmoOperation == ImGuizmo::SCALE) { snapValues[0] = snapScale; snapValues[1] = snapScale; snapValues[2] = snapScale; }
    }

    // Disable Gizmo interaction when CTRL is held.
    // This stops ImGuizmo from eating the mouse click, allowing you to easily deselect the primary node!
    bool isCtrlDown = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    ImGuizmo::Enable(!isCtrlDown);

    float deltaMatrix[16];
    ImGuizmo::Manipulate(view, proj, currentGizmoOperation, currentGizmoMode, gizmoMatrix, deltaMatrix, shouldSnap ? snapValues : nullptr);

    ImGuizmo::Enable(true); // Re-enable immediately so other ImGuizmo features (ViewCube) aren't broken

    static bool wasUsingGizmo = false;
    bool isUsingGizmo = ImGuizmo::IsUsing();

    // If we are actively dragging the gizmo, update the point visually
    if (isUsingGizmo) {
        float oldX, oldY, oldZ; pt.getPosition(oldX, oldY, oldZ);
        float oldDir = pt.getDirection(), oldInc = pt.getInclination(), oldBank = pt.getBank();
        float oldSW = pt.getScaleWidth(), oldSH = pt.getScaleHeight();
        
        float dX = 0, dY = 0, dZ = 0, dDir = 0, dInc = 0, dBank = 0, dSW = 0, dSH = 0;

        // Extract Position
        float newX = gizmoMatrix[12];
        float newY = gizmoMatrix[13];
        float newZ = gizmoMatrix[14];

        // Prevents lateral drift: Apply global magnetism (Y=0) ONLY if Gizmo is in WORLD mode.
        // Applying global snap during translation on a slanted local axis causes mathematical feedback.
        if (currentGizmoOperation == ImGuizmo::TRANSLATE && snapToGround && currentGizmoMode == ImGuizmo::WORLD) {
            if (fabsf(newY) < 5.0f) newY = 0.0f;
        }
        pt.setPosition(newX, newY, newZ);
        dX = newX - oldX; dY = newY - oldY; dZ = newZ - oldZ;

        // Only extract scale if we are explicitly scaling to prevent floating point degradation
        if (currentGizmoOperation == ImGuizmo::SCALE) {
            // Extract Scale (Length of Gizmo's X and Y columns)
            float newSW = Vector3Length(Vector3{gizmoMatrix[0], gizmoMatrix[1], gizmoMatrix[2]});
            float newSH = Vector3Length(Vector3{gizmoMatrix[4], gizmoMatrix[5], gizmoMatrix[6]});
            pt.setScaleWidth(newSW < 0.01f ? 0.01f : newSW);
            pt.setScaleHeight(newSH < 0.01f ? 0.01f : newSH);
            dSW = pt.getScaleWidth() - oldSW; dSH = pt.getScaleHeight() - oldSH;
        }
        // Only extract rotation if we are explicitly rotating
        else if (currentGizmoOperation == ImGuizmo::ROTATE) {
            float newSW = pt.getScaleWidth();
            float newSH = pt.getScaleHeight();

            // Extract and Normalize Rotation Axes
            Vector3 newRight    = Vector3Scale(Vector3{gizmoMatrix[0], gizmoMatrix[1], gizmoMatrix[2]}, 1.0f / (newSW < 0.01f ? 0.01f : newSW));
            Vector3 newUp       = Vector3Scale(Vector3{gizmoMatrix[4], gizmoMatrix[5], gizmoMatrix[6]}, 1.0f / (newSH < 0.01f ? 0.01f : newSH));
            Vector3 newBackward = Vector3Normalize(Vector3{gizmoMatrix[8], gizmoMatrix[9], gizmoMatrix[10]});
            
            // Reconstruct Forward by flipping Backward
            Vector3 newForward  = Vector3{-newBackward.x, -newBackward.y, -newBackward.z};

            // Robust Euler Angle Reconstruction (Trackmaker Order: Y -> Z -> X)
            float sinI = newForward.y;
            if (sinI > 1.0f) sinI = 1.0f;
            if (sinI < -1.0f) sinI = -1.0f;
            float inc = asinf(sinI);

            float dir, bank;
            float cosI = cosf(inc);
            
            if (fabs(cosI) > 0.001f) {
                dir = atan2f(-newForward.z, newForward.x);
                bank = atan2f(-newRight.y, newUp.y);
            } else {
                // Gimbal Lock prevention when the axis points exactly vertical (+/- 90 degrees Pitch)
                bank = pt.getBank();
                dir = pt.getDirection();
            }
            
            pt.setInclination(inc);
            pt.setDirection(dir);
            pt.setBank(bank);
            dDir = dir - oldDir; dInc = inc - oldInc; dBank = bank - oldBank;
        }

        // Apply deltas to other selected nodes
        for (size_t idx : selectedPointIndices) {
            if (idx == selectedPointIndex || idx >= trackPoints.size()) continue;
            AnchorPoint& op = trackPoints[idx];
            
            float ox, oy, oz; op.getPosition(ox, oy, oz);
            op.setPosition(ox + dX, oy + dY, oz + dZ);
            if (currentGizmoOperation == ImGuizmo::ROTATE) {
                op.setDirection(op.getDirection() + dDir); op.setInclination(op.getInclination() + dInc); op.setBank(op.getBank() + dBank);
            } else if (currentGizmoOperation == ImGuizmo::SCALE) {
                float nsw = op.getScaleWidth() + dSW, nsh = op.getScaleHeight() + dSH;
                op.setScaleWidth(nsw<0.01f?0.01f:nsw); op.setScaleHeight(nsh<0.01f?0.01f:nsh);
            }
        }
        
        if (realtimeMeshRebuild) {
            needsMeshRebuild = true;
        }
    }

    // Trigger a mesh rebuild ONLY when the user releases the mouse to save performance
    if (wasUsingGizmo && !isUsingGizmo) {
        needsMeshRebuild = true;
        RecordState();
    }
    wasUsingGizmo = isUsingGizmo;
}


// --- VIEW CUBE OVERLAY ---
void DrawViewCube(Camera3D& camera) {
    // PURE APPROACH: Draw entirely on top of everything, completely bypassing ImGui Windows!
    ImGuizmo::SetDrawlist(ImGui::GetForegroundDrawList());

    Matrix viewMat = GetCameraMatrix(camera);
    
    // EXPLICIT MAPPING to OpenGL Column-Major array
    float view[16] = {
        viewMat.m0, viewMat.m1, viewMat.m2, viewMat.m3,
        viewMat.m4, viewMat.m5, viewMat.m6, viewMat.m7,
        viewMat.m8, viewMat.m9, viewMat.m10, viewMat.m11,
        viewMat.m12, viewMat.m13, viewMat.m14, viewMat.m15
    };

    float oldView[16];
    for(int i = 0; i < 16; i++) oldView[i] = view[i];

    float camDistance = Vector3Distance(camera.position, camera.target);
    
    // Stick it precisely in the top right corner of the screen
    ImVec2 size(140, 140);
    ImVec2 pos(GetScreenWidth() - size.x, 0);
    ImGuizmo::ViewManipulate(view, camDistance, pos, size, 0x00000000);

    // If ImGuizmo modified the matrix, safely invert it back to the Raylib Camera struct
    bool modified = false;
    for(int i = 0; i < 16; i++) {
        if(view[i] != oldView[i]) { modified = true; break; }
    }

    if (modified) {
        Matrix updatedView;
        updatedView.m0 = view[0];   updatedView.m1 = view[1];   updatedView.m2 = view[2];   updatedView.m3 = view[3];
        updatedView.m4 = view[4];   updatedView.m5 = view[5];   updatedView.m6 = view[6];   updatedView.m7 = view[7];
        updatedView.m8 = view[8];   updatedView.m9 = view[9];   updatedView.m10 = view[10]; updatedView.m11 = view[11];
        updatedView.m12 = view[12]; updatedView.m13 = view[13]; updatedView.m14 = view[14]; updatedView.m15 = view[15];
        
        Matrix invView = MatrixInvert(updatedView);

        camera.position = Vector3{ invView.m12, invView.m13, invView.m14 };
        Vector3 forward = { -invView.m8, -invView.m9, -invView.m10 };
        camera.target = Vector3Add(camera.position, Vector3Scale(forward, camDistance));
        camera.up = Vector3{ invView.m4, invView.m5, invView.m6 };
    }
}


// --- SELECTION LOGIC ---
void HandleMousePicking(Camera3D& camera) {
    if (!showTrackNodes) return; // Prevent selecting invisible nodes

    static bool isTrackingClick = false;
    
    // Define the exclusion zone for the ViewCube (top right 140x140 area)
    bool isHoveringViewCube = (GetMouseX() >= GetScreenWidth() - 140 && GetMouseY() <= 140);

    bool isCtrlDown = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);

    // By ignoring ImGuizmo::IsOver() when CTRL is held, we can click "through" the gizmo to deselect
    if (ImGui::GetIO().WantCaptureMouse || (ImGuizmo::IsOver() && !isCtrlDown) || isHoveringViewCube) {
        isTrackingClick = false;
        return;
    }

    static Vector2 clickPosition = { 0.0f, 0.0f };

    // Store the mouse position when the user first presses the button
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        clickPosition = GetMousePosition();
        isTrackingClick = true;
    }

    // Only process the picking when the button is released AND we started the click cleanly
    if (isTrackingClick && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
        isTrackingClick = false;
        Vector2 releasePosition = GetMousePosition();
        float distance = Vector2Distance(clickPosition, releasePosition);

        // If the mouse moved less than 5 pixels, consider it a deliberate "click" instead of a "drag"
        if (distance < 5.0f) {
            Ray ray = GetMouseRay(GetMousePosition(), camera);
            float closestDistance = 1000000.0f; // A very large number to find the closest hit
            size_t hitIndex = NO_SELECTION;

            for (size_t i = 0; i < trackPoints.size(); i++) {
                float px, py, pz;
                trackPoints[i].getPosition(px, py, pz);
                Vector3 pos = { px, py, pz };

                // The radius used in DrawTrackStructure is 15.0f
                RayCollision collision = GetRayCollisionSphere(ray, pos, 15.0f);

                if (collision.hit && collision.distance < closestDistance) {
                    closestDistance = collision.distance;
                    hitIndex = i;
                }
            }

            if (IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL)) {
                if (hitIndex != NO_SELECTION) {
                    auto it = std::find(selectedPointIndices.begin(), selectedPointIndices.end(), hitIndex);
                    if (it != selectedPointIndices.end()) {
                        selectedPointIndices.erase(it);
                        if (selectedPointIndex == hitIndex) selectedPointIndex = selectedPointIndices.empty() ? NO_SELECTION : selectedPointIndices.back();
                    } else {
                        selectedPointIndices.push_back(hitIndex);
                        selectedPointIndex = hitIndex;
                    }
                }
            } else {
                selectedPointIndices.clear();
                if (hitIndex != NO_SELECTION) {
                    selectedPointIndices.push_back(hitIndex);
                }
                selectedPointIndex = hitIndex;
            }
        }
    }
}


// --- CAMERA LOGIC ---
void UpdateEditorCamera(Camera3D& camera) {
    if (ImGui::GetIO().WantCaptureMouse) return;

    Vector2 mouseDelta = GetMouseDelta();
    float wheel = GetMouseWheelMove();

    // 1. Zoom (Scroll Wheel)
    if (wheel != 0.0f) {
        if (camera.projection == CAMERA_ORTHOGRAPHIC) {
            // In Ortho mode, zooming modifies the viewport width/height directly via fovy
            camera.fovy -= wheel * 50.0f;
            if (camera.fovy < 10.0f) camera.fovy = 10.0f;
        } else {
            Vector3 forward = Vector3Normalize(Vector3Subtract(camera.target, camera.position));
            camera.position = Vector3Add(camera.position, Vector3Scale(forward, wheel * 100.0f));
        }
    }

    // 2. Orbit around target (Left Mouse Button)
    if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
        Vector3 posFromTarget = Vector3Subtract(camera.position, camera.target);

        float yawAngle = -mouseDelta.x * 0.005f;
        float pitchAngle = -mouseDelta.y * 0.005f;

        // Determine if we are upside down to keep mouse yaw intuitive
        bool isUpsideDown = (camera.up.y < 0.0f);
        Vector3 globalUp = { 0.0f, isUpsideDown ? -1.0f : 1.0f, 0.0f };

        // Calculate local Right vector based on Global UP
        Vector3 forward = Vector3Normalize(Vector3Scale(posFromTarget, -1.0f));
        Vector3 right = Vector3CrossProduct(forward, globalUp);

        // Prevent gimbal lock when looking exactly up/down
        if (Vector3Length(right) < 0.001f) {
            right = Vector3CrossProduct(forward, camera.up);
        }
        right = Vector3Normalize(right);

        // Apply Pitch (around local right axis)
        Matrix pitchMatrix = MatrixRotate(right, pitchAngle);
        posFromTarget = Vector3Transform(posFromTarget, pitchMatrix);

        // Apply Yaw (around global Y axis)
        Matrix yawMatrix = MatrixRotate(Vector3{ 0.0f, 1.0f, 0.0f }, yawAngle);
        posFromTarget = Vector3Transform(posFromTarget, yawMatrix);

        camera.position = Vector3Add(camera.target, posFromTarget);

        // Strictly re-calculate the UP vector to perfectly eliminate any roll drift (Tilt)
        forward = Vector3Normalize(Vector3Subtract(camera.target, camera.position));
        right = Vector3CrossProduct(forward, globalUp);
        
        if (Vector3Length(right) < 0.001f) {
            // If we pitched exactly to the pole, keep the old UP vector but rotate it
            camera.up = Vector3Transform(camera.up, pitchMatrix);
            camera.up = Vector3Transform(camera.up, yawMatrix);
        } else {
            camera.up = Vector3Normalize(Vector3CrossProduct(Vector3Normalize(right), forward));
        }
    }

    // 3. Pan (Right OR Middle Mouse Button)
    if (IsMouseButtonDown(MOUSE_BUTTON_RIGHT) || IsMouseButtonDown(MOUSE_BUTTON_MIDDLE)) {
        Vector3 forward = Vector3Normalize(Vector3Subtract(camera.target, camera.position));
        Vector3 right = Vector3Normalize(Vector3CrossProduct(forward, camera.up));
        Vector3 up = Vector3Normalize(Vector3CrossProduct(right, forward));

        // Calculate distance to scale the panning speed dynamically
        float dist = Vector3Distance(camera.position, camera.target);
        float panSpeed = dist * 0.001f;

        Vector3 panDelta = Vector3Add(Vector3Scale(right, -mouseDelta.x * panSpeed), Vector3Scale(up, mouseDelta.y * panSpeed));

        camera.position = Vector3Add(camera.position, panDelta);
        camera.target = Vector3Add(camera.target, panDelta);
    }
}


// --- CUSTOM GRID DRAWING ---
void DrawCustomGrid(int slices, float spacing, float yOffset, Color color) {
    int halfSlices = slices / 2;

    rlBegin(RL_LINES);
    for (int i = -halfSlices; i <= halfSlices; i++) {
        rlColor4ub(color.r, color.g, color.b, color.a);

        rlVertex3f((float)i * spacing, yOffset, (float)-halfSlices * spacing);
        rlVertex3f((float)i * spacing, yOffset, (float)halfSlices * spacing);

        rlVertex3f((float)-halfSlices * spacing, yOffset, (float)i * spacing);
        rlVertex3f((float)halfSlices * spacing, yOffset, (float)i * spacing);
    }
    rlEnd();
}

// --- 3D HYBRID VIEWPORT ---
void DrawTrackStructure() {
    for (size_t i = 0; i < trackPoints.size(); i++) {
        AnchorPoint& pt = trackPoints[i];

        float px, py, pz;
        pt.getPosition(px, py, pz);
        Vector3 pos = { px, py, pz };

        matrix rotMat = pt.getMatrix();
        Vector3 forward = { rotMat.x.x, rotMat.x.y, rotMat.x.z };

        if (showTrackNodes) {
            // Draw local orientation axes
            Vector3 up = { rotMat.y.x, rotMat.y.y, rotMat.y.z };
            Vector3 right = { rotMat.z.x, rotMat.z.y, rotMat.z.z };

            // INCREASED AXIS SCALE: Changed from 10.0f to 50.0f
            float axisLength = 50.0f;
            DrawLine3D(pos, Vector3Add(pos, Vector3Scale(forward, axisLength * 2.0f)), BLUE);
            DrawLine3D(pos, Vector3Add(pos, Vector3Scale(up, axisLength * pt.getScaleHeight())), GREEN);
            DrawLine3D(pos, Vector3Add(pos, Vector3Scale(right, axisLength * pt.getScaleWidth())), RED);
            DrawLine3D(pos, Vector3Subtract(pos, Vector3Scale(right, axisLength * pt.getScaleWidth())), RED);

            // INCREASED SPHERE SCALE: Changed radius from 2.0f to 15.0f
            Color pointColor = YELLOW;
            if (std::find(selectedPointIndices.begin(), selectedPointIndices.end(), i) != selectedPointIndices.end()) {
                pointColor = (i == selectedPointIndex) ? RED : ORANGE;
            }
            DrawSphere(pos, 15.0f, pointColor);
        }

        // Draw Bezier Curves
        if (showTrackCurve && i < trackPoints.size() - 1) {
            AnchorPoint& nextPt = trackPoints[i + 1];

            float nx, ny, nz;
            nextPt.getPosition(nx, ny, nz);
            Vector3 nextPos = { nx, ny, nz };

            matrix m2 = nextPt.getMatrix();
            float length = Vector3Distance(pos, nextPos);

            Vector3 p1 = Vector3Add(pos, Vector3Scale(forward, length * pt.getNextControlPointDistanceFactor()));
            Vector3 p2 = Vector3Add(nextPos, Vector3Scale(Vector3{ m2.x.x, m2.x.y, m2.x.z }, -length * nextPt.getPreviousControlPointDistanceFactor()));

            Vector3 previousCurvePt = pos;
            for (int step = 1; step <= 20; step++) {
                float t = (float)step / 20.0f;
                Vector3 currentCurvePt = EvaluateCubicBezier(pos, p1, p2, nextPos, t);
                DrawLine3D(previousCurvePt, currentCurvePt, ORANGE);
                previousCurvePt = currentCurvePt;
            }
        }
    }
}


// --- MAIN ENTRY POINT ---
int main(void) {
    const int screenWidth = 1280;
    const int screenHeight = 720;

    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT);
    InitWindow(screenWidth, screenHeight, "Trackmaker - Windows Editor");

    // Increase the far clipping plane distance to render massive track layouts.
    // Pushing the near plane to 10.0 further avoids aggressive Z-buffer precision loss.
    rlSetClipPlanes(10.0, 100000.0);

    // INCREASED INITIAL SCALE: Pull the camera back to see large tracks like scurve.track
    Camera3D camera = { 0 };
    camera.position = Vector3{ 800.0f, 500.0f, 800.0f };
    camera.target = Vector3{ 200.0f, 150.0f, -250.0f }; // Roughly the center of scurve
    camera.up = Vector3{ 0.0f, 1.0f, 0.0f };
    camera.fovy = 45.0f;
    camera.projection = CAMERA_PERSPECTIVE;

    rlImGuiSetup(true);

    // NEW: Disable the generation of imgui.ini to keep your track folders clean
    ImGui::GetIO().IniFilename = nullptr;

    SetTargetFPS(60);

    Matrix viewMat = { 0 };
    Matrix projMat = { 0 };

    while (!WindowShouldClose()) {

        // Handle global shortcuts for Gizmo tools
        if (!ImGui::GetIO().WantTextInput) {
            if (IsKeyPressed(KEY_W)) currentGizmoOperation = ImGuizmo::TRANSLATE;
            if (IsKeyPressed(KEY_E)) currentGizmoOperation = ImGuizmo::ROTATE;
            if (IsKeyPressed(KEY_R)) currentGizmoOperation = ImGuizmo::SCALE;
            if (IsKeyPressed(KEY_Q)) {
                currentGizmoMode = (currentGizmoMode == ImGuizmo::LOCAL) ? ImGuizmo::WORLD : ImGuizmo::LOCAL;
            }

            // Handle Undo/Redo shortcuts (Ctrl+Z / Ctrl+Y)
            bool isCtrlDown = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
            if (isCtrlDown && IsKeyPressed(KEY_Z)) {
                Undo();
            }
            if (isCtrlDown && IsKeyPressed(KEY_Y)) {
                Redo();
            }
            
            // Handle Duplicate Point shortcut (Ctrl+D)
            if (isCtrlDown && IsKeyPressed(KEY_D)) {
                if (!selectedPointIndices.empty()) {
                    std::vector<size_t> sortedIndices = selectedPointIndices;
                    std::sort(sortedIndices.begin(), sortedIndices.end()); // Ascending order
                    
                    std::vector<AnchorPoint> duplicatedPoints;
                    
                    // To keep the block shape intact, we calculate a SINGLE offset 
                    // along the direction of the primary node (the one with the Gizmo).
                    size_t referenceIdx = (selectedPointIndex != NO_SELECTION && selectedPointIndex < trackPoints.size()) 
                                          ? selectedPointIndex : sortedIndices.back();
                    
                    matrix refMat = trackPoints[referenceIdx].getMatrix();
                    float dx = refMat.x.x * 10.0f;
                    float dy = refMat.x.y * 10.0f;
                    float dz = refMat.x.z * 10.0f;

                    // Append the block right after the last element of the selection
                    size_t insertIndex = sortedIndices.back() + 1;
                    
                    for (size_t idx : sortedIndices) {
                        if (idx < trackPoints.size()) {
                            AnchorPoint dup = trackPoints[idx];
                            float px, py, pz; dup.getPosition(px, py, pz);
                            dup.setPosition(px + dx, py + dy, pz + dz);
                            duplicatedPoints.push_back(dup);
                        }
                    }
                    
                    // Insert all duplicated points as a sequential block
                    trackPoints.insert(trackPoints.begin() + insertIndex, duplicatedPoints.begin(), duplicatedPoints.end());
                    
                    // Update selection to the newly created points (shifting focus)
                    selectedPointIndices.clear();
                    size_t newPrimaryIndex = insertIndex;
                    for (size_t i = 0; i < sortedIndices.size(); ++i) {
                        size_t newIdx = insertIndex + i;
                        selectedPointIndices.push_back(newIdx);
                        if (sortedIndices[i] == selectedPointIndex) {
                            newPrimaryIndex = newIdx; // Keeps the gizmo on the corresponding node
                        }
                    }
                    
                    selectedPointIndex = newPrimaryIndex;

                    needsMeshRebuild = true;
                    RecordState();
                    TraceLog(LOG_INFO, "Multiple nodes duplicated as block via CTRL+D");
                }
            }
        }

        if (needsMeshRebuild) {
            RebuildTrackMesh();
        }

        HandleMousePicking(camera);

        // --- ABSOLUTE CAMERA PROTECTION ---
        // Explicitly block Raylib from rotating the background camera if we are interacting with the ViewCube
        static bool isDraggingViewCube = false;
        bool isHoveringViewCube = (GetMouseX() >= GetScreenWidth() - 140 && GetMouseY() <= 140);
        
        if (isHoveringViewCube && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) isDraggingViewCube = true;
        if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) isDraggingViewCube = false;

        // Only update standard camera logic if we are completely clear of the top-right corner
        if (!isDraggingViewCube && !isHoveringViewCube) {
            UpdateEditorCamera(camera);
        }

        BeginDrawing();
        ClearBackground(Color{ 30, 30, 30, 255 });

        BeginMode3D(camera);

        // Capture matrices while in 3D mode for ImGuizmo
        viewMat = rlGetMatrixModelview();
        projMat = rlGetMatrixProjection();

        // --- BACKGROUND GRID FIX ---
        // By disabling the depth test, we force the grid to render purely as a background.
        // This completely eliminates Z-fighting, regardless of how far the camera is!
        rlDisableDepthTest();

        Color gridColor = { 50, 50, 50, 255 }; // Subtle dark gray
        DrawCustomGrid(266, 250.0f, 0.0f, gridColor);

        // Draw origin axes (X = Red, Z = Blue)
        DrawLine3D(Vector3{ -33250.0f, 0.0f, 0.0f }, Vector3{ 33250.0f, 0.0f, 0.0f }, RED);
        DrawLine3D(Vector3{ 0.0f, 0.0f, -33250.0f }, Vector3{ 0.0f, 0.0f, 33250.0f }, BLUE);

        rlDrawRenderBatchActive(); // Force flush the lines so they render right now
        rlEnableDepthTest();       // Re-enable depth testing for the 3D track
        // ---------------------------

        // 1. Draw the solid 3D Track Model FIRST
        if (currentTrackModel.meshCount > 0) {
            if (showSolidMesh) DrawModel(currentTrackModel, Vector3{ 0.0f, 0.0f, 0.0f }, 1.0f, WHITE);
            if (showWireframe) DrawModelWires(currentTrackModel, Vector3{ 0.0f, 0.0f, 0.0f }, 1.0f, DARKGRAY);
        }

        // 2. Disable depth testing for X-Ray vision
        rlDisableDepthTest();

        // 3. Queue the skeleton and points
        DrawTrackStructure();

        // 4. Flush the 3D queue while depth testing is disabled
        EndMode3D();

        // REMOVED: rlEnableDepthTest(); 
        // By leaving it disabled, ImGui is guaranteed to draw over everything!

        rlImGuiBegin();
        ImGuizmo::BeginFrame();

        // FIX: Force ImGuizmo context initialization to prevent division by zero (NaN) 
        // inside ViewManipulate when no Anchor Point is currently selected.
        ImGuizmo::SetRect(0.0f, 0.0f, static_cast<float>(GetScreenWidth()), static_cast<float>(GetScreenHeight()));
        float identity[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
        
        // Passing identity to view/proj ensures we don't accidentally intercept actual 3D interactions
        ImGuizmo::Manipulate(identity, identity, (ImGuizmo::OPERATION)0, ImGuizmo::LOCAL, identity);

        RenderTrackmakerGUI(camera);
        DrawViewCube(camera);
        if (showTrackNodes) {
            DrawGizmo(viewMat, projMat, camera.projection == CAMERA_ORTHOGRAPHIC);
        }

        rlImGuiEnd();

        EndDrawing();
    }

    // Cleanup GPU Memory
    if (currentTrackModel.meshCount > 0) {
        UnloadModel(currentTrackModel);
    }
    if (currentTrackTexture.id != 0) {
        UnloadTexture(currentTrackTexture);
    }
    rlImGuiShutdown();
    CloseWindow();
    return 0;
}
#include "raylib.h"
#include "raymath.h"
#include "imgui.h"
#include "rlImGui.h"
#include "rlgl.h"
#include "ImGuizmo.h"
#include <fstream>
#include <sstream>
#include <cstdio>
#include <vector>
#include <string>
#include <exception>
#include <deque>
#include <algorithm>

// Clean OS utilities (No Windows.h here!)
#include "os_utils.h"

// Trackmaker core headers
#include "shared/Vec4.h"
#include "shared/AnchorPoint.h"
#include "shared/Track.h"
#include "shared/Template.h"
#include "shared/FileWriter.h"


// --- GLOBAL STATE ---
std::vector<AnchorPoint> trackPoints;
const size_t NO_SELECTION = static_cast<size_t>(-1);
size_t selectedPointIndex = NO_SELECTION;
std::vector<size_t> selectedPointIndices;
std::string currentTemplatePath = "";
bool needsMeshRebuild = false;
std::string lastMeshError = "";

// --- GIZMO STATE ---
ImGuizmo::OPERATION currentGizmoOperation = ImGuizmo::TRANSLATE;
ImGuizmo::MODE currentGizmoMode = ImGuizmo::LOCAL;

// --- SNAP STATE ---
bool useSnap = false;
float snapTranslation = 1.0f; // Meters
float snapRotation = 15.0f;   // Degrees
float snapScale = 0.5f;       // Multiplier
bool snapToGround = true;     // Magnetic ground snap

// --- EDITOR PERFORMANCE STATE ---
bool realtimeMeshRebuild = false; // Toggle real-time 3D mesh generation on drag

// --- UNDO/REDO STATE ---
struct EditorState {
    std::vector<AnchorPoint> points;
    size_t selectedIndex;
    std::vector<size_t> selectedIndices;
};
std::deque<EditorState> undoHistory;
int historyIndex = -1;
const size_t MAX_HISTORY_STATES = 100;

// The Raylib model that holds the solid track mesh
Model currentTrackModel = { 0 };

// --- APPEARANCE & TEXTURE STATE ---
Texture2D currentTrackTexture = { 0 };
bool showTexture = true;
float globalUvScale[2] = { 1.0f, 1.0f }; // U and V scaling multipliers
bool showSolidMesh = true;
bool showWireframe = true;
bool showTrackCurve = true;
bool showTrackNodes = true;


// --- UTILITY: BEZIER MATH FOR HYBRID VIEW ---
Vector3 EvaluateCubicBezier(Vector3 p0, Vector3 p1, Vector3 p2, Vector3 p3, float t) {
    float u = 1.0f - t;
    float tt = t * t;
    float uu = u * u;
    float uuu = uu * u;
    float ttt = tt * t;

    Vector3 p = Vector3Scale(p0, uuu);
    p = Vector3Add(p, Vector3Scale(p1, 3 * uu * t));
    p = Vector3Add(p, Vector3Scale(p2, 3 * u * tt));
    p = Vector3Add(p, Vector3Scale(p3, ttt));
    return p;
}


// --- UNDO/REDO LOGIC ---
bool AreStatesEqual(const EditorState& a, const EditorState& b) {
    if (a.points.size() != b.points.size()) return false;
    for (size_t i = 0; i < a.points.size(); ++i) {
        float pA[3], pB[3];
        a.points[i].getPosition(pA[0], pA[1], pA[2]);
        b.points[i].getPosition(pB[0], pB[1], pB[2]);
        
        if (pA[0] != pB[0] || pA[1] != pB[1] || pA[2] != pB[2]) return false;
        if (a.points[i].getDirection() != b.points[i].getDirection()) return false;
        if (a.points[i].getInclination() != b.points[i].getInclination()) return false;
        if (a.points[i].getBank() != b.points[i].getBank()) return false;
        if (a.points[i].getScaleWidth() != b.points[i].getScaleWidth()) return false;
        if (a.points[i].getScaleHeight() != b.points[i].getScaleHeight()) return false;
        if (a.points[i].getPreviousControlPointDistanceFactor() != b.points[i].getPreviousControlPointDistanceFactor()) return false;
        if (a.points[i].getNextControlPointDistanceFactor() != b.points[i].getNextControlPointDistanceFactor()) return false;
    }
    return true;
}

void RecordState() {
    EditorState newState = { trackPoints, selectedPointIndex, selectedPointIndices };

    // Prevent saving identical consecutive states (e.g., clicking the gizmo without dragging)
    if (historyIndex >= 0 && historyIndex < static_cast<int>(undoHistory.size())) {
        if (AreStatesEqual(undoHistory[historyIndex], newState)) {
            // Just update the selection index if it changed, without creating a new state
            undoHistory[historyIndex].selectedIndex = selectedPointIndex;
            undoHistory[historyIndex].selectedIndices = selectedPointIndices;
            return;
        }
    }

    // If we have undone actions and make a new modification, clear the "future" (Redo) states
    if (historyIndex >= 0 && historyIndex < static_cast<int>(undoHistory.size()) - 1) {
        undoHistory.erase(undoHistory.begin() + historyIndex + 1, undoHistory.end());
    }

    undoHistory.push_back(newState);

    // Enforce the states limit
    if (undoHistory.size() > MAX_HISTORY_STATES) {
        undoHistory.pop_front();
    } else {
        historyIndex++;
    }
}

void Undo() {
    if (historyIndex > 0) {
        historyIndex--;
        trackPoints = undoHistory[historyIndex].points;
        selectedPointIndex = undoHistory[historyIndex].selectedIndex;
        selectedPointIndices = undoHistory[historyIndex].selectedIndices;
        needsMeshRebuild = true;
        TraceLog(LOG_INFO, "Undo performed. Reverted to state %d", historyIndex);
    }
}

void Redo() {
    if (historyIndex < static_cast<int>(undoHistory.size()) - 1) {
        historyIndex++;
        trackPoints = undoHistory[historyIndex].points;
        selectedPointIndex = undoHistory[historyIndex].selectedIndex;
        selectedPointIndices = undoHistory[historyIndex].selectedIndices;
        needsMeshRebuild = true;
        TraceLog(LOG_INFO, "Redo performed. Advanced to state %d", historyIndex);
    }
}


// --- MESH GENERATION (The Core Engine Link) ---
void RebuildTrackMesh() {
    needsMeshRebuild = false;

    if (trackPoints.size() < 2) return;
    if (currentTemplatePath.empty()) return;

    TraceLog(LOG_INFO, "Rebuilding solid 3D mesh in memory...");

    try {
        lastMeshError = ""; // Clear previous error

        // 1. Initialize the Template using the exact C-string signature
        Template trackTemplate(currentTemplatePath.c_str());

        // 2. Initialize the Track by passing the template reference
        Track track(trackTemplate);

        // 3. DIRECT MEMORY INJECTION: Copy UI points into the Track engine
        for (const AnchorPoint& uiPt : trackPoints) {

            // Create a new blank point inside the engine's memory vector
            AnchorPoint& enginePt = track.createAnchorPointAtEnd();

            // Extract values from our UI point
            float px, py, pz;
            uiPt.getPosition(px, py, pz);

            // Apply them directly to the newly created engine point
            enginePt.setPosition(px, py, pz);
            enginePt.setDirection(uiPt.getDirection());
            enginePt.setInclination(uiPt.getInclination());
            enginePt.setBank(uiPt.getBank());
            enginePt.setScaleWidth(uiPt.getScaleWidth());
            enginePt.setScaleHeight(uiPt.getScaleHeight());

            // NEW: Synchronize the Bezier tangent lengths to perfectly match the UI!
            enginePt.setNextControlPointDistanceFactor(uiPt.getNextControlPointDistanceFactor());
            enginePt.setPreviousControlPointDistanceFactor(uiPt.getPreviousControlPointDistanceFactor());
        }

        // --- ZERO DISK I/O: DIRECT MESH GENERATION ---
        std::vector<Vertex> trackVertices;
        std::vector<unsigned> trackIndices;
        track.fillBuffers(trackVertices, trackIndices);
        Mesh mesh = { 0 };
        
        if (trackVertices.empty() || trackIndices.empty()) {
            throw std::runtime_error("Track generator returned 0 vertices or faces. The .tracktemplate format might be invalid (e.g. expecting Quads 'q' instead of 'f', or unrecognized tokens).");
        }

        // We unroll the indexed mesh into a flat unindexed mesh to completely bypass 
        // the 16-bit unsigned short limit of Raylib's indices array for very large tracks.
        mesh.vertexCount = static_cast<int>(trackIndices.size());
        mesh.triangleCount = static_cast<int>(trackIndices.size() / 3);
        
        mesh.vertices = (float *)MemAlloc(mesh.vertexCount * 3 * sizeof(float));
        mesh.normals = (float *)MemAlloc(mesh.vertexCount * 3 * sizeof(float));
        mesh.texcoords = (float *)MemAlloc(mesh.vertexCount * 2 * sizeof(float));
        mesh.indices = nullptr; // Leave unindexed
        
        for (size_t i = 0; i < trackIndices.size(); i++) {
            unsigned int idx = trackIndices[i];
            
            mesh.vertices[i*3 + 0] = trackVertices[idx].position[0];
            mesh.vertices[i*3 + 1] = trackVertices[idx].position[1];
            mesh.vertices[i*3 + 2] = trackVertices[idx].position[2];

            mesh.normals[i*3 + 0] = trackVertices[idx].normal[0];
            mesh.normals[i*3 + 1] = trackVertices[idx].normal[1];
            mesh.normals[i*3 + 2] = trackVertices[idx].normal[2];

            mesh.texcoords[i*2 + 0] = trackVertices[idx].texCoord[0] * globalUvScale[0];
            
            // Raylib's internal LoadOBJ previously flipped the V coordinate automatically. 
            // We replicate that flip here (1.0f - V) to keep textures rendering correctly.
            mesh.texcoords[i*2 + 1] = (1.0f - trackVertices[idx].texCoord[1]) * globalUvScale[1];
        }
        
        if (currentTrackModel.meshCount > 0) {
            UnloadModel(currentTrackModel);
        }

        UploadMesh(&mesh, false); // Sends the raw arrays directly to VRAM
        currentTrackModel = LoadModelFromMesh(mesh);
        
        if (currentTrackModel.meshCount > 0) {
            if (currentTrackTexture.id != 0 && showTexture) {
                currentTrackModel.materials[0].maps[MATERIAL_MAP_DIFFUSE].texture = currentTrackTexture;
                currentTrackModel.materials[0].maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
            } else {
                currentTrackModel.materials[0].maps[MATERIAL_MAP_DIFFUSE].color = LIGHTGRAY;
            }
        }

        TraceLog(LOG_INFO, "In-memory mesh rebuild successful!");
    }
    catch (const std::exception& e) {
        lastMeshError = e.what();
        TraceLog(LOG_ERROR, "Failed to rebuild mesh: %s", e.what());
    }
}


// --- FILE SAVING LOGIC ---
void SaveTrackToFile(const std::string& path) {
    std::ofstream trackFile(path);
    if (!trackFile.is_open()) {
        TraceLog(LOG_ERROR, "Failed to open file for saving: %s", path.c_str());
        return;
    }

    for (const AnchorPoint& pt : trackPoints) {
        float px, py, pz;
        pt.getPosition(px, py, pz);

        // Write exactly as the original Mac version expects:
        // p X Y Z   Dir(Deg) Inc(Deg) Bank(Deg)   ScaleW ScaleH
        trackFile << "p " << px << " " << py << " " << pz << " "
            << (pt.getDirection() * RAD2DEG) << " "
            << (pt.getInclination() * RAD2DEG) << " "
            << (pt.getBank() * RAD2DEG) << " "
            << pt.getScaleWidth() << " " << pt.getScaleHeight() << "\n";
    }
    trackFile.close();
    TraceLog(LOG_INFO, "Track successfully saved to: %s", path.c_str());
}


// --- FILE LOADING LOGIC ---
void LoadTrackFromFile(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        TraceLog(LOG_ERROR, "Failed to open track file for reading: %s", path.c_str());
        return;
    }

    std::vector<AnchorPoint> loadedPoints;
    std::string line;

    // Parse the file line by line
    while (std::getline(file, line)) {
        if (line.empty()) continue;

        std::istringstream iss(line);
        char type;
        iss >> type;

        if (type == 'p') {
            AnchorPoint newPoint;
            float px, py, pz, dirDeg, incDeg, bankDeg, sw, sh;

            // Read all 8 values from the exact same line
            if (iss >> px >> py >> pz >> dirDeg >> incDeg >> bankDeg >> sw >> sh) {

                newPoint.setPosition(px, py, pz);

                // Convert the degrees from the text file into radians for the C++ engine
                newPoint.setDirection(dirDeg * DEG2RAD);
                newPoint.setInclination(incDeg * DEG2RAD);
                newPoint.setBank(bankDeg * DEG2RAD);

                newPoint.setScaleWidth(sw);
                newPoint.setScaleHeight(sh);

                // NEW: Force the legacy Mac engine default (0.5) for missing tangent data
                newPoint.setNextControlPointDistanceFactor(0.5f);
                newPoint.setPreviousControlPointDistanceFactor(0.5f);

                loadedPoints.push_back(newPoint);
            }
        }
    }
    file.close();

    // If we successfully loaded points, overwrite the current editor state
    if (!loadedPoints.empty()) {
        trackPoints = loadedPoints;
        selectedPointIndex = 0;
        selectedPointIndices.clear();
        selectedPointIndices.push_back(0);
        needsMeshRebuild = true;
        
        undoHistory.clear();
        historyIndex = -1;
        RecordState();
        
        TraceLog(LOG_INFO, "Track successfully loaded: %s", path.c_str());
    }
}


// --- OBJ EXPORT LOGIC ---
void ExportTrackToOBJ(const std::string& path) {
    if (trackPoints.size() < 2 || currentTemplatePath.empty()) {
        TraceLog(LOG_WARNING, "Cannot export: Track needs at least 2 points and a valid template.");
        return;
    }

    try {
        // Rebuild the engine track cleanly for export
        Template trackTemplate(currentTemplatePath.c_str());
        Track track(trackTemplate);

        for (const AnchorPoint& uiPt : trackPoints) {
            AnchorPoint& enginePt = track.createAnchorPointAtEnd();
            
            float px, py, pz;
            uiPt.getPosition(px, py, pz);
            
            enginePt.setPosition(px, py, pz);
            enginePt.setDirection(uiPt.getDirection());
            enginePt.setInclination(uiPt.getInclination());
            enginePt.setBank(uiPt.getBank());
            enginePt.setScaleWidth(uiPt.getScaleWidth());
            enginePt.setScaleHeight(uiPt.getScaleHeight());
            enginePt.setNextControlPointDistanceFactor(uiPt.getNextControlPointDistanceFactor());
            enginePt.setPreviousControlPointDistanceFactor(uiPt.getPreviousControlPointDistanceFactor());
        }

        // Export directly to the user's chosen path
        FileWriter writer(track);
        writer.write(OBJ, path.c_str());
        
        TraceLog(LOG_INFO, "OBJ Export successful: %s", path.c_str());

    } catch (const std::exception& e) {
        TraceLog(LOG_ERROR, "Failed to export OBJ: %s", e.what());
    }
}


// --- AI WAYPOINT EXPORT LOGIC ---
void ExportTrackToJSON(const std::string& path) {
    if (trackPoints.size() < 2) {
        TraceLog(LOG_WARNING, "Cannot export JSON: Track needs at least 2 points.");
        return;
    }

    std::ofstream outFile(path);
    if (!outFile.is_open()) {
        TraceLog(LOG_ERROR, "Failed to open file for saving JSON: %s", path.c_str());
        return;
    }

    outFile << "{\n";
    outFile << "  \"waypoints\": [\n";

    bool first = true;
    int waypointIndex = 0;
    const int steps = 20; // Resolution: 20 dense waypoints per Anchor Segment

    for (size_t i = 0; i < trackPoints.size() - 1; i++) {
        AnchorPoint& pt = trackPoints[i];
        AnchorPoint& nextPt = trackPoints[i + 1];

        float px, py, pz; pt.getPosition(px, py, pz);
        Vector3 pos = { px, py, pz };

        float nx, ny, nz; nextPt.getPosition(nx, ny, nz);
        Vector3 nextPos = { nx, ny, nz };

        matrix m1 = pt.getMatrix();
        Vector3 forward = { m1.x.x, m1.x.y, m1.x.z };
        
        matrix m2 = nextPt.getMatrix();
        Vector3 backward = { -m2.x.x, -m2.x.y, -m2.x.z };

        float length = Vector3Distance(pos, nextPos);

        Vector3 p1 = Vector3Add(pos, Vector3Scale(forward, length * pt.getNextControlPointDistanceFactor()));
        Vector3 p2 = Vector3Add(nextPos, Vector3Scale(backward, length * nextPt.getPreviousControlPointDistanceFactor()));

        // We don't include the last point of the segment (step == steps) to avoid duplicates with the next segment
        for (int step = 0; step < steps; step++) {
            float t = (float)step / (float)steps;
            
            // Evaluate slightly ahead to calculate the exact true forward vector
            float tNext = (float)(step + 1) / (float)steps;

            Vector3 currentCurvePt = EvaluateCubicBezier(pos, p1, p2, nextPos, t);
            Vector3 nextCurvePt = EvaluateCubicBezier(pos, p1, p2, nextPos, tNext);
            
            Vector3 dir = Vector3Normalize(Vector3Subtract(nextCurvePt, currentCurvePt));

            if (!first) outFile << ",\n";
            outFile << "    {\n";
            outFile << "      \"id\": " << waypointIndex++ << ",\n";
            outFile << "      \"position\": { \"x\": " << currentCurvePt.x << ", \"y\": " << currentCurvePt.y << ", \"z\": " << currentCurvePt.z << " },\n";
            outFile << "      \"forward\": { \"x\": " << dir.x << ", \"y\": " << dir.y << ", \"z\": " << dir.z << " }\n";
            outFile << "    }";
            first = false;
        }
    }

    // Add the very last Anchor Point to close the file
    AnchorPoint& lastPt = trackPoints.back();
    float lx, ly, lz; lastPt.getPosition(lx, ly, lz);
    matrix m_last = lastPt.getMatrix();
    
    if (!first) outFile << ",\n";
    outFile << "    {\n";
    outFile << "      \"id\": " << waypointIndex++ << ",\n";
    outFile << "      \"position\": { \"x\": " << lx << ", \"y\": " << ly << ", \"z\": " << lz << " },\n";
    outFile << "      \"forward\": { \"x\": " << m_last.x.x << ", \"y\": " << m_last.x.y << ", \"z\": " << m_last.x.z << " }\n";
    outFile << "    }\n";

    outFile << "  ]\n";
    outFile << "}\n";
    
    outFile.close();
    TraceLog(LOG_INFO, "AI Waypoints (JSON) exported successfully to: %s", path.c_str());
}


// --- FEATURE: SUBDIVIDE SPLINE ---
// Inserts a new node exactly halfway along the curve between two selected nodes
void SubdivideSegment(size_t segmentStartIndex) {
    if (segmentStartIndex >= trackPoints.size() - 1) return;

    AnchorPoint& p0 = trackPoints[segmentStartIndex];
    AnchorPoint& p1 = trackPoints[segmentStartIndex + 1];

    // Get positions
    float pos0[3], pos1[3];
    p0.getPosition(pos0[0], pos0[1], pos0[2]);
    p1.getPosition(pos1[0], pos1[1], pos1[2]);
    Vector3 v0 = { pos0[0], pos0[1], pos0[2] };
    Vector3 v1 = { pos1[0], pos1[1], pos1[2] };

    // Calculate forward vectors
    matrix m0 = p0.getMatrix();
    matrix m1 = p1.getMatrix();
    Vector3 fwd0 = { m0.x.x, m0.x.y, m0.x.z };
    Vector3 bwd1 = { -m1.x.x, -m1.x.y, -m1.x.z };

    // Find control points based on distance factors
    float length = Vector3Distance(v0, v1);
    Vector3 ctrl1 = Vector3Add(v0, Vector3Scale(fwd0, length * p0.getNextControlPointDistanceFactor()));
    Vector3 ctrl2 = Vector3Add(v1, Vector3Scale(bwd1, length * p1.getPreviousControlPointDistanceFactor()));

    // De Casteljau's algorithm at t = 0.5
    Vector3 P0 = v0;
    Vector3 P1 = ctrl1;
    Vector3 P2 = ctrl2;
    Vector3 P3 = v1;

    Vector3 P01 = Vector3Lerp(P0, P1, 0.5f);
    Vector3 P12 = Vector3Lerp(P1, P2, 0.5f);
    Vector3 P23 = Vector3Lerp(P2, P3, 0.5f);

    Vector3 P012 = Vector3Lerp(P01, P12, 0.5f);
    Vector3 P123 = Vector3Lerp(P12, P23, 0.5f);

    Vector3 P0123 = Vector3Lerp(P012, P123, 0.5f); // Exactly the midpoint on the curve
    Vector3 midPos = P0123;

    // Create the new intermediate point
    AnchorPoint midPt;
    midPt.setPosition(midPos.x, midPos.y, midPos.z);
    
    // Linearly interpolate other properties (banking, scale, etc.)
    midPt.setBank((p0.getBank() + p1.getBank()) * 0.5f);
    midPt.setScaleWidth((p0.getScaleWidth() + p1.getScaleWidth()) * 0.5f);
    midPt.setScaleHeight((p0.getScaleHeight() + p1.getScaleHeight()) * 0.5f);
    
    // Evaluate exact tangent at t = 0.5 to set the new rotation
    Vector3 tangent = Vector3Subtract(P123, P012);
    Vector3 forwardDir = Vector3Normalize(tangent);
    
    float newDir = atan2f(-forwardDir.z, forwardDir.x);
    float yClamp = forwardDir.y;
    if (yClamp > 1.0f) yClamp = 1.0f;
    if (yClamp < -1.0f) yClamp = -1.0f;
    float newInc = asinf(yClamp);
    midPt.setDirection(newDir);
    midPt.setInclination(newInc);

    // Calculate exact Bezier tangent factors for the new subdivision to preserve the curve shape
    float L_left = Vector3Distance(P0, midPos);
    float L_right = Vector3Distance(midPos, P3);
    float tangent_len = Vector3Distance(P012, P123);

    if (L_left > 0.001f) {
        p0.setNextControlPointDistanceFactor((length / (2.0f * L_left)) * p0.getNextControlPointDistanceFactor());
        midPt.setPreviousControlPointDistanceFactor(tangent_len / (2.0f * L_left));
    } else {
        p0.setNextControlPointDistanceFactor(0.5f);
        midPt.setPreviousControlPointDistanceFactor(0.5f);
    }

    if (L_right > 0.001f) {
        midPt.setNextControlPointDistanceFactor(tangent_len / (2.0f * L_right));
        p1.setPreviousControlPointDistanceFactor((length / (2.0f * L_right)) * p1.getPreviousControlPointDistanceFactor());
    } else {
        midPt.setNextControlPointDistanceFactor(0.5f);
        p1.setPreviousControlPointDistanceFactor(0.5f);
    }

    // Insert into vector
    trackPoints.insert(trackPoints.begin() + segmentStartIndex + 1, midPt);
    
    TraceLog(LOG_INFO, "Spline subdivided at index %zu", segmentStartIndex);
    
    needsMeshRebuild = true;
    RecordState();
}

// --- FEATURE: SIMPLIFY TRACK ---
// Removes redundant nodes that form a perfectly straight line with no property variations
void SimplifyTrack() {
    if (trackPoints.size() < 3) return;

    size_t initialSize = trackPoints.size();
    std::vector<size_t> nodesToRemove;

    // We skip the first and last nodes to preserve the end caps
    for (size_t i = 1; i < trackPoints.size() - 1; i++) {
        AnchorPoint& pPrev = trackPoints[i - 1];
        AnchorPoint& pCurr = trackPoints[i];
        AnchorPoint& pNext = trackPoints[i + 1];

        float posPrev[3], posCurr[3], posNext[3];
        pPrev.getPosition(posPrev[0], posPrev[1], posPrev[2]);
        pCurr.getPosition(posCurr[0], posCurr[1], posCurr[2]);
        pNext.getPosition(posNext[0], posNext[1], posNext[2]);

        Vector3 vPrev = { posPrev[0], posPrev[1], posPrev[2] };
        Vector3 vCurr = { posCurr[0], posCurr[1], posCurr[2] };
        Vector3 vNext = { posNext[0], posNext[1], posNext[2] };

        // Calculate direction vectors between the three nodes
        Vector3 dir1 = Vector3Normalize(Vector3Subtract(vCurr, vPrev));
        Vector3 dir2 = Vector3Normalize(Vector3Subtract(vNext, vCurr));

        // Check collinearity (1.0 means perfectly straight)
        float dot = Vector3DotProduct(dir1, dir2);

        // Ensure all visual properties are practically identical
        bool propertiesMatch = (fabsf(pPrev.getBank() - pCurr.getBank()) < 0.01f) &&
                               (fabsf(pCurr.getBank() - pNext.getBank()) < 0.01f) &&
                               (fabsf(pPrev.getScaleWidth() - pCurr.getScaleWidth()) < 0.01f) &&
                               (fabsf(pCurr.getScaleWidth() - pNext.getScaleWidth()) < 0.01f) &&
                               (fabsf(pPrev.getScaleHeight() - pCurr.getScaleHeight()) < 0.01f) &&
                               (fabsf(pCurr.getScaleHeight() - pNext.getScaleHeight()) < 0.01f);

        // If the path is straight and properties didn't change, the node is redundant
        if (dot > 0.999f && propertiesMatch) {
            nodesToRemove.push_back(i);
        }
    }

    // Erase from back to front to avoid invalidating indices
    for (auto it = nodesToRemove.rbegin(); it != nodesToRemove.rend(); ++it) {
        trackPoints.erase(trackPoints.begin() + *it);
    }

    if (!nodesToRemove.empty()) {
        selectedPointIndices.clear();
        selectedPointIndex = NO_SELECTION;
        needsMeshRebuild = true;
        RecordState();
        TraceLog(LOG_INFO, "Track simplified. Removed %zu redundant nodes.", initialSize - trackPoints.size());
    } else {
        TraceLog(LOG_INFO, "Track simplified. No redundant nodes found.");
    }
}


// --- GUI RENDERING ---
void RenderTrackmakerGUI(Camera3D& camera) {
    // Initialize default point if empty
    if (trackPoints.empty()) {
        AnchorPoint initialPoint;
        initialPoint.setPosition(0.0f, 0.0f, 0.0f);

        // NEW: Ensure brand new tracks also use the Mac engine default
        initialPoint.setNextControlPointDistanceFactor(0.5f);
        initialPoint.setPreviousControlPointDistanceFactor(0.5f);

        trackPoints.push_back(initialPoint);
        selectedPointIndex = 0;
        selectedPointIndices.clear();
        selectedPointIndices.push_back(0);
        
        undoHistory.clear();
        historyIndex = -1;
        RecordState();
    }

    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(400, 720), ImGuiCond_FirstUseEver);

    ImGui::Begin("Trackmaker Inspector", nullptr, ImGuiWindowFlags_MenuBar);

    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("New Track")) {
                trackPoints.clear();
                if (currentTrackModel.meshCount > 0) UnloadModel(currentTrackModel);
                currentTrackModel = { 0 };
                
                undoHistory.clear();
                historyIndex = -1;
            }

            // NEW: The Open Button
            if (ImGui::MenuItem("Open Track...")) {
                std::string openPath = OSUtils::OpenTrackFileDialog();
                if (!openPath.empty()) {
                    LoadTrackFromFile(openPath);
                }
            }

            // NEW: The Save As Button
            if (ImGui::MenuItem("Save Track As...")) {
                std::string savePath = OSUtils::SaveTrackFileDialog();
                if (!savePath.empty()) {
                    SaveTrackToFile(savePath);
                }
            }

            ImGui::Separator();
            
            // NEW: The Final Export Button
            if (ImGui::MenuItem("Export to OBJ...")) {
                std::string exportPath = OSUtils::SaveObjFileDialog();
                if (!exportPath.empty()) {
                    ExportTrackToOBJ(exportPath);
                }
            }

            // NEW: Export AI Waypoints JSON
            if (ImGui::MenuItem("Export AI Waypoints (JSON)...")) {
                std::string exportPath = OSUtils::SaveJsonFileDialog();
                if (!exportPath.empty()) {
                    ExportTrackToJSON(exportPath);
                }
            }

            ImGui::Separator();
            if (ImGui::MenuItem("Exit")) { CloseWindow(); }
            ImGui::EndMenu();
        }
        
        if (ImGui::BeginMenu("Edit")) {
            if (ImGui::MenuItem("Undo", "Ctrl+Z", false, historyIndex > 0)) {
                Undo();
            }
            if (ImGui::MenuItem("Redo", "Ctrl+Y", false, historyIndex < static_cast<int>(undoHistory.size()) - 1)) {
                Redo();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Simplify Track")) {
                SimplifyTrack();
            }
            ImGui::EndMenu();
        }
        
        if (ImGui::BeginMenu("Options")) {
            ImGui::MenuItem("Real-time 3D Mesh Updates", nullptr, &realtimeMeshRebuild);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Rebuilds the solid 3D mesh continuously while dragging.\nMay cause lag on large tracks.");
            ImGui::EndMenu();
        }
        
        ImGui::EndMenuBar();
    }

    // Template Loading Section
    ImGui::Text("Template File:");
    ImGui::TextDisabled("%s", currentTemplatePath.empty() ? "None loaded" : currentTemplatePath.c_str());
    if (ImGui::Button("Load .tracktemplate...", ImVec2(-FLT_MIN, 0))) {
        std::string path = OSUtils::OpenTrackTemplateDialog();
        if (!path.empty()) {
            currentTemplatePath = path;
            needsMeshRebuild = true;
        }
    }

    if (!lastMeshError.empty()) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
        ImGui::TextWrapped("Template Error:\n%s", lastMeshError.c_str());
        ImGui::PopStyleColor();
    }

    ImGui::Separator();

    // Anchor Points List
    ImGui::Text("Anchor Points");
    if (ImGui::BeginListBox("##PointsList", ImVec2(-FLT_MIN, 150))) {
        for (size_t i = 0; i < trackPoints.size(); i++) {
            std::string label = "Point " + std::to_string(i);
            bool is_selected = std::find(selectedPointIndices.begin(), selectedPointIndices.end(), i) != selectedPointIndices.end();
            
            if (ImGui::Selectable(label.c_str(), is_selected, ImGuiSelectableFlags_AllowDoubleClick)) {
                if (ImGui::GetIO().KeyCtrl) {
                    if (is_selected) {
                        selectedPointIndices.erase(std::remove(selectedPointIndices.begin(), selectedPointIndices.end(), i), selectedPointIndices.end());
                        if (selectedPointIndex == i) selectedPointIndex = selectedPointIndices.empty() ? NO_SELECTION : selectedPointIndices.back();
                    } else {
                        selectedPointIndices.push_back(i);
                        selectedPointIndex = i;
                    }
                } else {
                    selectedPointIndices.clear();
                    selectedPointIndices.push_back(i);
                    selectedPointIndex = i;
                }

                if (ImGui::IsMouseDoubleClicked(0)) {
                    float px, py, pz;
                    trackPoints[i].getPosition(px, py, pz);
                    Vector3 newTarget = { px, py, pz };
                    
                    // Keep the current camera distance and angle relative to the new target
                    Vector3 offset = Vector3Subtract(camera.position, camera.target);
                    camera.target = newTarget;
                    camera.position = Vector3Add(camera.target, offset);
                }
            }
        }
        ImGui::EndListBox();
    }

    if (ImGui::Button("Add Point", ImVec2(100, 0))) {
        AnchorPoint newPt = trackPoints.back();
        float px, py, pz;
        newPt.getPosition(px, py, pz);
        newPt.setPosition(px + 50.0f, py, pz); // Offset to make it visible
        trackPoints.push_back(newPt);
        selectedPointIndex = trackPoints.size() - 1ull;
        selectedPointIndices.clear();
        selectedPointIndices.push_back(selectedPointIndex);
        needsMeshRebuild = true;
        RecordState();
    }
    ImGui::SameLine();
    if (ImGui::Button("Remove", ImVec2(100, 0))) {
        if (!selectedPointIndices.empty() && trackPoints.size() > 1) {
            size_t numToDelete = std::min(selectedPointIndices.size(), trackPoints.size() - 1);
            std::vector<size_t> indicesToDelete = selectedPointIndices;
            indicesToDelete.resize(numToDelete);
            std::sort(indicesToDelete.rbegin(), indicesToDelete.rend());
            
            for (size_t idx : indicesToDelete) {
                if (idx < trackPoints.size()) trackPoints.erase(trackPoints.begin() + idx);
            }
            selectedPointIndices.clear();
            selectedPointIndex = NO_SELECTION;
            
            if (!trackPoints.empty()) {
                selectedPointIndex = trackPoints.size() - 1;
                selectedPointIndices.push_back(selectedPointIndex);
            }
            needsMeshRebuild = true;
            RecordState();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Subdivide", ImVec2(80, 0))) {
        if (selectedPointIndex != NO_SELECTION && selectedPointIndex < trackPoints.size() - 1) {
            SubdivideSegment(selectedPointIndex);
            // Auto-select the newly created point
            selectedPointIndex++;
            selectedPointIndices.clear();
            selectedPointIndices.push_back(selectedPointIndex);
        } else {
            TraceLog(LOG_WARNING, "Cannot subdivide: Select a valid point that is not the very last one.");
        }
    }

    ImGui::Separator();

    // Gizmo Tools Section
    ImGui::Text("Gizmo Tools");
    if (ImGui::RadioButton("Translate (W)", currentGizmoOperation == ImGuizmo::TRANSLATE)) currentGizmoOperation = ImGuizmo::TRANSLATE; ImGui::SameLine();
    if (ImGui::RadioButton("Rotate (E)", currentGizmoOperation == ImGuizmo::ROTATE)) currentGizmoOperation = ImGuizmo::ROTATE; ImGui::SameLine();
    if (ImGui::RadioButton("Scale (R)", currentGizmoOperation == ImGuizmo::SCALE)) currentGizmoOperation = ImGuizmo::SCALE;
    if (ImGui::RadioButton("Local (Q)", currentGizmoMode == ImGuizmo::LOCAL)) currentGizmoMode = ImGuizmo::LOCAL; ImGui::SameLine();
    if (ImGui::RadioButton("World (Q)", currentGizmoMode == ImGuizmo::WORLD)) currentGizmoMode = ImGuizmo::WORLD;
    
    ImGui::Spacing();
    ImGui::Checkbox("Enable Snapping (Hold SHIFT)", &useSnap);
    ImGui::SameLine();
    ImGui::Checkbox("Magnetic Ground (Y=0)", &snapToGround);
    if (useSnap) {
        ImGui::SetNextItemWidth(100);
        ImGui::DragFloat("Pos Step", &snapTranslation, 0.1f, 0.1f, 100.0f, "%.1fm");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100);
        ImGui::DragFloat("Rot Step", &snapRotation, 1.0f, 1.0f, 180.0f, "%.0f°");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100);
        ImGui::DragFloat("Scl Step", &snapScale, 0.05f, 0.05f, 10.0f, "%.2fx");
    }
    
    ImGui::Separator();

    // Appearance & Texture Section
    ImGui::Text("Appearance");
    if (ImGui::Button("Load Texture...", ImVec2(-FLT_MIN, 0))) {
        std::string imgPath = OSUtils::OpenImageFileDialog();
        if (!imgPath.empty()) {
            if (currentTrackTexture.id != 0) UnloadTexture(currentTrackTexture);
            currentTrackTexture = LoadTexture(imgPath.c_str());
            needsMeshRebuild = true;
        }
    }
    if (ImGui::Checkbox("Show Texture Preview", &showTexture)) {
        needsMeshRebuild = true;
    }
    if (ImGui::DragFloat2("Global UV Scale", globalUvScale, 0.05f, 0.01f, 100.0f)) {
        // UV Scale is applied during mesh generation
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) { needsMeshRebuild = true; }
    ImGui::Separator();

    // Visibility Section
    ImGui::Text("Visibility");
    ImGui::Checkbox("Solid Mesh", &showSolidMesh); ImGui::SameLine();
    ImGui::Checkbox("Wireframe", &showWireframe);
    ImGui::Checkbox("Track Nodes", &showTrackNodes); ImGui::SameLine();
    ImGui::Checkbox("Track Curve", &showTrackCurve);
    ImGui::Separator();

    // Camera Tools Section
    ImGui::Text("Camera Controls");
    if (ImGui::Button("Reset View")) {
        camera.position = Vector3{ 800.0f, 500.0f, 800.0f };
        camera.target = Vector3{ 200.0f, 150.0f, -250.0f };
        camera.up = Vector3{ 0.0f, 1.0f, 0.0f };
        camera.fovy = 45.0f;
        camera.projection = CAMERA_PERSPECTIVE;
    }
    ImGui::SameLine();
    bool isOrtho = (camera.projection == CAMERA_ORTHOGRAPHIC);
    if (ImGui::Checkbox("Orthographic", &isOrtho)) {
        camera.projection = isOrtho ? CAMERA_ORTHOGRAPHIC : CAMERA_PERSPECTIVE;
        if (isOrtho) camera.fovy = Vector3Distance(camera.position, camera.target) * 0.8f; 
        else camera.fovy = 45.0f;
    }
    ImGui::Separator();

// Inspector
    if (selectedPointIndex != NO_SELECTION && selectedPointIndex < trackPoints.size()) {
        AnchorPoint& pt = trackPoints[selectedPointIndex];
        
        float px, py, pz;
        pt.getPosition(px, py, pz);
        float pos[3] = { px, py, pz };
        float dir = pt.getDirection() * RAD2DEG;
        float inc = pt.getInclination() * RAD2DEG;
        float bnk = pt.getBank() * RAD2DEG;
        float sw = pt.getScaleWidth();
        float sh = pt.getScaleHeight();
        float prevFac = pt.getPreviousControlPointDistanceFactor();
        float nextFac = pt.getNextControlPointDistanceFactor();

        float oldPos[3] = { pos[0], pos[1], pos[2] };
        float oldDir = dir;
        float oldInc = inc;
        float oldBank = bnk;
        float oldSW = sw;
        float oldSH = sh;
        float oldPrevFac = prevFac;
        float oldNextFac = nextFac;
        
        // Calculate dynamic drag speeds based on snap settings
        float posSpeed = (useSnap && snapTranslation >= 0.1f) ? snapTranslation : 1.0f;
        float rotSpeed = (useSnap && snapRotation >= 1.0f) ? snapRotation : 1.0f;
        float sclSpeed = (useSnap && snapScale >= 0.05f) ? snapScale : 0.05f;
        bool modPos = false, modRot = false, modScale = false, modTan = false;

        // Helper lambda for UI Reset Buttons
        auto ResetButton = [](const char* id, float& val, float resetTo = 0.0f) {
            ImGui::SameLine();
            if (ImGui::Button(id)) {
                val = resetTo;
                return true;
            }
            return false;
        };

        ImGui::Text("Transform");
        if (ImGui::DragFloat3("Position", pos, posSpeed)) {
            if (useSnap) {
                pos[0] = roundf(pos[0] / snapTranslation) * snapTranslation;
                pos[1] = roundf(pos[1] / snapTranslation) * snapTranslation;
                pos[2] = roundf(pos[2] / snapTranslation) * snapTranslation;
            }
            if (snapToGround && fabsf(pos[1]) < 5.0f) {
                pos[1] = 0.0f;
            }
            pt.setPosition(pos[0], pos[1], pos[2]);
            modPos = true;
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) { needsMeshRebuild = true; RecordState(); }

        ImGui::Spacing();
        ImGui::Text("Angles (Degrees)");
        if (ImGui::DragFloat("Direction", &dir, rotSpeed, -360.0f, 360.0f)) {
            if (useSnap) dir = roundf(dir / snapRotation) * snapRotation;
            pt.setDirection(dir * DEG2RAD);
            modRot = true;
        }
        if (ResetButton("0##dir", dir, 0.0f)) { pt.setDirection(0.0f); modRot = true; }
        if (ImGui::IsItemDeactivatedAfterEdit()) { needsMeshRebuild = true; RecordState(); }
        
        if (ImGui::DragFloat("Inclination", &inc, rotSpeed, -90.0f, 90.0f)) {
            if (useSnap) inc = roundf(inc / snapRotation) * snapRotation;
            pt.setInclination(inc * DEG2RAD);
            modRot = true;
        }
        if (ResetButton("0##inc", inc, 0.0f)) { pt.setInclination(0.0f); modRot = true; }
        if (ImGui::IsItemDeactivatedAfterEdit()) { needsMeshRebuild = true; RecordState(); }
        
        if (ImGui::DragFloat("Bank (Roll)", &bnk, rotSpeed, -180.0f, 180.0f)) {
            if (useSnap) bnk = roundf(bnk / snapRotation) * snapRotation;
            pt.setBank(bnk * DEG2RAD);
            modRot = true;
        }
        if (ResetButton("0##bnk", bnk, 0.0f)) { pt.setBank(0.0f); modRot = true; }
        if (ImGui::IsItemDeactivatedAfterEdit()) { needsMeshRebuild = true; RecordState(); }

        ImGui::Spacing();
        ImGui::Text("Scale");
        if (ImGui::DragFloat("Scale W", &sw, sclSpeed, 0.1f, 10.0f)) {
            if (useSnap) sw = roundf(sw / snapScale) * snapScale;
            pt.setScaleWidth(sw);
            modScale = true;
        }
        if (ResetButton("1##sw", sw, 1.0f)) { pt.setScaleWidth(1.0f); modScale = true; }
        if (ImGui::IsItemDeactivatedAfterEdit()) { needsMeshRebuild = true; RecordState(); }
        
        if (ImGui::DragFloat("Scale H", &sh, sclSpeed, 0.1f, 10.0f)) {
            if (useSnap) sh = roundf(sh / snapScale) * snapScale;
            pt.setScaleHeight(sh);
            modScale = true;
        }
        if (ResetButton("1##sh", sh, 1.0f)) { pt.setScaleHeight(1.0f); modScale = true; }
        if (ImGui::IsItemDeactivatedAfterEdit()) { needsMeshRebuild = true; RecordState(); }

        // NEW: Bezier Tangent Controls
        ImGui::Spacing();
        ImGui::Text("Bezier Tangents (Curve Force)");

        if (ImGui::DragFloat("Prev Tangent Length", &prevFac, 0.01f, 0.0f, 2.0f)) {
            pt.setPreviousControlPointDistanceFactor(prevFac);
            modTan = true;
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) { needsMeshRebuild = true; RecordState(); }

        if (ImGui::DragFloat("Next Tangent Length", &nextFac, 0.01f, 0.0f, 2.0f)) {
            pt.setNextControlPointDistanceFactor(nextFac);
            modTan = true;
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) { needsMeshRebuild = true; RecordState(); }
        
        // Apply deltas to other selected nodes
        if (modPos || modRot || modScale || modTan) {
            if (realtimeMeshRebuild) needsMeshRebuild = true;
            
            for (size_t idx : selectedPointIndices) {
                if (idx == selectedPointIndex || idx >= trackPoints.size()) continue;
                AnchorPoint& op = trackPoints[idx];
                if (modPos) { float ox, oy, oz; op.getPosition(ox, oy, oz); op.setPosition(ox + (pos[0]-oldPos[0]), oy + (pos[1]-oldPos[1]), oz + (pos[2]-oldPos[2])); }
                if (modRot) { op.setDirection(op.getDirection() + (dir-oldDir)*DEG2RAD); op.setInclination(op.getInclination() + (inc-oldInc)*DEG2RAD); op.setBank(op.getBank() + (bnk-oldBank)*DEG2RAD); }
                if (modScale) { float nsw = op.getScaleWidth()+(sw-oldSW), nsh = op.getScaleHeight()+(sh-oldSH); op.setScaleWidth(nsw<0.01f?0.01f:nsw); op.setScaleHeight(nsh<0.01f?0.01f:nsh); }
                if (modTan) { op.setPreviousControlPointDistanceFactor(op.getPreviousControlPointDistanceFactor() + (prevFac-oldPrevFac)); op.setNextControlPointDistanceFactor(op.getNextControlPointDistanceFactor() + (nextFac-oldNextFac)); }
            }
        }
    }

    ImGui::End();
}


// --- GIZMO RENDERING & LOGIC ---
void DrawGizmo(Matrix viewMat, Matrix projMat, bool isOrthographic) {
    if (selectedPointIndex == NO_SELECTION || selectedPointIndex >= trackPoints.size()) return;

    ImGuiIO& io = ImGui::GetIO();
    
    // Force ImGuizmo to draw into the background of the screen, not inside a floating window
    ImGuizmo::SetDrawlist(ImGui::GetBackgroundDrawList());
    ImGuizmo::SetOrthographic(isOrthographic);
    ImGuizmo::SetRect(0, 0, io.DisplaySize.x, io.DisplaySize.y);

    AnchorPoint& pt = trackPoints[selectedPointIndex];
    float px, py, pz;
    pt.getPosition(px, py, pz);

    // Convert Raylib's Row-Major struct to Column-Major float arrays for ImGuizmo
    float view[16] = {
        viewMat.m0, viewMat.m1, viewMat.m2, viewMat.m3,
        viewMat.m4, viewMat.m5, viewMat.m6, viewMat.m7,
        viewMat.m8, viewMat.m9, viewMat.m10, viewMat.m11,
        viewMat.m12, viewMat.m13, viewMat.m14, viewMat.m15
    };

    float proj[16] = {
        projMat.m0, projMat.m1, projMat.m2, projMat.m3,
        projMat.m4, projMat.m5, projMat.m6, projMat.m7,
        projMat.m8, projMat.m9, projMat.m10, projMat.m11,
        projMat.m12, projMat.m13, projMat.m14, projMat.m15
    };

    matrix rotMat = pt.getMatrix();
    float sw = pt.getScaleWidth();
    float sh = pt.getScaleHeight();

    // Build the matrix for ImGuizmo.
    // Trackmaker uses a Right-Handed system: X = Forward, Y = Up, Z = Right
    // The 3D visual standard expects: X (Red) = Right, Y (Green) = Up.
    // To keep the matrix Right-Handed and prevent mouse controls from inverting,
    // the Gizmo's Z axis (Blue) MUST point Backward (-Forward).
    float gizmoMatrix[16] = {
        rotMat.z.x * sw, rotMat.z.y * sw, rotMat.z.z * sw, 0.0f, // Gizmo X (Red)   = Right
        rotMat.y.x * sh, rotMat.y.y * sh, rotMat.y.z * sh, 0.0f, // Gizmo Y (Green) = Up
       -rotMat.x.x,     -rotMat.x.y,     -rotMat.x.z,      0.0f, // Gizmo Z (Blue)  = Backward
        px,              py,              pz,              1.0f  // Position
    };

    // Prepare snapping values
    float snapValues[3] = { 0.0f, 0.0f, 0.0f };
    bool shouldSnap = useSnap || IsKeyDown(KEY_LEFT_SHIFT);
    
    if (shouldSnap) {
        if (currentGizmoOperation == ImGuizmo::TRANSLATE) { snapValues[0] = snapTranslation; snapValues[1] = snapTranslation; snapValues[2] = snapTranslation; }
        else if (currentGizmoOperation == ImGuizmo::ROTATE) { snapValues[0] = snapRotation; snapValues[1] = snapRotation; snapValues[2] = snapRotation; }
        else if (currentGizmoOperation == ImGuizmo::SCALE) { snapValues[0] = snapScale; snapValues[1] = snapScale; snapValues[2] = snapScale; }
    }

    // Disable Gizmo interaction when CTRL is held.
    // This stops ImGuizmo from eating the mouse click, allowing you to easily deselect the primary node!
    bool isCtrlDown = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    ImGuizmo::Enable(!isCtrlDown);

    float deltaMatrix[16];
    ImGuizmo::Manipulate(view, proj, currentGizmoOperation, currentGizmoMode, gizmoMatrix, deltaMatrix, shouldSnap ? snapValues : nullptr);

    ImGuizmo::Enable(true); // Re-enable immediately so other ImGuizmo features (ViewCube) aren't broken

    static bool wasUsingGizmo = false;
    bool isUsingGizmo = ImGuizmo::IsUsing();

    // If we are actively dragging the gizmo, update the point visually
    if (isUsingGizmo) {
        float oldX, oldY, oldZ; pt.getPosition(oldX, oldY, oldZ);
        float oldDir = pt.getDirection(), oldInc = pt.getInclination(), oldBank = pt.getBank();
        float oldSW = pt.getScaleWidth(), oldSH = pt.getScaleHeight();
        
        float dX = 0, dY = 0, dZ = 0, dDir = 0, dInc = 0, dBank = 0, dSW = 0, dSH = 0;

        // Extract Position
        float newX = gizmoMatrix[12];
        float newY = gizmoMatrix[13];
        float newZ = gizmoMatrix[14];

        // Prevents lateral drift: Apply global magnetism (Y=0) ONLY if Gizmo is in WORLD mode.
        // Applying global snap during translation on a slanted local axis causes mathematical feedback.
        if (currentGizmoOperation == ImGuizmo::TRANSLATE && snapToGround && currentGizmoMode == ImGuizmo::WORLD) {
            if (fabsf(newY) < 5.0f) newY = 0.0f;
        }
        pt.setPosition(newX, newY, newZ);
        dX = newX - oldX; dY = newY - oldY; dZ = newZ - oldZ;

        // Only extract scale if we are explicitly scaling to prevent floating point degradation
        if (currentGizmoOperation == ImGuizmo::SCALE) {
            // Extract Scale (Length of Gizmo's X and Y columns)
            float newSW = Vector3Length(Vector3{gizmoMatrix[0], gizmoMatrix[1], gizmoMatrix[2]});
            float newSH = Vector3Length(Vector3{gizmoMatrix[4], gizmoMatrix[5], gizmoMatrix[6]});
            pt.setScaleWidth(newSW < 0.01f ? 0.01f : newSW);
            pt.setScaleHeight(newSH < 0.01f ? 0.01f : newSH);
            dSW = pt.getScaleWidth() - oldSW; dSH = pt.getScaleHeight() - oldSH;
        }
        // Only extract rotation if we are explicitly rotating
        else if (currentGizmoOperation == ImGuizmo::ROTATE) {
            float newSW = pt.getScaleWidth();
            float newSH = pt.getScaleHeight();

            // Extract and Normalize Rotation Axes
            Vector3 newRight    = Vector3Scale(Vector3{gizmoMatrix[0], gizmoMatrix[1], gizmoMatrix[2]}, 1.0f / (newSW < 0.01f ? 0.01f : newSW));
            Vector3 newUp       = Vector3Scale(Vector3{gizmoMatrix[4], gizmoMatrix[5], gizmoMatrix[6]}, 1.0f / (newSH < 0.01f ? 0.01f : newSH));
            Vector3 newBackward = Vector3Normalize(Vector3{gizmoMatrix[8], gizmoMatrix[9], gizmoMatrix[10]});
            
            // Reconstruct Forward by flipping Backward
            Vector3 newForward  = Vector3{-newBackward.x, -newBackward.y, -newBackward.z};

            // Robust Euler Angle Reconstruction (Trackmaker Order: Y -> Z -> X)
            float sinI = newForward.y;
            if (sinI > 1.0f) sinI = 1.0f;
            if (sinI < -1.0f) sinI = -1.0f;
            float inc = asinf(sinI);

            float dir, bank;
            float cosI = cosf(inc);
            
            if (fabs(cosI) > 0.001f) {
                dir = atan2f(-newForward.z, newForward.x);
                bank = atan2f(-newRight.y, newUp.y);
            } else {
                // Gimbal Lock prevention when the axis points exactly vertical (+/- 90 degrees Pitch)
                bank = pt.getBank();
                dir = pt.getDirection();
            }
            
            pt.setInclination(inc);
            pt.setDirection(dir);
            pt.setBank(bank);
            dDir = dir - oldDir; dInc = inc - oldInc; dBank = bank - oldBank;
        }

        // Apply deltas to other selected nodes
        for (size_t idx : selectedPointIndices) {
            if (idx == selectedPointIndex || idx >= trackPoints.size()) continue;
            AnchorPoint& op = trackPoints[idx];
            
            float ox, oy, oz; op.getPosition(ox, oy, oz);
            op.setPosition(ox + dX, oy + dY, oz + dZ);
            if (currentGizmoOperation == ImGuizmo::ROTATE) {
                op.setDirection(op.getDirection() + dDir); op.setInclination(op.getInclination() + dInc); op.setBank(op.getBank() + dBank);
            } else if (currentGizmoOperation == ImGuizmo::SCALE) {
                float nsw = op.getScaleWidth() + dSW, nsh = op.getScaleHeight() + dSH;
                op.setScaleWidth(nsw<0.01f?0.01f:nsw); op.setScaleHeight(nsh<0.01f?0.01f:nsh);
            }
        }
        
        if (realtimeMeshRebuild) {
            needsMeshRebuild = true;
        }
    }

    // Trigger a mesh rebuild ONLY when the user releases the mouse to save performance
    if (wasUsingGizmo && !isUsingGizmo) {
        needsMeshRebuild = true;
        RecordState();
    }
    wasUsingGizmo = isUsingGizmo;
}


// --- VIEW CUBE OVERLAY ---
void DrawViewCube(Camera3D& camera) {
    // PURE APPROACH: Draw entirely on top of everything, completely bypassing ImGui Windows!
    ImGuizmo::SetDrawlist(ImGui::GetForegroundDrawList());

    Matrix viewMat = GetCameraMatrix(camera);
    
    // EXPLICIT MAPPING to OpenGL Column-Major array
    float view[16] = {
        viewMat.m0, viewMat.m1, viewMat.m2, viewMat.m3,
        viewMat.m4, viewMat.m5, viewMat.m6, viewMat.m7,
        viewMat.m8, viewMat.m9, viewMat.m10, viewMat.m11,
        viewMat.m12, viewMat.m13, viewMat.m14, viewMat.m15
    };

    float oldView[16];
    for(int i = 0; i < 16; i++) oldView[i] = view[i];

    float camDistance = Vector3Distance(camera.position, camera.target);
    
    // Stick it precisely in the top right corner of the screen
    ImVec2 size(140, 140);
    ImVec2 pos(GetScreenWidth() - size.x, 0);
    ImGuizmo::ViewManipulate(view, camDistance, pos, size, 0x00000000);

    // If ImGuizmo modified the matrix, safely invert it back to the Raylib Camera struct
    bool modified = false;
    for(int i = 0; i < 16; i++) {
        if(view[i] != oldView[i]) { modified = true; break; }
    }

    if (modified) {
        Matrix updatedView;
        updatedView.m0 = view[0];   updatedView.m1 = view[1];   updatedView.m2 = view[2];   updatedView.m3 = view[3];
        updatedView.m4 = view[4];   updatedView.m5 = view[5];   updatedView.m6 = view[6];   updatedView.m7 = view[7];
        updatedView.m8 = view[8];   updatedView.m9 = view[9];   updatedView.m10 = view[10]; updatedView.m11 = view[11];
        updatedView.m12 = view[12]; updatedView.m13 = view[13]; updatedView.m14 = view[14]; updatedView.m15 = view[15];
        
        Matrix invView = MatrixInvert(updatedView);

        camera.position = Vector3{ invView.m12, invView.m13, invView.m14 };
        Vector3 forward = { -invView.m8, -invView.m9, -invView.m10 };
        camera.target = Vector3Add(camera.position, Vector3Scale(forward, camDistance));
        camera.up = Vector3{ invView.m4, invView.m5, invView.m6 };
    }
}


// --- SELECTION LOGIC ---
void HandleMousePicking(Camera3D& camera) {
    if (!showTrackNodes) return; // Prevent selecting invisible nodes

    static bool isTrackingClick = false;
    
    // Define the exclusion zone for the ViewCube (top right 140x140 area)
    bool isHoveringViewCube = (GetMouseX() >= GetScreenWidth() - 140 && GetMouseY() <= 140);

    bool isCtrlDown = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);

    // By ignoring ImGuizmo::IsOver() when CTRL is held, we can click "through" the gizmo to deselect
    if (ImGui::GetIO().WantCaptureMouse || (ImGuizmo::IsOver() && !isCtrlDown) || isHoveringViewCube) {
        isTrackingClick = false;
        return;
    }

    static Vector2 clickPosition = { 0.0f, 0.0f };

    // Store the mouse position when the user first presses the button
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        clickPosition = GetMousePosition();
        isTrackingClick = true;
    }

    // Only process the picking when the button is released AND we started the click cleanly
    if (isTrackingClick && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
        isTrackingClick = false;
        Vector2 releasePosition = GetMousePosition();
        float distance = Vector2Distance(clickPosition, releasePosition);

        // If the mouse moved less than 5 pixels, consider it a deliberate "click" instead of a "drag"
        if (distance < 5.0f) {
            Ray ray = GetMouseRay(GetMousePosition(), camera);
            float closestDistance = 1000000.0f; // A very large number to find the closest hit
            size_t hitIndex = NO_SELECTION;

            for (size_t i = 0; i < trackPoints.size(); i++) {
                float px, py, pz;
                trackPoints[i].getPosition(px, py, pz);
                Vector3 pos = { px, py, pz };

                // The radius used in DrawTrackStructure is 15.0f
                RayCollision collision = GetRayCollisionSphere(ray, pos, 15.0f);

                if (collision.hit && collision.distance < closestDistance) {
                    closestDistance = collision.distance;
                    hitIndex = i;
                }
            }

            if (IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL)) {
                if (hitIndex != NO_SELECTION) {
                    auto it = std::find(selectedPointIndices.begin(), selectedPointIndices.end(), hitIndex);
                    if (it != selectedPointIndices.end()) {
                        selectedPointIndices.erase(it);
                        if (selectedPointIndex == hitIndex) selectedPointIndex = selectedPointIndices.empty() ? NO_SELECTION : selectedPointIndices.back();
                    } else {
                        selectedPointIndices.push_back(hitIndex);
                        selectedPointIndex = hitIndex;
                    }
                }
            } else {
                selectedPointIndices.clear();
                if (hitIndex != NO_SELECTION) {
                    selectedPointIndices.push_back(hitIndex);
                }
                selectedPointIndex = hitIndex;
            }
        }
    }
}


// --- CAMERA LOGIC ---
void UpdateEditorCamera(Camera3D& camera) {
    if (ImGui::GetIO().WantCaptureMouse) return;

    Vector2 mouseDelta = GetMouseDelta();
    float wheel = GetMouseWheelMove();

    // 1. Zoom (Scroll Wheel)
    if (wheel != 0.0f) {
        if (camera.projection == CAMERA_ORTHOGRAPHIC) {
            // In Ortho mode, zooming modifies the viewport width/height directly via fovy
            camera.fovy -= wheel * 50.0f;
            if (camera.fovy < 10.0f) camera.fovy = 10.0f;
        } else {
            Vector3 forward = Vector3Normalize(Vector3Subtract(camera.target, camera.position));
            camera.position = Vector3Add(camera.position, Vector3Scale(forward, wheel * 100.0f));
        }
    }

    // 2. Orbit around target (Left Mouse Button)
    if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
        Vector3 posFromTarget = Vector3Subtract(camera.position, camera.target);

        float yawAngle = -mouseDelta.x * 0.005f;
        float pitchAngle = -mouseDelta.y * 0.005f;

        // Determine if we are upside down to keep mouse yaw intuitive
        bool isUpsideDown = (camera.up.y < 0.0f);
        Vector3 globalUp = { 0.0f, isUpsideDown ? -1.0f : 1.0f, 0.0f };

        // Calculate local Right vector based on Global UP
        Vector3 forward = Vector3Normalize(Vector3Scale(posFromTarget, -1.0f));
        Vector3 right = Vector3CrossProduct(forward, globalUp);

        // Prevent gimbal lock when looking exactly up/down
        if (Vector3Length(right) < 0.001f) {
            right = Vector3CrossProduct(forward, camera.up);
        }
        right = Vector3Normalize(right);

        // Apply Pitch (around local right axis)
        Matrix pitchMatrix = MatrixRotate(right, pitchAngle);
        posFromTarget = Vector3Transform(posFromTarget, pitchMatrix);

        // Apply Yaw (around global Y axis)
        Matrix yawMatrix = MatrixRotate(Vector3{ 0.0f, 1.0f, 0.0f }, yawAngle);
        posFromTarget = Vector3Transform(posFromTarget, yawMatrix);

        camera.position = Vector3Add(camera.target, posFromTarget);

        // Strictly re-calculate the UP vector to perfectly eliminate any roll drift (Tilt)
        forward = Vector3Normalize(Vector3Subtract(camera.target, camera.position));
        right = Vector3CrossProduct(forward, globalUp);
        
        if (Vector3Length(right) < 0.001f) {
            // If we pitched exactly to the pole, keep the old UP vector but rotate it
            camera.up = Vector3Transform(camera.up, pitchMatrix);
            camera.up = Vector3Transform(camera.up, yawMatrix);
        } else {
            camera.up = Vector3Normalize(Vector3CrossProduct(Vector3Normalize(right), forward));
        }
    }

    // 3. Pan (Right OR Middle Mouse Button)
    if (IsMouseButtonDown(MOUSE_BUTTON_RIGHT) || IsMouseButtonDown(MOUSE_BUTTON_MIDDLE)) {
        Vector3 forward = Vector3Normalize(Vector3Subtract(camera.target, camera.position));
        Vector3 right = Vector3Normalize(Vector3CrossProduct(forward, camera.up));
        Vector3 up = Vector3Normalize(Vector3CrossProduct(right, forward));

        // Calculate distance to scale the panning speed dynamically
        float dist = Vector3Distance(camera.position, camera.target);
        float panSpeed = dist * 0.001f;

        Vector3 panDelta = Vector3Add(Vector3Scale(right, -mouseDelta.x * panSpeed), Vector3Scale(up, mouseDelta.y * panSpeed));

        camera.position = Vector3Add(camera.position, panDelta);
        camera.target = Vector3Add(camera.target, panDelta);
    }
}


// --- CUSTOM GRID DRAWING ---
void DrawCustomGrid(int slices, float spacing, float yOffset, Color color) {
    int halfSlices = slices / 2;

    rlBegin(RL_LINES);
    for (int i = -halfSlices; i <= halfSlices; i++) {
        rlColor4ub(color.r, color.g, color.b, color.a);

        rlVertex3f((float)i * spacing, yOffset, (float)-halfSlices * spacing);
        rlVertex3f((float)i * spacing, yOffset, (float)halfSlices * spacing);

        rlVertex3f((float)-halfSlices * spacing, yOffset, (float)i * spacing);
        rlVertex3f((float)halfSlices * spacing, yOffset, (float)i * spacing);
    }
    rlEnd();
}

// --- 3D HYBRID VIEWPORT ---
void DrawTrackStructure() {
    for (size_t i = 0; i < trackPoints.size(); i++) {
        AnchorPoint& pt = trackPoints[i];

        float px, py, pz;
        pt.getPosition(px, py, pz);
        Vector3 pos = { px, py, pz };

        matrix rotMat = pt.getMatrix();
        Vector3 forward = { rotMat.x.x, rotMat.x.y, rotMat.x.z };

        if (showTrackNodes) {
            // Draw local orientation axes
            Vector3 up = { rotMat.y.x, rotMat.y.y, rotMat.y.z };
            Vector3 right = { rotMat.z.x, rotMat.z.y, rotMat.z.z };

            // INCREASED AXIS SCALE: Changed from 10.0f to 50.0f
            float axisLength = 50.0f;
            DrawLine3D(pos, Vector3Add(pos, Vector3Scale(forward, axisLength * 2.0f)), BLUE);
            DrawLine3D(pos, Vector3Add(pos, Vector3Scale(up, axisLength * pt.getScaleHeight())), GREEN);
            DrawLine3D(pos, Vector3Add(pos, Vector3Scale(right, axisLength * pt.getScaleWidth())), RED);
            DrawLine3D(pos, Vector3Subtract(pos, Vector3Scale(right, axisLength * pt.getScaleWidth())), RED);

            // INCREASED SPHERE SCALE: Changed radius from 2.0f to 15.0f
            Color pointColor = YELLOW;
            if (std::find(selectedPointIndices.begin(), selectedPointIndices.end(), i) != selectedPointIndices.end()) {
                pointColor = (i == selectedPointIndex) ? RED : ORANGE;
            }
            DrawSphere(pos, 15.0f, pointColor);
        }

        // Draw Bezier Curves
        if (showTrackCurve && i < trackPoints.size() - 1) {
            AnchorPoint& nextPt = trackPoints[i + 1];

            float nx, ny, nz;
            nextPt.getPosition(nx, ny, nz);
            Vector3 nextPos = { nx, ny, nz };

            matrix m2 = nextPt.getMatrix();
            float length = Vector3Distance(pos, nextPos);

            Vector3 p1 = Vector3Add(pos, Vector3Scale(forward, length * pt.getNextControlPointDistanceFactor()));
            Vector3 p2 = Vector3Add(nextPos, Vector3Scale(Vector3{ m2.x.x, m2.x.y, m2.x.z }, -length * nextPt.getPreviousControlPointDistanceFactor()));

            Vector3 previousCurvePt = pos;
            for (int step = 1; step <= 20; step++) {
                float t = (float)step / 20.0f;
                Vector3 currentCurvePt = EvaluateCubicBezier(pos, p1, p2, nextPos, t);
                DrawLine3D(previousCurvePt, currentCurvePt, ORANGE);
                previousCurvePt = currentCurvePt;
            }
        }
    }
}


// --- MAIN ENTRY POINT ---
int main(void) {
    const int screenWidth = 1280;
    const int screenHeight = 720;

    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT);
    InitWindow(screenWidth, screenHeight, "Trackmaker - Windows Editor");

    // Increase the far clipping plane distance to render massive track layouts.
    // Pushing the near plane to 10.0 further avoids aggressive Z-buffer precision loss.
    rlSetClipPlanes(10.0, 100000.0);

    // INCREASED INITIAL SCALE: Pull the camera back to see large tracks like scurve.track
    Camera3D camera = { 0 };
    camera.position = Vector3{ 800.0f, 500.0f, 800.0f };
    camera.target = Vector3{ 200.0f, 150.0f, -250.0f }; // Roughly the center of scurve
    camera.up = Vector3{ 0.0f, 1.0f, 0.0f };
    camera.fovy = 45.0f;
    camera.projection = CAMERA_PERSPECTIVE;

    rlImGuiSetup(true);

    // NEW: Disable the generation of imgui.ini to keep your track folders clean
    ImGui::GetIO().IniFilename = nullptr;

    SetTargetFPS(60);

    Matrix viewMat = { 0 };
    Matrix projMat = { 0 };

    while (!WindowShouldClose()) {

        // Handle global shortcuts for Gizmo tools
        if (!ImGui::GetIO().WantTextInput) {
            if (IsKeyPressed(KEY_W)) currentGizmoOperation = ImGuizmo::TRANSLATE;
            if (IsKeyPressed(KEY_E)) currentGizmoOperation = ImGuizmo::ROTATE;
            if (IsKeyPressed(KEY_R)) currentGizmoOperation = ImGuizmo::SCALE;
            if (IsKeyPressed(KEY_Q)) {
                currentGizmoMode = (currentGizmoMode == ImGuizmo::LOCAL) ? ImGuizmo::WORLD : ImGuizmo::LOCAL;
            }

            // Handle Undo/Redo shortcuts (Ctrl+Z / Ctrl+Y)
            bool isCtrlDown = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
            if (isCtrlDown && IsKeyPressed(KEY_Z)) {
                Undo();
            }
            if (isCtrlDown && IsKeyPressed(KEY_Y)) {
                Redo();
            }
            
            // Handle Duplicate Point shortcut (Ctrl+D)
            if (isCtrlDown && IsKeyPressed(KEY_D)) {
                if (!selectedPointIndices.empty()) {
                    std::vector<size_t> sortedIndices = selectedPointIndices;
                    std::sort(sortedIndices.begin(), sortedIndices.end()); // Ascending order
                    
                    std::vector<AnchorPoint> duplicatedPoints;
                    
                    // To keep the block shape intact, we calculate a SINGLE offset 
                    // along the direction of the primary node (the one with the Gizmo).
                    size_t referenceIdx = (selectedPointIndex != NO_SELECTION && selectedPointIndex < trackPoints.size()) 
                                          ? selectedPointIndex : sortedIndices.back();
                    
                    matrix refMat = trackPoints[referenceIdx].getMatrix();
                    float dx = refMat.x.x * 10.0f;
                    float dy = refMat.x.y * 10.0f;
                    float dz = refMat.x.z * 10.0f;

                    // Append the block right after the last element of the selection
                    size_t insertIndex = sortedIndices.back() + 1;
                    
                    for (size_t idx : sortedIndices) {
                        if (idx < trackPoints.size()) {
                            AnchorPoint dup = trackPoints[idx];
                            float px, py, pz; dup.getPosition(px, py, pz);
                            dup.setPosition(px + dx, py + dy, pz + dz);
                            duplicatedPoints.push_back(dup);
                        }
                    }
                    
                    // Insert all duplicated points as a sequential block
                    trackPoints.insert(trackPoints.begin() + insertIndex, duplicatedPoints.begin(), duplicatedPoints.end());
                    
                    // Update selection to the newly created points (shifting focus)
                    selectedPointIndices.clear();
                    size_t newPrimaryIndex = insertIndex;
                    for (size_t i = 0; i < sortedIndices.size(); ++i) {
                        size_t newIdx = insertIndex + i;
                        selectedPointIndices.push_back(newIdx);
                        if (sortedIndices[i] == selectedPointIndex) {
                            newPrimaryIndex = newIdx; // Keeps the gizmo on the corresponding node
                        }
                    }
                    
                    selectedPointIndex = newPrimaryIndex;

                    needsMeshRebuild = true;
                    RecordState();
                    TraceLog(LOG_INFO, "Multiple nodes duplicated as block via CTRL+D");
                }
            }
        }

        if (needsMeshRebuild) {
            RebuildTrackMesh();
        }

        HandleMousePicking(camera);

        // --- ABSOLUTE CAMERA PROTECTION ---
        // Explicitly block Raylib from rotating the background camera if we are interacting with the ViewCube
        static bool isDraggingViewCube = false;
        bool isHoveringViewCube = (GetMouseX() >= GetScreenWidth() - 140 && GetMouseY() <= 140);
        
        if (isHoveringViewCube && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) isDraggingViewCube = true;
        if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) isDraggingViewCube = false;

        // Only update standard camera logic if we are completely clear of the top-right corner
        if (!isDraggingViewCube && !isHoveringViewCube) {
            UpdateEditorCamera(camera);
        }

        BeginDrawing();
        ClearBackground(Color{ 30, 30, 30, 255 });

        BeginMode3D(camera);

        // Capture matrices while in 3D mode for ImGuizmo
        viewMat = rlGetMatrixModelview();
        projMat = rlGetMatrixProjection();

        // --- BACKGROUND GRID FIX ---
        // By disabling the depth test, we force the grid to render purely as a background.
        // This completely eliminates Z-fighting, regardless of how far the camera is!
        rlDisableDepthTest();

        Color gridColor = { 50, 50, 50, 255 }; // Subtle dark gray
        DrawCustomGrid(266, 250.0f, 0.0f, gridColor);

        // Draw origin axes (X = Red, Z = Blue)
        DrawLine3D(Vector3{ -33250.0f, 0.0f, 0.0f }, Vector3{ 33250.0f, 0.0f, 0.0f }, RED);
        DrawLine3D(Vector3{ 0.0f, 0.0f, -33250.0f }, Vector3{ 0.0f, 0.0f, 33250.0f }, BLUE);

        rlDrawRenderBatchActive(); // Force flush the lines so they render right now
        rlEnableDepthTest();       // Re-enable depth testing for the 3D track
        // ---------------------------

        // 1. Draw the solid 3D Track Model FIRST
        if (currentTrackModel.meshCount > 0) {
            if (showSolidMesh) DrawModel(currentTrackModel, Vector3{ 0.0f, 0.0f, 0.0f }, 1.0f, WHITE);
            if (showWireframe) DrawModelWires(currentTrackModel, Vector3{ 0.0f, 0.0f, 0.0f }, 1.0f, DARKGRAY);
        }

        // 2. Disable depth testing for X-Ray vision
        rlDisableDepthTest();

        // 3. Queue the skeleton and points
        DrawTrackStructure();

        // 4. Flush the 3D queue while depth testing is disabled
        EndMode3D();

        // REMOVED: rlEnableDepthTest(); 
        // By leaving it disabled, ImGui is guaranteed to draw over everything!

        rlImGuiBegin();
        ImGuizmo::BeginFrame();

        // FIX: Force ImGuizmo context initialization to prevent division by zero (NaN) 
        // inside ViewManipulate when no Anchor Point is currently selected.
        ImGuizmo::SetRect(0.0f, 0.0f, static_cast<float>(GetScreenWidth()), static_cast<float>(GetScreenHeight()));
        float identity[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
        
        // Passing identity to view/proj ensures we don't accidentally intercept actual 3D interactions
        ImGuizmo::Manipulate(identity, identity, (ImGuizmo::OPERATION)0, ImGuizmo::LOCAL, identity);

        RenderTrackmakerGUI(camera);
        DrawViewCube(camera);
        if (showTrackNodes) {
            DrawGizmo(viewMat, projMat, camera.projection == CAMERA_ORTHOGRAPHIC);
        }

        rlImGuiEnd();

        EndDrawing();
    }

    // Cleanup GPU Memory
    if (currentTrackModel.meshCount > 0) {
        UnloadModel(currentTrackModel);
    }
    if (currentTrackTexture.id != 0) {
        UnloadTexture(currentTrackTexture);
    }
    rlImGuiShutdown();
    CloseWindow();
    return 0;
}