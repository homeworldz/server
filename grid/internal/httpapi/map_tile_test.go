package httpapi

import (
	"bytes"
	"context"
	"encoding/binary"
	"image"
	"image/color"
	"image/jpeg"
	"math"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"sync/atomic"
	"testing"
	"time"

	"github.com/homeworldz/server/grid/internal/provisioning"
	"github.com/homeworldz/server/grid/internal/regions"
)

func encodedHeightmapSize(width int, height float32) []byte {
	result := make([]byte, width*width*4)
	bits := math.Float32bits(height)
	for offset := 0; offset < len(result); offset += 4 {
		binary.LittleEndian.PutUint32(result[offset:], bits)
	}
	return result
}

func encodedHeightmap(height float32) []byte { return encodedHeightmapSize(256, height) }

func TestTerrainHeightmapUsesNorthAtTop(t *testing.T) {
	data := encodedHeightmap(20)
	binary.LittleEndian.PutUint32(data[(255*256+10)*4:], math.Float32bits(30))
	tile := renderTerrainHeightmap(data, 256, 256, nil)
	north := tile.At(10, 0)
	south := tile.At(10, 255)
	_, northGreen, _, _ := north.RGBA()
	_, southGreen, _, _ := south.RGBA()
	if northGreen <= southGreen {
		t.Fatalf("north green = %d, south green = %d; terrain row orientation was lost", northGreen, southGreen)
	}
}

func TestMapTileSlicesLargeRegionAcrossGridCells(t *testing.T) {
	terrain := image.NewRGBA(image.Rect(0, 0, 512, 512))
	for y := 0; y < 512; y++ {
		for x := 0; x < 512; x++ {
			if x < 256 {
				terrain.SetRGBA(x, y, color.RGBA{R: 240, A: 255})
			} else {
				terrain.SetRGBA(x, y, color.RGBA{B: 240, A: 255})
			}
		}
	}
	region := regions.Region{ID: "large", GridX: 1000, GridY: 1000}
	mapped := []mapRegion{{region: region, sizeX: 2, sizeY: 2}}
	tiles := map[string]image.Image{"large": terrain}
	westBytes, westFound, westErr := renderMapTile(1, 1000, 1000, mapped, tiles)
	eastBytes, eastFound, eastErr := renderMapTile(1, 1001, 1000, mapped, tiles)
	if westErr != nil || eastErr != nil || !westFound || !eastFound {
		t.Fatalf("large map slices failed: west=%v/%v east=%v/%v", westFound, westErr, eastFound, eastErr)
	}
	west, err := jpeg.Decode(bytes.NewReader(westBytes))
	if err != nil {
		t.Fatal(err)
	}
	east, err := jpeg.Decode(bytes.NewReader(eastBytes))
	if err != nil {
		t.Fatal(err)
	}
	westRed, _, westBlue, _ := west.At(128, 128).RGBA()
	eastRed, _, eastBlue, _ := east.At(128, 128).RGBA()
	if westRed <= westBlue || eastBlue <= eastRed {
		t.Fatalf("large map slices lost orientation: west=(%d,%d) east=(%d,%d)",
			westRed, westBlue, eastRed, eastBlue)
	}
}

func TestMapTileSlicesFourByFourRegionAtOppositeCorners(t *testing.T) {
	terrain := image.NewRGBA(image.Rect(0, 0, 1024, 1024))
	for y := 0; y < 1024; y++ {
		for x := 0; x < 1024; x++ {
			pixel := color.RGBA{G: 240, A: 255}
			if x < 256 && y >= 768 {
				pixel = color.RGBA{R: 240, A: 255}
			} else if x >= 768 && y < 256 {
				pixel = color.RGBA{B: 240, A: 255}
			}
			terrain.SetRGBA(x, y, pixel)
		}
	}
	region := regions.Region{ID: "large", GridX: 1000, GridY: 1000}
	mapped := []mapRegion{{region: region, sizeX: 4, sizeY: 4}}
	tiles := map[string]image.Image{"large": terrain}
	southwestBytes, southwestFound, southwestErr := renderMapTile(1, 1000, 1000, mapped, tiles)
	northeastBytes, northeastFound, northeastErr := renderMapTile(1, 1003, 1003, mapped, tiles)
	if southwestErr != nil || northeastErr != nil || !southwestFound || !northeastFound {
		t.Fatalf("4x4 map slices failed: southwest=%v/%v northeast=%v/%v",
			southwestFound, southwestErr, northeastFound, northeastErr)
	}
	southwest, err := jpeg.Decode(bytes.NewReader(southwestBytes))
	if err != nil {
		t.Fatal(err)
	}
	northeast, err := jpeg.Decode(bytes.NewReader(northeastBytes))
	if err != nil {
		t.Fatal(err)
	}
	southwestRed, southwestGreen, southwestBlue, _ := southwest.At(128, 128).RGBA()
	northeastRed, northeastGreen, northeastBlue, _ := northeast.At(128, 128).RGBA()
	if southwestRed <= southwestGreen || southwestRed <= southwestBlue ||
		northeastBlue <= northeastRed || northeastBlue <= northeastGreen {
		t.Fatalf("4x4 map corners lost orientation: southwest=(%d,%d,%d) northeast=(%d,%d,%d)",
			southwestRed, southwestGreen, southwestBlue, northeastRed, northeastGreen, northeastBlue)
	}
}

func TestMapTileFetchesAndCachesLiveRegionTerrain(t *testing.T) {
	var requests atomic.Int32
	heightmap := encodedHeightmap(90)
	regionServer := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		// Only the heightmap is counted, and only it is served: a region that
		// does not publish terrain layers is a supported case, and the tile
		// falls back to the height palette these assertions describe.
		if r.URL.Path != "/map/terrain.raw" {
			http.NotFound(w, r)
			return
		}
		requests.Add(1)
		if r.Header.Get("Authorization") != "Bearer secret" {
			http.Error(w, "unauthorized", http.StatusUnauthorized)
			return
		}
		w.Header().Set("Content-Type", "application/vnd.homeworldz.heightmap-f32le")
		_, _ = w.Write(heightmap)
	}))
	defer regionServer.Close()

	store := newMemoryRegionStore()
	_, err := store.RegisterProvisioned(context.Background(),
		"11111111-1111-4111-8111-111111111111", regions.Registration{
			Name: "Welcome", GridX: 1000, GridY: 1000, PublicEndpoint: regionServer.URL,
			ViewerPort: 42002, LeaseDuration: time.Minute,
		})
	if err != nil {
		t.Fatal(err)
	}
	handler := New(checker{}, "test", Options{
		ServiceToken: "secret", Regions: store, TerrainHTTPClient: regionServer.Client(),
	})
	for attempt := 0; attempt < 2; attempt++ {
		request := httptest.NewRequest(http.MethodGet, "/map/map-1-1000-1000-objects.jpg", nil)
		response := httptest.NewRecorder()
		handler.ServeHTTP(response, request)
		if response.Code != http.StatusOK {
			t.Fatalf("map status = %d", response.Code)
		}
		decoded, err := jpeg.Decode(response.Body)
		if err != nil {
			t.Fatal(err)
		}
		red, green, blue, _ := decoded.At(128, 128).RGBA()
		if red < 40000 || green < 40000 || blue < 40000 {
			t.Fatalf("high terrain pixel = (%d, %d, %d), want light mountain", red, green, blue)
		}
	}
	if requests.Load() != 1 {
		t.Fatalf("heightmap requests = %d, want one cached fetch", requests.Load())
	}
}

func TestTerrainCacheSeparatesRegionWidths(t *testing.T) {
	var requests atomic.Int32
	regionServer := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		// Width is chosen by which heightmap request this is, so only heightmap
		// requests may be counted; the layer endpoint would otherwise shift the
		// widths this test is about.
		if r.URL.Path != "/map/terrain.raw" {
			http.NotFound(w, r)
			return
		}
		request := requests.Add(1)
		width := 256
		if request == 2 {
			width = 512
		}
		_, _ = w.Write(encodedHeightmapSize(width, 20))
	}))
	defer regionServer.Close()

	api := &API{terrainHTTP: regionServer.Client(), terrainCache: newTerrainTileCache(),
		layerCache: newTerrainLayerCache()}
	region := regions.Region{PublicEndpoint: regionServer.URL}
	first, ok := api.regionTerrainTile(context.Background(), region, 256, 256)
	if !ok || first.Bounds().Dx() != 256 {
		t.Fatalf("first terrain tile = %v/%d, want true/256", ok, first.Bounds().Dx())
	}
	second, ok := api.regionTerrainTile(context.Background(), region, 512, 512)
	if !ok || second.Bounds().Dx() != 512 {
		t.Fatalf("second terrain tile = %v/%d, want true/512", ok, second.Bounds().Dx())
	}
	if requests.Load() != 2 {
		t.Fatalf("heightmap requests = %d, want one fetch per region width", requests.Load())
	}
}

func TestMapTileFetchesLargeRegionTerrainSlice(t *testing.T) {
	heightmap := encodedHeightmapSize(512, 20)
	for y := 0; y < 512; y++ {
		for x := 256; x < 512; x++ {
			binary.LittleEndian.PutUint32(heightmap[(y*512+x)*4:], math.Float32bits(90))
		}
	}
	regionServer := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		_, _ = w.Write(heightmap)
	}))
	defer regionServer.Close()

	const id = "11111111-1111-4111-8111-111111111111"
	store := newMemoryRegionStore()
	_, err := store.RegisterProvisioned(context.Background(), id, regions.Registration{
		Name: "Large", GridX: 1000, GridY: 1000, PublicEndpoint: regionServer.URL,
		ViewerPort: 42002, LeaseDuration: time.Minute,
	})
	if err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(t.TempDir(), "regions.json")
	if err := os.WriteFile(path, []byte(`[{"id":"`+id+`","name":"Large","mapX":1000,"mapY":1000,"size":2,"accessKey":"large-key"}]`), 0600); err != nil {
		t.Fatal(err)
	}
	provisioned, err := provisioning.Load(path)
	if err != nil {
		t.Fatal(err)
	}
	handler := New(checker{}, "test", Options{ServiceToken: "secret", Regions: store,
		Provisioned: provisioned, TerrainHTTPClient: regionServer.Client()})
	request := httptest.NewRequest(http.MethodGet, "/map/map-1-1001-1000-objects.jpg", nil)
	response := httptest.NewRecorder()
	handler.ServeHTTP(response, request)
	if response.Code != http.StatusOK {
		t.Fatalf("large map status = %d: %s", response.Code, response.Body.String())
	}
	decoded, err := jpeg.Decode(response.Body)
	if err != nil {
		t.Fatal(err)
	}
	red, green, blue, _ := decoded.At(128, 128).RGBA()
	if red < 40000 || green < 40000 || blue < 40000 {
		t.Fatalf("large eastern terrain pixel = (%d, %d, %d), want light mountain", red, green, blue)
	}
}

// The layer bands a region publishes, applied the way the viewer applies them.
// Welcome's live settings are start 20, range 60, which puts the first boundary
// at 27.5m — so its mostly-flat 22m ground is nearly pure layer 1. The fixed
// height palette this replaced called anything above 22m grass, which is why a
// region drew green where a viewer drew dirt.
func TestTerrainLayerBandsFollowCompositionRule(t *testing.T) {
	layers := &terrainLayers{
		StartHeight: [4]float64{20, 20, 20, 20},
		HeightRange: [4]float64{60, 60, 60, 60},
		WaterHeight: 20,
		palette: [4]color.RGBA{
			{R: 10, A: 255}, {R: 20, A: 255}, {R: 30, A: 255}, {R: 40, A: 255}},
	}
	for _, testCase := range []struct {
		name   string
		height float64
		want   uint8
	}{
		// t = 0: layer 1 pure, at the start height itself.
		{"at start", 20, 10},
		// Boundaries sit where t is a half integer: start + 0.125*range and so
		// on. Exactly there the two neighbours are evenly mixed.
		{"first boundary", 27.5, 15},
		{"second boundary", 42.5, 25},
		{"third boundary", 57.5, 35},
		// Layer 4 is pure from start + 0.75*range upward and stays there.
		{"fourth pure", 65, 40},
		{"above everything", 400, 40},
	} {
		got := layerColorAt(layers, testCase.height, 0.5, 0.5)
		if got.R != testCase.want {
			t.Errorf("%s (%.1fm): R = %d, want %d", testCase.name, testCase.height, got.R, testCase.want)
		}
	}
}

// Corner values are interpolated bilinearly, in the order the RegionHandshake
// writes them: south-west, north-west, south-east, north-east. Reading them in
// another order tilts every region's bands the wrong way, which no single-corner
// test would catch.
func TestTerrainLayerCornersInterpolateBilinearly(t *testing.T) {
	corners := [4]float64{0, 10, 20, 30} // SW, NW, SE, NE
	for _, testCase := range []struct {
		name, at string
		u, v     float64
		want     float64
	}{
		{"south-west", "0,0", 0, 0, 0},
		{"north-west", "0,1", 0, 1, 10},
		{"south-east", "1,0", 1, 0, 20},
		{"north-east", "1,1", 1, 1, 30},
		{"centre", "0.5,0.5", 0.5, 0.5, 15},
	} {
		if got := bilinear(corners, testCase.u, testCase.v); got != testCase.want {
			t.Errorf("%s at %s: %v, want %v", testCase.name, testCase.at, got, testCase.want)
		}
	}
}
