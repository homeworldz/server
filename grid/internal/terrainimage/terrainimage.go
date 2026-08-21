// Package terrainimage implements the OpenSimulator/Halcyon image-heightmap
// convention used by Homeworldz terrain import tools.
package terrainimage

import (
	"encoding/binary"
	"fmt"
	"image"
	"image/color"
	"math"
)

// Heights converts an image to metre heights. Image rows are flipped because
// image Y grows downward while terrain Y grows northward. Pixel height is HSL
// lightness multiplied by 128, matching System.Drawing.Color.GetBrightness in
// the OpenSimulator and Halcyon GenericSystemDrawing terrain loaders.
func Heights(source image.Image, width, height int) ([]float32, error) {
	bounds := source.Bounds()
	if bounds.Dx() != width || bounds.Dy() != height {
		return nil, fmt.Errorf("terrain image is %dx%d; expected %dx%d",
			bounds.Dx(), bounds.Dy(), width, height)
	}
	result := make([]float32, width*height)
	for y := 0; y < height; y++ {
		sourceY := bounds.Min.Y + height - y - 1
		for x := 0; x < width; x++ {
			r, g, b, _ := source.At(bounds.Min.X+x, sourceY).RGBA()
			maximum := max(r, g, b)
			minimum := min(r, g, b)
			result[y*width+x] = float32(float64(maximum+minimum) * 64.0 / 65535.0)
		}
	}
	return result, nil
}

// ByteRaw rounds compatible metre heights to the region service's current
// eight-bit raw format. The float heights remain the authoritative import
// semantics; this quantization is only for the initial raw-file loader.
func ByteRaw(heights []float32) []byte {
	result := make([]byte, len(heights))
	for index, height := range heights {
		result[index] = byte(math.Round(float64(height)))
	}
	return result
}

// FloatRaw writes metre heights in the exact four-byte little-endian layout
// the region reads as a .r32 — the same one OpenSimulator calls RAW32. Unlike
// ByteRaw it loses nothing, which matters for a graded slope: rounding one to
// whole metres turns a constant angle into a staircase.
func FloatRaw(heights []float32) []byte {
	result := make([]byte, 4*len(heights))
	for index, height := range heights {
		binary.LittleEndian.PutUint32(result[4*index:], math.Float32bits(height))
	}
	return result
}

// GrayscaleImage encodes metre heights as the inverse of the compatible
// image-import rule. Loading the resulting PNG reconstructs the heights to the
// precision available in an eight-bit grayscale image.
func GrayscaleImage(heights []float32, width, height int) (*image.Gray, error) {
	if len(heights) != width*height {
		return nil, fmt.Errorf("got %d heights; expected %d", len(heights), width*height)
	}
	result := image.NewGray(image.Rect(0, 0, width, height))
	for terrainY := 0; terrainY < height; terrainY++ {
		imageY := height - terrainY - 1
		for x := 0; x < width; x++ {
			value := math.Round(float64(heights[terrainY*width+x]) * 255.0 / 128.0)
			value = math.Max(0, math.Min(255, value))
			result.SetGray(x, imageY, color.Gray{Y: byte(value)})
		}
	}
	return result, nil
}
