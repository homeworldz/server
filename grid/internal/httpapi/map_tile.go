package httpapi

import (
	"bytes"
	"context"
	_ "embed"
	"encoding/binary"
	"encoding/json"
	"image"
	"image/color"
	"image/draw"
	"image/jpeg"
	_ "image/png"
	"io"
	"math"
	"net/http"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/homeworldz/server/grid/internal/regions"
)

//go:embed assets/default-map.jpg
var defaultMapTile []byte

var (
	decodedMapTile     image.Image
	decodedMapTileErr  error
	decodedMapTileOnce sync.Once
)

type cachedTerrainTile struct {
	image     image.Image
	expiresAt time.Time
}

type cachedTerrainLayers struct {
	layers    *terrainLayers
	expiresAt time.Time
}

// Layer settings are cached per region endpoint; layer colours are cached per
// asset id across every region, since a content-addressed texture averages to
// the same colour wherever it is used and most regions share the defaults.
type terrainLayerCache struct {
	mu      sync.Mutex
	entries map[string]cachedTerrainLayers
	colors  map[string]color.RGBA
}

func newTerrainLayerCache() terrainLayerCache {
	return terrainLayerCache{
		entries: make(map[string]cachedTerrainLayers),
		colors:  make(map[string]color.RGBA),
	}
}

type terrainTileCache struct {
	mu      sync.Mutex
	entries map[string]cachedTerrainTile
}

type mapRegion struct {
	region regions.Region
	sizeX  int
	sizeY  int
}

func newTerrainTileCache() terrainTileCache {
	return terrainTileCache{entries: make(map[string]cachedTerrainTile)}
}

func (a *API) mapTile(w http.ResponseWriter, r *http.Request) {
	if a.regions == nil {
		a.notFound(w, r)
		return
	}
	name := strings.TrimPrefix(r.URL.Path, "/map/")
	const prefix = "map-"
	const suffix = "-objects.jpg"
	if !strings.HasPrefix(name, prefix) || !strings.HasSuffix(name, suffix) {
		a.notFound(w, r)
		return
	}
	coordinates := strings.Split(strings.TrimSuffix(strings.TrimPrefix(name, prefix), suffix), "-")
	if len(coordinates) != 3 {
		a.notFound(w, r)
		return
	}
	level, levelErr := strconv.Atoi(coordinates[0])
	x, xErr := strconv.Atoi(coordinates[1])
	y, yErr := strconv.Atoi(coordinates[2])
	if levelErr != nil || level < 1 || level > 8 || xErr != nil || yErr != nil ||
		x < 0 || x > 65535 || y < 0 || y > 65535 {
		a.notFound(w, r)
		return
	}
	online, err := a.regions.List(r.Context())
	if err != nil {
		writeJSON(w, http.StatusInternalServerError, Error{Code: "region_store_error", Message: "map tile lookup failed"})
		return
	}
	span := 1 << (level - 1)
	mapRegions := make([]mapRegion, 0, len(online))
	terrain := make(map[string]image.Image)
	for _, region := range online {
		sizeX, sizeY := 1, 1
		if a.provisioned != nil {
			if provisioned, provisionedErr := a.provisioned.Get(r.Context(), region.ID); provisionedErr == nil {
				sizeX, sizeY = provisioned.SizeX, provisioned.SizeY
			}
		}
		mapped := mapRegion{region: region, sizeX: sizeX, sizeY: sizeY}
		mapRegions = append(mapRegions, mapped)
		if region.GridX >= x+span || region.GridX+sizeX <= x ||
			region.GridY >= y+span || region.GridY+sizeY <= y {
			continue
		}
		if tile, ok := a.regionTerrainTile(r.Context(), region, sizeX*256, sizeY*256); ok {
			terrain[region.ID] = tile
		}
	}
	tile, found, err := renderMapTile(level, x, y, mapRegions, terrain)
	if err != nil {
		writeJSON(w, http.StatusInternalServerError, Error{Code: "map_tile_error", Message: "map tile rendering failed"})
		return
	}
	if !found {
		a.notFound(w, r)
		return
	}
	w.Header().Set("Content-Type", "image/jpeg")
	w.Header().Set("Cache-Control", "public, max-age=60")
	w.WriteHeader(http.StatusOK)
	_, _ = w.Write(tile)
}

func renderMapTile(level, tileX, tileY int, online []mapRegion, terrain map[string]image.Image) ([]byte, bool, error) {
	span := 1 << (level - 1)
	matching := make([]mapRegion, 0)
	for _, mapped := range online {
		region := mapped.region
		if region.GridX < tileX+span && region.GridX+mapped.sizeX > tileX &&
			region.GridY < tileY+span && region.GridY+mapped.sizeY > tileY {
			matching = append(matching, mapped)
		}
	}
	if len(matching) == 0 {
		return nil, false, nil
	}
	decodedMapTileOnce.Do(func() {
		decodedMapTile, decodedMapTileErr = jpeg.Decode(bytes.NewReader(defaultMapTile))
	})
	if decodedMapTileErr != nil {
		return nil, false, decodedMapTileErr
	}
	const size = 256
	result := image.NewRGBA(image.Rect(0, 0, size, size))
	draw.Draw(result, result.Bounds(), &image.Uniform{C: color.RGBA{R: 36, G: 87, B: 122, A: 255}}, image.Point{}, draw.Src)
	for _, mapped := range matching {
		region := mapped.region
		source := decodedMapTile
		if tile := terrain[region.ID]; tile != nil {
			source = tile
		}
		left := (region.GridX - tileX) * size / span
		right := (region.GridX + mapped.sizeX - tileX) * size / span
		top := (tileY + span - region.GridY - mapped.sizeY) * size / span
		bottom := (tileY + span - region.GridY) * size / span
		clippedLeft, clippedRight := max(0, left), min(size, right)
		clippedTop, clippedBottom := max(0, top), min(size, bottom)
		sourceBounds := source.Bounds()
		for pixelY := clippedTop; pixelY < clippedBottom; pixelY++ {
			for pixelX := clippedLeft; pixelX < clippedRight; pixelX++ {
				sourceX := sourceBounds.Min.X + (pixelX-left)*sourceBounds.Dx()/(right-left)
				sourceY := sourceBounds.Min.Y + (pixelY-top)*sourceBounds.Dy()/(bottom-top)
				result.Set(pixelX, pixelY, source.At(sourceX, sourceY))
			}
		}
	}
	return encodeJPEG(result)
}

func encodeJPEG(source image.Image) ([]byte, bool, error) {
	var encoded bytes.Buffer
	if err := jpeg.Encode(&encoded, source, &jpeg.Options{Quality: 90}); err != nil {
		return nil, false, err
	}
	return encoded.Bytes(), true, nil
}

func (a *API) regionTerrainTile(ctx context.Context, region regions.Region, width, height int) (image.Image, bool) {
	if a.terrainHTTP == nil || region.PublicEndpoint == "" {
		return nil, false
	}
	endpoint := strings.TrimRight(region.PublicEndpoint, "/") + "/map/terrain.raw"
	cacheKey := endpoint + "#" + strconv.Itoa(width) + "x" + strconv.Itoa(height)
	now := time.Now()
	a.terrainCache.mu.Lock()
	if cached, ok := a.terrainCache.entries[cacheKey]; ok && cached.expiresAt.After(now) {
		a.terrainCache.mu.Unlock()
		return cached.image, cached.image != nil
	}
	a.terrainCache.mu.Unlock()

	request, err := http.NewRequestWithContext(ctx, http.MethodGet, endpoint, nil)
	if err != nil {
		return nil, false
	}
	request.Header.Set("Authorization", "Bearer "+a.serviceToken)
	response, err := a.terrainHTTP.Do(request)
	if err != nil {
		a.cacheTerrainTile(cacheKey, nil, now.Add(5*time.Second))
		return nil, false
	}
	defer response.Body.Close()
	byteCount := width * height * 4
	body, err := io.ReadAll(io.LimitReader(response.Body, int64(byteCount+1)))
	if err != nil || response.StatusCode != http.StatusOK || len(body) != byteCount {
		a.cacheTerrainTile(cacheKey, nil, now.Add(5*time.Second))
		return nil, false
	}
	tile := renderTerrainHeightmap(body, width, height, a.regionTerrainLayers(ctx, region))
	a.cacheTerrainTile(cacheKey, tile, now.Add(60*time.Second))
	return tile, true
}

// terrainLayers is what a region says its ground is made of: the four layer
// textures, the start height and height range that select between them per
// corner, and the water line. Served by the region beside terrain.raw, because
// an operator can change any of it through the Terrain tab and a heightmap
// alone cannot be coloured without it.
type terrainLayers struct {
	Assets      [4]string  `json:"assets"`
	StartHeight [4]float64 `json:"startHeight"`
	HeightRange [4]float64 `json:"heightRange"`
	WaterHeight float64    `json:"waterHeight"`
	palette     [4]color.RGBA
}

// regionTerrainLayers reads a region's live layer settings and resolves each
// layer texture to one representative colour. Nil when the region cannot be
// asked or its textures cannot be read, which leaves the caller on the height
// palette rather than inventing ground.
func (a *API) regionTerrainLayers(ctx context.Context, region regions.Region) *terrainLayers {
	if a.terrainHTTP == nil || region.PublicEndpoint == "" {
		return nil
	}
	endpoint := strings.TrimRight(region.PublicEndpoint, "/") + "/map/terrain-layers.json"
	now := time.Now()
	a.layerCache.mu.Lock()
	if cached, ok := a.layerCache.entries[endpoint]; ok && cached.expiresAt.After(now) {
		a.layerCache.mu.Unlock()
		return cached.layers
	}
	a.layerCache.mu.Unlock()

	request, err := http.NewRequestWithContext(ctx, http.MethodGet, endpoint, nil)
	if err != nil {
		return nil
	}
	request.Header.Set("Authorization", "Bearer "+a.serviceToken)
	response, err := a.terrainHTTP.Do(request)
	if err != nil {
		a.cacheTerrainLayers(endpoint, nil, now.Add(5*time.Second))
		return nil
	}
	defer response.Body.Close()
	body, err := io.ReadAll(io.LimitReader(response.Body, 8192))
	if err != nil || response.StatusCode != http.StatusOK {
		a.cacheTerrainLayers(endpoint, nil, now.Add(5*time.Second))
		return nil
	}
	layers := &terrainLayers{}
	if err := json.Unmarshal(body, layers); err != nil {
		a.cacheTerrainLayers(endpoint, nil, now.Add(60*time.Second))
		return nil
	}
	for index, asset := range layers.Assets {
		resolved, ok := a.layerColor(ctx, asset)
		if !ok {
			// One unreadable layer makes the whole palette wrong rather than
			// slightly wrong: the missing band would be drawn in whatever the
			// zero value is. Better to stay on the height palette and be
			// uniformly approximate than to publish one false band.
			a.cacheTerrainLayers(endpoint, nil, now.Add(60*time.Second))
			return nil
		}
		layers.palette[index] = resolved
	}
	// Same freshness as the heightmap beside it. Layer settings change rarely,
	// but they change by an operator editing the Terrain tab and then looking
	// at the map — and a ten minute cache, which is what this held first, makes
	// that edit look like it did nothing. The costly half is the texture
	// averaging, and that is cached per asset id below and survives this
	// expiring, so re-reading the settings every minute is a JSON fetch.
	a.cacheTerrainLayers(endpoint, layers, now.Add(60*time.Second))
	return layers
}

func (a *API) cacheTerrainLayers(endpoint string, layers *terrainLayers, expiresAt time.Time) {
	a.layerCache.mu.Lock()
	defer a.layerCache.mu.Unlock()
	a.layerCache.entries[endpoint] = cachedTerrainLayers{layers: layers, expiresAt: expiresAt}
}

// layerColor reduces a ground texture to the one colour a 256-pixel map tile
// can show of it. A region pixel covers a metre and the texture repeats many
// times within that, so the mean is what a viewer's ground averages to at map
// zoom — sampling a single texel would pick out whichever blade of grass it
// landed on.
func (a *API) layerColor(ctx context.Context, assetID string) (color.RGBA, bool) {
	if a.assets == nil || a.vault == nil || !validUUID(assetID) {
		return color.RGBA{}, false
	}
	a.layerCache.mu.Lock()
	if cached, ok := a.layerCache.colors[assetID]; ok {
		a.layerCache.mu.Unlock()
		return cached, true
	}
	a.layerCache.mu.Unlock()

	blob, err := a.assets.Blob(ctx, assetID)
	if err != nil {
		return color.RGBA{}, false
	}
	content, _, err := a.vault.Open(ctx, blob.BlobID)
	if err != nil {
		return color.RGBA{}, false
	}
	defer content.Close()
	decoded, _, err := image.Decode(io.LimitReader(content, 32<<20))
	if err != nil {
		return color.RGBA{}, false
	}
	bounds := decoded.Bounds()
	if bounds.Empty() {
		return color.RGBA{}, false
	}
	// Every 8th pixel in each direction: a 1024x1024 canonical averages the
	// same to three decimal places at that stride and costs a sixty-fourth of
	// the work, on a path a map tile request waits for.
	const stride = 8
	var sumR, sumG, sumB, count uint64
	for y := bounds.Min.Y; y < bounds.Max.Y; y += stride {
		for x := bounds.Min.X; x < bounds.Max.X; x += stride {
			r, g, b, _ := decoded.At(x, y).RGBA()
			sumR += uint64(r >> 8)
			sumG += uint64(g >> 8)
			sumB += uint64(b >> 8)
			count++
		}
	}
	if count == 0 {
		return color.RGBA{}, false
	}
	resolved := color.RGBA{
		R: uint8(sumR / count), G: uint8(sumG / count), B: uint8(sumB / count), A: 255}
	a.layerCache.mu.Lock()
	a.layerCache.colors[assetID] = resolved
	a.layerCache.mu.Unlock()
	return resolved, true
}

// bilinear reads a per-corner value at a position across the region. Corners
// arrive south-west, north-west, south-east, north-east — the order the
// RegionHandshake writes them.
func bilinear(corners [4]float64, u, v float64) float64 {
	south := corners[0] + (corners[2]-corners[0])*u
	north := corners[1] + (corners[3]-corners[1])*u
	return south + (north-south)*v
}

// layerColorAt applies the viewer's own composition rule
// (llvlcomposition.cpp, restated in region/include/homeworldz/terrain_layers.h):
//
//	t = clamp((height - start) * 4 / range, 0, 3)
//
// with layer 1 pure at t = 0, layer 4 at t = 3, and linear blending between
// neighbours. The viewer adds a turbulence term before this that no one has
// reproduced outside Linden, so a tile matches the ground's bands but not its
// patchiness — which is the right trade at 256 pixels for a region.
func layerColorAt(layers *terrainLayers, height, u, v float64) color.RGBA {
	start := bilinear(layers.StartHeight, u, v)
	span := bilinear(layers.HeightRange, u, v)
	if span <= 0 {
		return layers.palette[0]
	}
	t := (height - start) * 4 / span
	t = math.Min(3, math.Max(0, t))
	lower := int(t)
	if lower > 2 {
		return layers.palette[3]
	}
	blend := t - float64(lower)
	from, to := layers.palette[lower], layers.palette[lower+1]
	mix := func(a, b uint8) uint8 {
		return uint8(math.Round(float64(a) + (float64(b)-float64(a))*blend))
	}
	return color.RGBA{R: mix(from.R, to.R), G: mix(from.G, to.G), B: mix(from.B, to.B), A: 255}
}

func (a *API) cacheTerrainTile(endpoint string, tile image.Image, expiresAt time.Time) {
	a.terrainCache.mu.Lock()
	defer a.terrainCache.mu.Unlock()
	a.terrainCache.entries[endpoint] = cachedTerrainTile{image: tile, expiresAt: expiresAt}
}

func renderTerrainHeightmap(data []byte, width, height int, layers *terrainLayers) image.Image {
	heights := make([]float64, width*height)
	for index := range heights {
		heights[index] = float64(math.Float32frombits(binary.LittleEndian.Uint32(data[index*4:])))
	}
	result := image.NewRGBA(image.Rect(0, 0, width, height))
	for outputY := 0; outputY < height; outputY++ {
		y := height - 1 - outputY
		for x := 0; x < width; x++ {
			sample := heights[y*width+x]
			// The region's own ground when it will say what that is, and the
			// fixed height palette when it will not. The palette was every
			// region's map for as long as it was the only thing here: it puts
			// grass at any moderate height, so a region whose layers start
			// above that drew green where a viewer draws dirt.
			base := terrainColor(sample)
			if layers != nil {
				if sample <= layers.WaterHeight {
					base = waterColor(sample, layers.WaterHeight)
				} else {
					base = layerColorAt(layers, sample,
						float64(x)/float64(width-1), float64(y)/float64(height-1))
				}
			}
			left, right := max(0, x-1), min(width-1, x+1)
			down, up := max(0, y-1), min(height-1, y+1)
			dx := heights[y*width+right] - heights[y*width+left]
			dy := heights[up*width+x] - heights[down*width+x]
			shade := 0.9 + 0.18*(-dx+dy)/math.Sqrt(dx*dx+dy*dy+4)
			result.SetRGBA(x, outputY, color.RGBA{
				R: shadeChannel(base.R, shade), G: shadeChannel(base.G, shade),
				B: shadeChannel(base.B, shade), A: 255})
		}
	}
	return result
}

// waterColor darkens with depth below a region's own water line, rather than
// the 20 metres the fixed palette assumed every region used.
func waterColor(height, waterHeight float64) color.RGBA {
	span := waterHeight
	if span <= 0 {
		span = 20
	}
	depth := min(1, max(0, (waterHeight-height)/span))
	return color.RGBA{R: uint8(35 - 12*depth), G: uint8(105 - 35*depth), B: uint8(145 - 25*depth), A: 255}
}

func terrainColor(height float64) color.RGBA {
	if height <= 20 {
		depth := min(1, max(0, (20-height)/20))
		return color.RGBA{R: uint8(35 - 12*depth), G: uint8(105 - 35*depth), B: uint8(145 - 25*depth), A: 255}
	}
	if height < 22 {
		return color.RGBA{R: 194, G: 178, B: 128, A: 255}
	}
	if height < 45 {
		return color.RGBA{R: 78, G: 132, B: 70, A: 255}
	}
	if height < 80 {
		return color.RGBA{R: 112, G: 108, B: 91, A: 255}
	}
	return color.RGBA{R: 205, G: 207, B: 201, A: 255}
}

func shadeChannel(value uint8, shade float64) uint8 {
	return uint8(min(255, max(0, math.Round(float64(value)*shade))))
}
