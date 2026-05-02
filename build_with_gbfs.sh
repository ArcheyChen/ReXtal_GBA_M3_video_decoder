#!/bin/bash
#
# Build multibank GBA ROM with GBFS media archive
#
# Usage: ./build_with_gbfs.sh <video.gbm> <audio.gbs>
#        ./build_with_gbfs.sh <video.gbm>
#        ./build_with_gbfs.sh <audio.gbs>
#

set -e

GBFS_TOOLS="./tools/gbfs/tools"
GBFS_CMD="$GBFS_TOOLS/gbfs"
PADBIN_CMD="$GBFS_TOOLS/padbin"

# Check if gbfs tools exist
if [ ! -x "$GBFS_CMD" ]; then
    echo "Error: gbfs tool not found at $GBFS_CMD"
    echo "Please build the tools: cd $GBFS_TOOLS && sh mktools.sh"
    exit 1
fi

# Show usage
show_usage() {
    echo "Usage: $0 <video.gbm> <audio.gbs>"
    echo "       $0 <video.gbm>"
    echo "       $0 <audio.gbs>"
    echo ""
    echo "Examples:"
    echo "  $0 ../output.gbm ../output.gbs"
    echo "  $0 ../video_only.gbm"
    echo "  $0 ../audio_only.gbs"
    exit 1
}

# Need at least one argument
if [ $# -eq 0 ]; then
    show_usage
fi

# Parse arguments
GBM_FILE=""
GBS_FILE=""

for arg in "$@"; do
    if [ ! -f "$arg" ]; then
        echo "Error: File not found: $arg"
        exit 1
    fi

    case "${arg##*.}" in
        gbm|GBM)
            if [ -n "$GBM_FILE" ]; then
                echo "Error: Multiple .gbm files specified"
                exit 1
            fi
            GBM_FILE="$arg"
            ;;
        gbs|GBS)
            if [ -n "$GBS_FILE" ]; then
                echo "Error: Multiple .gbs files specified"
                exit 1
            fi
            GBS_FILE="$arg"
            ;;
        *)
            echo "Error: Unknown file type: $arg"
            echo "Supported: .gbm (video), .gbs (audio)"
            exit 1
            ;;
    esac
done

# Build the ROM first
echo "Building ROM..."
make

echo ""
echo "Creating GBFS archive:"

# Collect files to add (rename to standard names for simplicity)
TEMP_DIR=$(mktemp -d)
MEDIA_FILES=""

if [ -n "$GBM_FILE" ]; then
    cp "$GBM_FILE" "$TEMP_DIR/video.gbm"
    MEDIA_FILES="$TEMP_DIR/video.gbm"
    echo "  [Video] $GBM_FILE -> video.gbm"
fi

if [ -n "$GBS_FILE" ]; then
    cp "$GBS_FILE" "$TEMP_DIR/audio.gbs"
    MEDIA_FILES="$MEDIA_FILES $TEMP_DIR/audio.gbs"
    echo "  [Audio] $GBS_FILE -> audio.gbs"
fi

# Output filename
INPUT_ROM="M3_Movie_Player_mb.gba"
OUTPUT_ROM="M3_Movie_Player_output.gba"

# Copy ROM to output file (preserve original)
echo ""
echo "Copying ROM to $OUTPUT_ROM..."
cp "$INPUT_ROM" "$OUTPUT_ROM"

# Pad output ROM to 256-byte boundary (required for GBFS search)
echo "Padding ROM..."
$PADBIN_CMD 256 "$OUTPUT_ROM"

# Create GBFS archive
echo "Creating GBFS archive..."
$GBFS_CMD media_data.gbfs $MEDIA_FILES

# Append GBFS to output ROM
echo "Appending GBFS to ROM..."
cat media_data.gbfs >> "$OUTPUT_ROM"

# Clean up
rm -f media_data.gbfs
rm -rf "$TEMP_DIR"

# Show result
echo ""
echo "Done! Output: $OUTPUT_ROM"
ls -la "$OUTPUT_ROM"
