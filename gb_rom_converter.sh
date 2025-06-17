#!/bin/bash
read -p "Enter ROM file path: " rom_path
filename=$(basename "$rom_path" .gb)
header_name=$(echo "$filename" | sed 's/[^a-zA-Z0-9]/_/g').h
array_name=$(echo "$filename" | sed 's/[^a-zA-Z0-9]/_/g' | tr '[:upper:]' '[:lower:]')_rom

echo "Choose compression method:"
echo "1) Uncompressed (current)"
echo "2) GZIP compressed"
read -p "Enter choice (1-2): " choice

case $choice in
    2)
        temp_gz="/tmp/$(basename "$rom_path").gz"
        gzip -c "$rom_path" > "$temp_gz"
        bin2header "$temp_gz" "${array_name}_gz" > "$header_name"
        rm "$temp_gz"
        echo "Created: $header_name (GZIP compressed) with array: ${array_name}_gz"
        ;;
    *)
        bin2header "$rom_path" "$array_name" > "$header_name"
        echo "Created: $header_name with array: $array_name"
        ;;
esac

echo "File size: $(du -h "$header_name" | cut -f1)"
