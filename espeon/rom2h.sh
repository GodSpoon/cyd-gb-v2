#!/bin/bash
read -p "Enter ROM file path: " rom_path
filename=$(basename "$rom_path" .gb)
header_name=$(echo "$filename" | sed 's/[^a-zA-Z0-9]/_/g').h
array_name=$(echo "$filename" | sed 's/[^a-zA-Z0-9]/_/g' | tr '[:upper:]' '[:lower:]')_rom
bin2header "$rom_path" "$array_name" > "$header_name"
echo "Created: $header_name with array: $array_name"