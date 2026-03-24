import sys

def convert_template_to_obj(input_path, output_path):
    base_vertices = []
    faces = []

    try:
        with open(input_path, 'r') as f:
            for line in f:
                parts = line.strip().split()
                if not parts:
                    continue
                
                if 'v' in parts:
                    idx = parts.index('v')
                    if len(parts) >= idx + 4:
                        # Save X, Y, Z as decimal numbers
                        base_vertices.append((float(parts[idx+1]), float(parts[idx+2]), float(parts[idx+3])))
                        
                elif 'f' in parts:
                    idx = parts.index('f')
                    if len(parts) >= idx + 4:
                        faces.append((int(parts[idx+1]), int(parts[idx+2]), int(parts[idx+3])))

        # SIMULATE TRACKMAKER EXTRUSION
        all_vertices = []
        segment_length = 10.0 # Move the second ring forward by 10 units on the X axis
        
        # 1. Add the vertices of the initial ring
        for v in base_vertices:
            all_vertices.append((v[0], v[1], v[2]))
            
        # 2. Add the vertices of the final ring
        for v in base_vertices:
            # Add length to the X axis
            all_vertices.append((v[0] + segment_length, v[1], v[2]))

        # WRITE THE .OBJ FILE
        with open(output_path, 'w') as out:
            out.write("# Extruded OBJ from .tracktemplate\n\n")
            
            if len(all_vertices) == 0:
                print("ERROR: No vertices found in the template.")
                return
            
            # Write all extruded vertices
            for v in all_vertices:
                out.write(f"v {v[0]} {v[1]} {v[2]}\n")
            out.write("\n")
                
            # Write faces
            for f in faces:
                i1, i2, i3 = f[0] + 1, f[1] + 1, f[2] + 1
                out.write(f"f {i1} {i2} {i3}\n")
                
        # --- PRINT STATISTICAL REPORT ---
        print("\n" + "="*45)
        print(" TEMPLATE STATISTICS")
        print("="*45)
        print(f" Base vertices found (2D): {len(base_vertices)}")
        print(f" Total exported vertices (3D): {len(all_vertices)}")
        print(f" Faces/Polygons created: {len(faces)}")
        print("="*45)
        print(f" File converted successfully!\n Saved in: {output_path}\n")

    except Exception as e:
        print(f"An error occurred: {e}")

if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("Correct usage: python template_to_obj.py <input.tracktemplate> <output.obj>")
    else:
        file_input = sys.argv[1]
        file_output = sys.argv[2]
        convert_template_to_obj(file_input, file_output)