#!/usr/bin/env python3
"""
Convert gbborder.jpg to gbborder.h C header file
This replaces the functionality of bin2h tool used in makeborder.bat
"""

import os

def create_header_from_binary(input_file, output_file, array_name):
    """Convert binary file to C header with byte array"""
    try:
        with open(input_file, 'rb') as f:
            data = f.read()
        
        with open(output_file, 'w') as f:
            f.write(f"// Auto-generated header file from {input_file}\n")
            f.write(f"#ifndef __{array_name.upper()}_H__\n")
            f.write(f"#define __{array_name.upper()}_H__\n\n")
            f.write(f"#include <stdint.h>\n\n")
            f.write(f"const uint32_t {array_name}_size = {len(data)};\n\n")
            f.write(f"const uint8_t {array_name}[] = {{\n")
            
            # Write bytes in groups of 16 per line
            for i in range(0, len(data), 16):
                chunk = data[i:i+16]
                hex_values = [f"0x{byte:02x}" for byte in chunk]
                f.write("    " + ", ".join(hex_values))
                if i + 16 < len(data):
                    f.write(",")
                f.write("\n")
            
            f.write("};\n\n")
            f.write(f"#endif // __{array_name.upper()}_H__\n")
        
        print(f"Successfully generated {output_file}")
        return True
        
    except Exception as e:
        print(f"Error: {e}")
        return False

if __name__ == "__main__":
    input_file = "espeon/gbborder.jpg"
    output_file = "espeon/gbborder.h"
    array_name = "gb_border"
    
    if not os.path.exists(input_file):
        print(f"Error: {input_file} not found")
        exit(1)
    
    success = create_header_from_binary(input_file, output_file, array_name)
    if not success:
        exit(1)
