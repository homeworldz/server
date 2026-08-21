package provisioning

import (
	"bytes"
	"context"
	"crypto/subtle"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/url"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
	"sync"
)

var uuidPattern = regexp.MustCompile(`^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[1-5][0-9a-fA-F]{3}-[89abAB][0-9a-fA-F]{3}-[0-9a-fA-F]{12}$`)

var (
	ErrNotFound = errors.New("provisioned region not found")
	ErrConflict = errors.New("provisioned region conflicts with an existing region")
	ErrInvalid  = errors.New("provisioned region is invalid")
)

type Region struct {
	ID          string `json:"id"`
	Name        string `json:"name"`
	OwnerUserID string `json:"ownerUserId,omitempty"`
	MapX        int    `json:"mapX"`
	MapY        int    `json:"mapY"`
	// SizeX by SizeY tiles (each tile 256 m). The shape rule (ADR 0036): the
	// shorter dimension is a proven square viewer size (1, 2, or 4) and divides
	// the longer, so square facets of the shorter edge tile the shape exactly.
	SizeX    int `json:"sizeX"`
	SizeY    int `json:"sizeY"`
	Maturity int `json:"maturity"`
	// FacetNames names facets 1..N-1 of a rectangle, in map-coordinate order;
	// facet 0 carries the region's own name. Empty for square regions.
	FacetNames     []string `json:"facetNames,omitempty"`
	PublicEndpoint string   `json:"publicEndpoint,omitempty"`
	ViewerPort     int      `json:"viewerPort,omitempty"`
	Enabled        bool     `json:"enabled"`
	Kind           string   `json:"kind,omitempty"`
	Tags           string   `json:"tags,omitempty"`
	AccessKey      string   `json:"-"`
}

type Update struct {
	Name           *string
	OwnerUserID    *string
	MapX           *int
	MapY           *int
	SizeX          *int
	SizeY          *int
	FacetNames     *[]string
	Maturity       *int
	PublicEndpoint *string
	ViewerPort     *int
	Enabled        *bool
	Kind           *string
	Tags           *string
}

type fileRegion struct {
	ID          string `json:"id"`
	Name        string `json:"name"`
	OwnerUserID string `json:"ownerUserId,omitempty"`
	MapX        int    `json:"mapX"`
	MapY        int    `json:"mapY"`
	// Size is the square shorthand older registry files carry; sizeX/sizeY
	// supersede it and win when both are present.
	Size           *int     `json:"size,omitempty"`
	SizeX          *int     `json:"sizeX,omitempty"`
	SizeY          *int     `json:"sizeY,omitempty"`
	FacetNames     []string `json:"facetNames,omitempty"`
	Maturity       int      `json:"maturity,omitempty"`
	PublicEndpoint string   `json:"publicEndpoint,omitempty"`
	ViewerPort     int      `json:"viewerPort,omitempty"`
	Enabled        *bool    `json:"enabled,omitempty"`
	Kind           string   `json:"kind,omitempty"`
	Tags           string   `json:"tags,omitempty"`
	AccessKey      string   `json:"accessKey"`
}

type Registry struct {
	mu   sync.RWMutex
	path string
	byID map[string]Region
}

type Store interface {
	// Authenticate answers "which region is this", not "may it run": a
	// disabled region still authenticates, so that it can release its lease.
	Authenticate(context.Context, string, string) (Region, bool)
	List(context.Context) ([]Region, error)
	Get(context.Context, string) (Region, error)
	Create(context.Context, Region) (Region, error)
	Update(context.Context, string, Update) (Region, error)
	RotateAccessKey(context.Context, string, string) (Region, error)
	Delete(context.Context, string) error
}

func Load(path string) (*Registry, error) {
	content, err := os.ReadFile(path)
	if err != nil {
		return nil, fmt.Errorf("read provisioned regions: %w", err)
	}
	var stored []fileRegion
	decoder := json.NewDecoder(bytes.NewReader(content))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&stored); err != nil {
		return nil, fmt.Errorf("decode provisioned regions: %w", err)
	}
	if err := decoder.Decode(&struct{}{}); err != io.EOF {
		return nil, fmt.Errorf("decode provisioned regions: file must contain one JSON array")
	}
	items := make(map[string]Region, len(stored))
	for index, item := range stored {
		enabled := true
		sizeX, sizeY := 1, 1
		if item.Enabled != nil {
			enabled = *item.Enabled
		}
		if item.Size != nil {
			sizeX, sizeY = *item.Size, *item.Size
		}
		if item.SizeX != nil {
			sizeX = *item.SizeX
		}
		if item.SizeY != nil {
			sizeY = *item.SizeY
		}
		kind := item.Kind
		if kind == "" {
			kind = "user"
		}
		region := Region{ID: item.ID, Name: item.Name, OwnerUserID: item.OwnerUserID,
			MapX: item.MapX, MapY: item.MapY, SizeX: sizeX, SizeY: sizeY, Maturity: item.Maturity,
			FacetNames:     item.FacetNames,
			PublicEndpoint: item.PublicEndpoint,
			ViewerPort:     item.ViewerPort, Enabled: enabled, Kind: kind, Tags: item.Tags,
			AccessKey: item.AccessKey}
		if err := validate(region); err != nil {
			return nil, fmt.Errorf("invalid provisioned region at index %d: %w", index, err)
		}
		if _, exists := items[region.ID]; exists {
			return nil, fmt.Errorf("duplicate provisioned region id %q", region.ID)
		}
		items[region.ID] = region
	}
	if err := validateUnique(items, ""); err != nil {
		return nil, err
	}
	return &Registry{path: path, byID: items}, nil
}

func (r *Registry) Authenticate(_ context.Context, id, accessKey string) (Region, bool) {
	if r == nil {
		return Region{}, false
	}
	r.mu.RLock()
	defer r.mu.RUnlock()
	region, found := r.byID[id]
	if !found {
		for _, candidate := range r.byID {
			if strings.EqualFold(candidate.Name, id) {
				region, found = candidate, true
				break
			}
		}
	}
	// Identity only; `enabled` is the caller's question. See the note on
	// PostgresStore.Authenticate.
	if !found || subtle.ConstantTimeCompare([]byte(region.AccessKey), []byte(accessKey)) != 1 {
		return Region{}, false
	}
	return region, true
}

func (r *Registry) List(_ context.Context) ([]Region, error) {
	if r == nil {
		return nil, nil
	}
	r.mu.RLock()
	defer r.mu.RUnlock()
	items := make([]Region, 0, len(r.byID))
	for _, item := range r.byID {
		items = append(items, item)
	}
	sort.Slice(items, func(i, j int) bool {
		if items[i].MapY != items[j].MapY {
			return items[i].MapY < items[j].MapY
		}
		if items[i].MapX != items[j].MapX {
			return items[i].MapX < items[j].MapX
		}
		return items[i].ID < items[j].ID
	})
	return items, nil
}

func (r *Registry) Get(_ context.Context, id string) (Region, error) {
	if r == nil {
		return Region{}, ErrNotFound
	}
	r.mu.RLock()
	defer r.mu.RUnlock()
	item, found := r.byID[id]
	if !found {
		return Region{}, ErrNotFound
	}
	return item, nil
}

func (r *Registry) Create(_ context.Context, item Region) (Region, error) {
	if r == nil {
		return Region{}, ErrNotFound
	}
	item.Name = strings.TrimSpace(item.Name)
	item.OwnerUserID = strings.TrimSpace(item.OwnerUserID)
	item.PublicEndpoint = strings.TrimSpace(item.PublicEndpoint)
	item.FacetNames = trimmedNames(item.FacetNames)
	if item.SizeX == 0 {
		item.SizeX = 1
	}
	if item.SizeY == 0 {
		item.SizeY = 1
	}
	if item.Kind == "" {
		item.Kind = "user"
	}
	if err := validate(item); err != nil {
		return Region{}, err
	}
	r.mu.Lock()
	defer r.mu.Unlock()
	if _, found := r.byID[item.ID]; found {
		return Region{}, ErrConflict
	}
	next := clone(r.byID)
	next[item.ID] = item
	if err := validateUnique(next, item.ID); err != nil {
		return Region{}, err
	}
	if err := r.persist(next); err != nil {
		return Region{}, err
	}
	r.byID = next
	return item, nil
}

func (r *Registry) Update(_ context.Context, id string, update Update) (Region, error) {
	if r == nil {
		return Region{}, ErrNotFound
	}
	r.mu.Lock()
	defer r.mu.Unlock()
	item, found := r.byID[id]
	if !found {
		return Region{}, ErrNotFound
	}
	if update.Name != nil {
		item.Name = strings.TrimSpace(*update.Name)
	}
	if update.OwnerUserID != nil {
		item.OwnerUserID = strings.TrimSpace(*update.OwnerUserID)
	}
	if update.MapX != nil {
		item.MapX = *update.MapX
	}
	if update.MapY != nil {
		item.MapY = *update.MapY
	}
	if update.SizeX != nil {
		item.SizeX = *update.SizeX
	}
	if update.SizeY != nil {
		item.SizeY = *update.SizeY
	}
	if update.FacetNames != nil {
		item.FacetNames = trimmedNames(*update.FacetNames)
	}
	if update.Maturity != nil {
		item.Maturity = *update.Maturity
	}
	if update.PublicEndpoint != nil {
		item.PublicEndpoint = strings.TrimSpace(*update.PublicEndpoint)
	}
	if update.ViewerPort != nil {
		item.ViewerPort = *update.ViewerPort
	}
	if update.Enabled != nil {
		item.Enabled = *update.Enabled
	}
	if update.Kind != nil {
		item.Kind = *update.Kind
	}
	if update.Tags != nil {
		item.Tags = *update.Tags
	}
	if err := validate(item); err != nil {
		return Region{}, err
	}
	next := clone(r.byID)
	next[id] = item
	if err := validateUnique(next, id); err != nil {
		return Region{}, err
	}
	if err := r.persist(next); err != nil {
		return Region{}, err
	}
	r.byID = next
	return item, nil
}

func (r *Registry) RotateAccessKey(_ context.Context, id, accessKey string) (Region, error) {
	if r == nil {
		return Region{}, ErrNotFound
	}
	if strings.TrimSpace(accessKey) == "" {
		return Region{}, fmt.Errorf("%w: access key is empty", ErrInvalid)
	}
	r.mu.Lock()
	defer r.mu.Unlock()
	item, found := r.byID[id]
	if !found {
		return Region{}, ErrNotFound
	}
	item.AccessKey = accessKey
	next := clone(r.byID)
	next[id] = item
	if err := r.persist(next); err != nil {
		return Region{}, err
	}
	r.byID = next
	return item, nil
}

func (r *Registry) Delete(_ context.Context, id string) error {
	if r == nil {
		return ErrNotFound
	}
	r.mu.Lock()
	defer r.mu.Unlock()
	if _, found := r.byID[id]; !found {
		return ErrNotFound
	}
	next := clone(r.byID)
	delete(next, id)
	if err := r.persist(next); err != nil {
		return err
	}
	r.byID = next
	return nil
}

func (r *Registry) persist(items map[string]Region) error {
	stored := make([]fileRegion, 0, len(items))
	for _, item := range items {
		enabled := item.Enabled
		sizeX, sizeY := item.SizeX, item.SizeY
		stored = append(stored, fileRegion{ID: item.ID, Name: item.Name, OwnerUserID: item.OwnerUserID,
			MapX: item.MapX, MapY: item.MapY, SizeX: &sizeX, SizeY: &sizeY, Maturity: item.Maturity,
			FacetNames:     item.FacetNames,
			PublicEndpoint: item.PublicEndpoint,
			ViewerPort:     item.ViewerPort, Enabled: &enabled, Kind: item.Kind, Tags: item.Tags,
			AccessKey: item.AccessKey})
	}
	sort.Slice(stored, func(i, j int) bool {
		if stored[i].MapY != stored[j].MapY {
			return stored[i].MapY < stored[j].MapY
		}
		if stored[i].MapX != stored[j].MapX {
			return stored[i].MapX < stored[j].MapX
		}
		return stored[i].ID < stored[j].ID
	})
	content, err := json.MarshalIndent(stored, "", "  ")
	if err != nil {
		return fmt.Errorf("encode provisioned regions: %w", err)
	}
	content = append(content, '\n')
	temporary, err := os.CreateTemp(filepath.Dir(r.path), ".regions-*.json")
	if err != nil {
		return fmt.Errorf("create provisioned regions update: %w", err)
	}
	temporaryPath := temporary.Name()
	defer os.Remove(temporaryPath)
	if err := temporary.Chmod(0600); err != nil {
		temporary.Close()
		return fmt.Errorf("protect provisioned regions update: %w", err)
	}
	if _, err := temporary.Write(content); err != nil {
		temporary.Close()
		return fmt.Errorf("write provisioned regions update: %w", err)
	}
	if err := temporary.Sync(); err != nil {
		temporary.Close()
		return fmt.Errorf("sync provisioned regions update: %w", err)
	}
	if err := temporary.Close(); err != nil {
		return fmt.Errorf("close provisioned regions update: %w", err)
	}
	if err := os.Rename(temporaryPath, r.path); err != nil {
		return fmt.Errorf("replace provisioned regions: %w", err)
	}
	return nil
}

func validate(item Region) error {
	if !uuidPattern.MatchString(item.ID) || strings.TrimSpace(item.Name) == "" || len(item.Name) > 128 ||
		item.MapX < 0 || item.MapY < 0 ||
		item.Maturity < 0 || item.Maturity > 2 || item.ViewerPort < 0 || item.ViewerPort > 65535 ||
		strings.TrimSpace(item.AccessKey) == "" {
		return fmt.Errorf("%w: UUID, name, coordinates, or access key is invalid", ErrInvalid)
	}
	if err := validateShape(item.SizeX, item.SizeY); err != nil {
		return err
	}
	if err := validateFacetNames(item); err != nil {
		return err
	}
	if item.OwnerUserID != "" && !uuidPattern.MatchString(item.OwnerUserID) {
		return fmt.Errorf("%w: owner user UUID is invalid", ErrInvalid)
	}
	if item.PublicEndpoint != "" {
		endpoint, err := url.ParseRequestURI(item.PublicEndpoint)
		if err != nil || (endpoint.Scheme != "http" && endpoint.Scheme != "https") || endpoint.Host == "" {
			return fmt.Errorf("%w: public endpoint is invalid", ErrInvalid)
		}
	}
	return nil
}

// validateShape is the ADR 0036 shape rule: the shorter dimension must be a
// proven square viewer size and must divide the longer, so square facets of
// the shorter edge tile the rectangle exactly. A 4x8 is two facets; a 2x5 has
// no whole tiling and is invalid, as is an 8x8 (no viewer has rendered an
// 8-tile square).
func validateShape(sizeX, sizeY int) error {
	shorter, longer := sizeX, sizeY
	if shorter > longer {
		shorter, longer = longer, shorter
	}
	if (shorter != 1 && shorter != 2 && shorter != 4) || longer%shorter != 0 {
		return fmt.Errorf("%w: region shape %dx%d violates the facet rule (shorter side must be 1, 2, or 4 tiles and divide the longer)",
			ErrInvalid, sizeX, sizeY)
	}
	return nil
}

// validateFacetNames requires exactly one name per facet beyond the first,
// each shaped like a region name; uniqueness against other regions is the
// store's job, but a region may not collide with itself.
func validateFacetNames(item Region) error {
	if len(item.FacetNames) != item.FacetCount()-1 {
		return fmt.Errorf("%w: a %dx%d region has %d facets and needs %d facet names beyond its own",
			ErrInvalid, item.SizeX, item.SizeY, item.FacetCount(), item.FacetCount()-1)
	}
	seen := map[string]bool{strings.ToLower(item.Name): true}
	for _, name := range item.FacetNames {
		if strings.TrimSpace(name) == "" || len(name) > 128 {
			return fmt.Errorf("%w: facet name %q is invalid", ErrInvalid, name)
		}
		lower := strings.ToLower(name)
		if seen[lower] {
			return fmt.Errorf("%w: facet name %q repeats within the region", ErrConflict, name)
		}
		seen[lower] = true
	}
	return nil
}

// trimmedNames trims whitespace from each name, mirroring what Create and
// Update do to the region's own name.
func trimmedNames(names []string) []string {
	result := make([]string, len(names))
	for index, name := range names {
		result[index] = strings.TrimSpace(name)
	}
	return result
}

// allNames lists every viewer-visible name a region claims: its own, then its
// facet names in facet order.
func allNames(item Region) []string {
	names := make([]string, 0, 1+len(item.FacetNames))
	names = append(names, item.Name)
	names = append(names, item.FacetNames...)
	return names
}

func validateUnique(items map[string]Region, changedID string) error {
	names := make(map[string]string, len(items))
	for id, item := range items {
		// Facet names are viewer-visible region names, so they share the
		// uniqueness namespace with region names.
		for _, name := range allNames(item) {
			lower := strings.ToLower(name)
			if other, exists := names[lower]; exists && other != id {
				return fmt.Errorf("%w: region %q shares a name with %q", ErrConflict, id, other)
			}
			names[lower] = id
		}
		for otherID, other := range items {
			if otherID == id {
				continue
			}
			if item.MapX < other.MapX+other.SizeX && other.MapX < item.MapX+item.SizeX &&
				item.MapY < other.MapY+other.SizeY && other.MapY < item.MapY+item.SizeY {
				return fmt.Errorf("%w: region %q overlaps %q", ErrConflict, id, otherID)
			}
		}
	}
	_ = changedID
	return nil
}

func clone(items map[string]Region) map[string]Region {
	result := make(map[string]Region, len(items))
	for id, item := range items {
		result[id] = item
	}
	return result
}
