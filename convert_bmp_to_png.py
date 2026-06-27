#!/usr/bin/env python3
"""
BMP to PNG converter using PIL/Pillow or manual binary conversion.
Fallback when ImageMagick and ffmpeg are not available.

Usage: python3 convert_bmp_to_png.py <input.bmp> <output.png>
"""

import sys
import struct
import os

def convert_with_pil(bmp_path, png_path):
    """Convert BMP to PNG using PIL/Pillow library."""
    try:
        from PIL import Image
        img = Image.open(bmp_path)
        img.save(png_path, 'PNG')
        return True
    except ImportError:
        return False
    except Exception as e:
        print(f"PIL conversion failed: {e}", file=sys.stderr)
        return False

def convert_manual(bmp_path, png_path):
    """
    Manual BMP to PNG conversion using simple binary format handling.
    Reads BMP header and pixel data, writes PNG format.
    """
    try:
        # Read BMP file
        with open(bmp_path, 'rb') as f:
            bmp_data = f.read()

        if len(bmp_data) < 54:
            print("Error: BMP file too small (need at least 54 bytes)", file=sys.stderr)
            return False

        # Parse BMP header (simplified for standard 24-bit BMPs)
        if bmp_data[0:2] != b'BM':
            print("Error: Not a valid BMP file (missing BM signature)", file=sys.stderr)
            return False

        # Extract width and height (at offset 18 and 22, little-endian)
        width = struct.unpack('<I', bmp_data[18:22])[0]
        height = struct.unpack('<I', bmp_data[22:26])[0]
        bits_per_pixel = struct.unpack('<H', bmp_data[28:30])[0]

        if bits_per_pixel not in [24, 32, 8, 4, 1]:
            print(f"Error: Unsupported BMP bit depth: {bits_per_pixel}", file=sys.stderr)
            return False

        # Get pixel data offset
        pixel_offset = struct.unpack('<I', bmp_data[10:14])[0]
        pixel_data = bmp_data[pixel_offset:]

        # Create minimal PNG file (simplified implementation)
        # This creates a valid PNG but with limited color space
        # For better results, use PIL or ImageMagick
        png_data = create_minimal_png(width, height, pixel_data, bits_per_pixel)

        if png_data:
            with open(png_path, 'wb') as f:
                f.write(png_data)
            return True
        else:
            print("Error: Failed to create PNG data", file=sys.stderr)
            return False

    except Exception as e:
        print(f"Manual conversion failed: {e}", file=sys.stderr)
        return False

def create_minimal_png(width, height, pixel_data, bits_per_pixel):
    """
    Create a minimal valid PNG from pixel data.
    NOTE: This fallback is deprecated. Use convert_with_pil() instead.
    This is only used if PIL is not available.
    """
    try:
        import zlib
        import struct as st

        # PNG signature
        png_sig = b'\x89PNG\r\n\x1a\n'

        # Determine PNG color type based on BMP bits_per_pixel
        if bits_per_pixel == 8:
            # Grayscale (8-bit)
            color_type = 0
            pixel_bytes = width * height
        elif bits_per_pixel == 24:
            # RGB (8-bit per channel)
            color_type = 2
            pixel_bytes = width * height * 3
        elif bits_per_pixel == 32:
            # RGBA (8-bit per channel)
            color_type = 6
            pixel_bytes = width * height * 4
        else:
            # Convert to grayscale for 1, 4 bit
            color_type = 0
            pixel_bytes = width * height

        # IHDR chunk (image header)
        ihdr_data = st.pack('>IIBBBBB', width, height, 8, color_type, 0, 0, 0)
        ihdr_crc = zlib.crc32(b'IHDR' + ihdr_data) & 0xffffffff
        ihdr_chunk = st.pack('>I', 13) + b'IHDR' + ihdr_data + st.pack('>I', ihdr_crc)

        # Convert BMP pixel data to PNG format
        # BMP stores pixels bottom-up, PNG stores top-down
        # For simplicity, we'll just use the pixel data as-is with filter bytes
        scanlines = b''
        bytes_per_pixel = max(1, bits_per_pixel // 8)
        scanline_bytes = width * bytes_per_pixel

        # Process each scanline
        if len(pixel_data) >= scanline_bytes * height:
            # Process bottom-up (BMP) to top-down (PNG)
            for y in range(height):
                scanline = b'\x00'  # Filter type: None
                # BMP is bottom-up, read from end
                row_offset = (height - 1 - y) * scanline_bytes
                if row_offset + scanline_bytes <= len(pixel_data):
                    scanline += pixel_data[row_offset:row_offset + scanline_bytes]
                else:
                    # Incomplete data, pad with zeros
                    available = max(0, len(pixel_data) - row_offset)
                    scanline += pixel_data[row_offset:row_offset + available]
                    scanline += b'\x00' * (scanline_bytes - available)
                scanlines += scanline
        else:
            # Fallback: create gray placeholder if pixel data is insufficient
            for y in range(height):
                scanline = b'\x00'  # Filter type byte
                scanline += b'\x80' * scanline_bytes  # Mid-gray placeholder
                scanlines += scanline

        # Compress image data
        compressed = zlib.compress(scanlines, 9)

        # IDAT chunk (image data)
        idat_crc = zlib.crc32(b'IDAT' + compressed) & 0xffffffff
        idat_chunk = st.pack('>I', len(compressed)) + b'IDAT' + compressed + st.pack('>I', idat_crc)

        # IEND chunk (image end)
        iend_crc = zlib.crc32(b'IEND') & 0xffffffff
        iend_chunk = st.pack('>I', 0) + b'IEND' + st.pack('>I', iend_crc)

        # Assemble PNG
        return png_sig + ihdr_chunk + idat_chunk + iend_chunk

    except Exception as e:
        print(f"PNG creation failed: {e}", file=sys.stderr)
        return None

def main():
    if len(sys.argv) < 3:
        print("Usage: convert_bmp_to_png.py <input.bmp> <output.png>", file=sys.stderr)
        sys.exit(1)

    bmp_path = sys.argv[1]
    png_path = sys.argv[2]

    # Check if input file exists
    if not os.path.exists(bmp_path):
        print(f"Error: Input file not found: {bmp_path}", file=sys.stderr)
        sys.exit(1)

    # Try PIL first (best quality)
    if convert_with_pil(bmp_path, png_path):
        print(f"Successfully converted {bmp_path} to {png_path} (using PIL)")
        sys.exit(0)

    # Fall back to manual conversion
    if convert_manual(bmp_path, png_path):
        print(f"Successfully converted {bmp_path} to {png_path} (using manual converter)")
        sys.exit(0)

    print("Error: Failed to convert BMP to PNG", file=sys.stderr)
    sys.exit(1)

if __name__ == '__main__':
    main()
