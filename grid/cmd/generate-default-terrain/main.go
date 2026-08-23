package main

import (
	"flag"
	"fmt"
	"image/png"
	"os"
	"path/filepath"

	"github.com/homeworldz/server/grid/internal/terrainimage"
)

func main() {
	output := flag.String("output", "assets/region/terrain", "terrain asset directory")
	flag.Parse()
	terrains := []struct {
		name    string
		heights []float32
	}{
		{"plateau-square", terrainimage.RoundedSquarePlateau()},
		{"plateau-round", terrainimage.RoundPlateau()},
	}
	for _, terrain := range terrains {
		if err := writeTerrain(*output, terrain.name, terrain.heights); err != nil {
			fmt.Fprintln(os.Stderr, "generate default terrain failed:", err)
			os.Exit(1)
		}
	}

	// Rectangular working surfaces for the macro regions of ADR 0036: flat at
	// 22 m, falling to zero over the outer 12 m. The two "join" variants keep
	// one half-edge flat where a neighbour abuts, so the shared border is
	// ground you walk rather than a trough you swim. The spans are in local
	// samples and follow from the pair's one-tile offset: Nova at (900,900)
	// meets Nova B at (899,899) along Nova's western half and Nova B's
	// eastern half.
	const (
		wide    = 512
		tall    = 256
		plateau = 22.0
		// The ramp must stay WALKABLE. 22 m over 8 m is 2.75 rise/run, which is
		// 70 degrees, against the region's 65-degree maxSlopeDegrees — so Jolt
		// reported OnSteepGround, gravity applied, and an avatar slid to the
		// bottom at x=0 and fell out of the world, oscillating between ground
		// level and tens of kilometres down (found live 2026-08-23, twice, and
		// flight could not escape it). 22 over 12 is 61.4 degrees, which clears
		// the limit with margin for the quantization the startup alignment
		// check already tolerates. Any change here wants that arithmetic
		// redone: plateau / ramp must stay under tan(65) = 2.14.
		ramp = 12.0
	)
	rectangles := []struct {
		name   string
		joined []terrainimage.Span
	}{
		{"plateau-512x256", nil},
		{"plateau-512x256-join-sw", []terrainimage.Span{{Edge: terrainimage.South, Begin: 0, End: 256}}},
		{"plateau-512x256-join-ne", []terrainimage.Span{{Edge: terrainimage.North, Begin: 256, End: 512}}},
	}
	for _, rectangle := range rectangles {
		heights := terrainimage.RampedPlateau(wide, tall, plateau, ramp, rectangle.joined)
		path := filepath.Join(*output, rectangle.name+".r32")
		if err := os.WriteFile(path, terrainimage.FloatRaw(heights), 0o644); err != nil {
			fmt.Fprintln(os.Stderr, "write rectangular terrain failed:", err)
			os.Exit(1)
		}
		fmt.Printf("Generated %s.\n", path)
	}
}

func writeTerrain(directory, name string, heights []float32) error {
	if err := os.MkdirAll(directory, 0o755); err != nil {
		return fmt.Errorf("create output directory: %w", err)
	}
	image, err := terrainimage.GrayscaleImage(
		heights, terrainimage.DefaultSize, terrainimage.DefaultSize)
	if err != nil {
		return err
	}
	imagePath := filepath.Join(directory, name+".png")
	file, err := os.Create(imagePath)
	if err != nil {
		return fmt.Errorf("create %s: %w", imagePath, err)
	}
	if err := png.Encode(file, image); err != nil {
		file.Close()
		return fmt.Errorf("encode %s: %w", imagePath, err)
	}
	if err := file.Close(); err != nil {
		return fmt.Errorf("close %s: %w", imagePath, err)
	}
	compatibleHeights, err := terrainimage.Heights(
		image, terrainimage.DefaultSize, terrainimage.DefaultSize)
	if err != nil {
		return err
	}
	rawPath := filepath.Join(directory, name+".raw")
	if err := os.WriteFile(rawPath, terrainimage.ByteRaw(compatibleHeights), 0o644); err != nil {
		return fmt.Errorf("write %s: %w", rawPath, err)
	}
	fmt.Printf("Generated %s and %s.\n", imagePath, rawPath)
	return nil
}
