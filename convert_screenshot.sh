#!/bin/bash
# Convert BMP screenshots to PNG format
# Supports: ImageMagick → ffmpeg → Python fallback
# Usage: ./convert_screenshot.sh

SCREENSHOT_DIR="/home/branis/Cynosure/screenshots"
PYTHON_CONVERTER="$(dirname "$0")/convert_bmp_to_png.py"

if [ ! -d "$SCREENSHOT_DIR" ]; then
    echo "Screenshot directory not found: $SCREENSHOT_DIR"
    exit 1
fi

echo "Converting BMP screenshots to PNG..."
echo "Looking for: ImageMagick, ffmpeg, or Python fallback"
count=0
failed=0

for bmp_file in "$SCREENSHOT_DIR"/*.bmp; do
    if [ -f "$bmp_file" ]; then
        # Get basename without extension
        base_name=$(basename "$bmp_file" .bmp)
        png_file="$SCREENSHOT_DIR/${base_name}.png"
        converted=0

        # Try ImageMagick first (best quality)
        if command -v convert &> /dev/null; then
            convert "$bmp_file" "$png_file" 2>/dev/null
            if [ -f "$png_file" ] && [ -s "$png_file" ]; then
                echo "✓ Converted (ImageMagick): $(basename "$bmp_file") → $(basename "$png_file")"
                ((count++))
                converted=1
            fi
        fi

        # Try ffmpeg if ImageMagick failed
        if [ $converted -eq 0 ] && command -v ffmpeg &> /dev/null; then
            ffmpeg -i "$bmp_file" "$png_file" -y 2>/dev/null
            if [ -f "$png_file" ] && [ -s "$png_file" ]; then
                echo "✓ Converted (ffmpeg): $(basename "$bmp_file") → $(basename "$png_file")"
                ((count++))
                converted=1
            fi
        fi

        # Try Python fallback if both above failed
        if [ $converted -eq 0 ]; then
            if [ -f "$PYTHON_CONVERTER" ]; then
                if command -v python3 &> /dev/null; then
                    python3 "$PYTHON_CONVERTER" "$bmp_file" "$png_file" 2>/dev/null
                    if [ -f "$png_file" ] && [ -s "$png_file" ]; then
                        echo "✓ Converted (Python): $(basename "$bmp_file") → $(basename "$png_file")"
                        ((count++))
                        converted=1
                    fi
                elif command -v python &> /dev/null; then
                    python "$PYTHON_CONVERTER" "$bmp_file" "$png_file" 2>/dev/null
                    if [ -f "$png_file" ] && [ -s "$png_file" ]; then
                        echo "✓ Converted (Python): $(basename "$bmp_file") → $(basename "$png_file")"
                        ((count++))
                        converted=1
                    fi
                fi
            fi
        fi

        # Report failure if all methods failed
        if [ $converted -eq 0 ]; then
            echo "✗ Failed to convert: $(basename "$bmp_file") (no converter available or conversion error)"
            ((failed++))
        fi
    fi
done

echo ""
echo "Conversion complete:"
echo "  Converted: $count files"
echo "  Failed: $failed files"
echo "  PNG files are in: $SCREENSHOT_DIR"

if [ $failed -gt 0 ]; then
    exit 1
else
    exit 0
fi
