package httpapi

import (
	"time"

	"github.com/homeworldz/server/grid/internal/estate"
	"github.com/homeworldz/server/grid/internal/inventory"
	"github.com/homeworldz/server/grid/internal/presence"
	"github.com/homeworldz/server/grid/internal/provisioning"
	"github.com/homeworldz/server/grid/internal/regions"
	"github.com/homeworldz/server/grid/internal/transit"
)

const APIVersion = "v1"

// Status is the response model for a successful operational status probe.
type Status struct {
	Status string `json:"status"`
}

// Version identifies a service build and its internal API compatibility level.
type Version struct {
	Service    string `json:"service"`
	Version    string `json:"version"`
	APIVersion string `json:"apiVersion"`
}

// Error is the common error response model.
type Error struct {
	Code    string `json:"code"`
	Message string `json:"message"`
}

type RegisterRegionRequest struct {
	Name           string `json:"name"`
	GridX          int    `json:"gridX"`
	GridY          int    `json:"gridY"`
	PublicEndpoint string `json:"publicEndpoint"`
	ViewerPort     int    `json:"viewerPort"`
	LeaseSeconds   int    `json:"leaseSeconds"`
}

type RenewRegionLeaseRequest struct {
	LeaseSeconds int `json:"leaseSeconds"`
	// RegionProtocol is the region software's grid-region protocol version
	// (docs/CLIENT2.md, "the region protocol version"). Zero means the region
	// predates the handshake and is accepted; a non-zero value must match.
	RegionProtocol int `json:"regionProtocol,omitempty"`
}

type StartProvisionedRegionRequest struct {
	PublicEndpoint string `json:"publicEndpoint"`
	ViewerPort     int    `json:"viewerPort"`
	LeaseSeconds   int    `json:"leaseSeconds"`
	// RegionProtocol, as on RenewRegionLeaseRequest.
	RegionProtocol int `json:"regionProtocol,omitempty"`
	// SessionEndpoint is the public ws:// or wss:// URL of the region's
	// session transport, when it serves one (docs/CLIENT2-TRANSPORT.md).
	SessionEndpoint string `json:"sessionEndpoint,omitempty"`
}

// ValidateRegionTicketRequest carries the region ticket a client presented to
// a region; the region forwards it here because the signing secret never
// leaves the grid.
type ValidateRegionTicketRequest struct {
	Token string `json:"token"`
}

// ValidateRegionTicketResult is the identity a valid ticket resolves to.
// Position is the region-local arrival point world entry resolved, present
// only when it resolved one; a region spawns there rather than trusting any
// position from the client.
type ValidateRegionTicketResult struct {
	UserID      string    `json:"userId"`
	Userid      string    `json:"userid"`
	DisplayName string    `json:"displayName"`
	SessionID   string    `json:"sessionId"`
	ExpiresAt   time.Time `json:"expiresAt"`
	Position    []float64 `json:"position,omitempty"`
}

type ProvisionedRegionRuntimeResult struct {
	regions.Region
	GridName      string         `json:"gridName"`
	GridPublicURL string         `json:"gridPublicUrl"`
	SizeX         int            `json:"sizeX"`
	SizeY         int            `json:"sizeY"`
	Maturity      int            `json:"maturity"`
	OwnerUserID   string         `json:"ownerUserId"`
	Estate        *estate.Estate `json:"estate,omitempty"`
	// Facets are the square viewer-facing regions this process presents
	// (ADR 0036), in map-coordinate order. A square region is one facet at its
	// own corner, name, and port, so a region that predates facets can ignore
	// the list entirely.
	Facets []RegionFacet `json:"facets,omitempty"`
	// RegionProtocol is the grid's current grid-region protocol version, so a
	// region that is behind can warn its operator before an increment is
	// enforced against it.
	RegionProtocol int `json:"regionProtocol"`
}

// RegionFacet is one square viewer-facing region of a provisioned region: its
// name, its southwest corner in map tiles, its edge in metres, and the viewer
// UDP port the process serves it on (consecutive from the base port).
type RegionFacet struct {
	Index      int    `json:"index"`
	Name       string `json:"name"`
	GridX      int    `json:"gridX"`
	GridY      int    `json:"gridY"`
	Edge       int    `json:"edge"`
	ViewerPort int    `json:"viewerPort"`
}

// EstateResult wraps an estate for the region-runtime estate endpoints.
type EstateResult struct {
	Estate estate.Estate `json:"estate"`
}

// EstateSettingsRequest updates estate scalar/flag fields; nil fields are unchanged.
type EstateSettingsRequest struct {
	Name           *string  `json:"name,omitempty"`
	OwnerUserID    *string  `json:"ownerUserId,omitempty"`
	Flags          *uint64  `json:"flags,omitempty"`
	PublicAccess   *bool    `json:"publicAccess,omitempty"`
	SunHour        *float64 `json:"sunHour,omitempty"`
	UseGlobalTime  *bool    `json:"useGlobalTime,omitempty"`
	FixedSun       *bool    `json:"fixedSun,omitempty"`
	BillableFactor *float64 `json:"billableFactor,omitempty"`
	PricePerMeter  *int     `json:"pricePerMeter,omitempty"`
	RedirectGridX  *int     `json:"redirectGridX,omitempty"`
	RedirectGridY  *int     `json:"redirectGridY,omitempty"`
	AbuseEmail     *string  `json:"abuseEmail,omitempty"`
}

func (r EstateSettingsRequest) toUpdate() estate.SettingsUpdate {
	return estate.SettingsUpdate{Name: r.Name, OwnerUserID: r.OwnerUserID, Flags: r.Flags,
		PublicAccess: r.PublicAccess, SunHour: r.SunHour, UseGlobalTime: r.UseGlobalTime,
		FixedSun: r.FixedSun, BillableFactor: r.BillableFactor, PricePerMeter: r.PricePerMeter,
		RedirectGridX: r.RedirectGridX, RedirectGridY: r.RedirectGridY, AbuseEmail: r.AbuseEmail}
}

// EstateMemberRequest adds or removes one access-list member (role 0=manager,
// 1=allowed user, 2=allowed group, 3=ban).
type EstateMemberRequest struct {
	MemberID string `json:"memberId"`
	Role     int    `json:"role"`
	Present  bool   `json:"present"`
}

type RegionList struct {
	Regions []regions.Region `json:"regions"`
}

type RegionNeighbor struct {
	Direction string         `json:"direction"`
	Region    RegionTopology `json:"region"`
}

type RegionTopology struct {
	ID   string `json:"id"`
	Name string `json:"name"`
	// Facet is which of its region's facets this entry is (ADR 0036); square
	// regions are facet 0. Entries of one rectangle share an id and differ here.
	Facet          int    `json:"facet,omitempty"`
	GridX          int    `json:"gridX"`
	GridY          int    `json:"gridY"`
	SizeX          int    `json:"sizeX"`
	SizeY          int    `json:"sizeY"`
	Maturity       int    `json:"maturity"`
	PublicEndpoint string `json:"publicEndpoint,omitempty"`
	ViewerPort     int    `json:"viewerPort,omitempty"`
	Online         bool   `json:"online"`
	// SessionEndpoint is the neighbor's region-session URL when it is online
	// and serves one, so a region crossing a session avatar can tell the
	// client where to continue (docs/CLIENT2-EMBODIMENT.md milestone E2).
	SessionEndpoint string `json:"sessionEndpoint,omitempty"`
}

type RegionNeighborList struct {
	Neighbors []RegionNeighbor `json:"neighbors"`
}

// RegionTopologyList is every region's placement, which is what a world map
// has to draw: the map is not a view of the regions next door.
type RegionTopologyList struct {
	Regions []RegionTopology `json:"regions"`
}

type CreateProvisionedRegionRequest struct {
	ID          string `json:"id,omitempty"`
	Name        string `json:"name"`
	OwnerUserID string `json:"ownerUserId,omitempty"`
	MapX        int    `json:"mapX"`
	MapY        int    `json:"mapY"`
	// Size is the square shorthand; sizeX/sizeY supersede it and win when both
	// are given (ADR 0036).
	Size           int      `json:"size,omitempty"`
	SizeX          int      `json:"sizeX,omitempty"`
	SizeY          int      `json:"sizeY,omitempty"`
	FacetNames     []string `json:"facetNames,omitempty"`
	Maturity       int      `json:"maturity,omitempty"`
	PublicEndpoint string   `json:"publicEndpoint,omitempty"`
	ViewerPort     int      `json:"viewerPort,omitempty"`
	Enabled        *bool    `json:"enabled,omitempty"`
}

type UpdateProvisionedRegionRequest struct {
	Name           *string   `json:"name,omitempty"`
	OwnerUserID    *string   `json:"ownerUserId,omitempty"`
	MapX           *int      `json:"mapX,omitempty"`
	MapY           *int      `json:"mapY,omitempty"`
	Size           *int      `json:"size,omitempty"`
	SizeX          *int      `json:"sizeX,omitempty"`
	SizeY          *int      `json:"sizeY,omitempty"`
	FacetNames     *[]string `json:"facetNames,omitempty"`
	Maturity       *int      `json:"maturity,omitempty"`
	PublicEndpoint *string   `json:"publicEndpoint,omitempty"`
	ViewerPort     *int      `json:"viewerPort,omitempty"`
	Enabled        *bool     `json:"enabled,omitempty"`
}

type ProvisionedRegionResult struct {
	Region    provisioning.Region `json:"region"`
	AccessKey string              `json:"accessKey,omitempty"`
}

type ProvisionedRegionList struct {
	Regions []provisioning.Region `json:"regions"`
}

type PrepareTransitRequest struct {
	ID                  string          `json:"id"`
	AgentID             string          `json:"agentId"`
	SessionID           string          `json:"sessionId"`
	SourceRegionID      string          `json:"sourceRegionId"`
	DestinationRegionID string          `json:"destinationRegionId"`
	Position            transit.Vector3 `json:"position"`
	LookAt              transit.Vector3 `json:"lookAt"`
	Flying              bool            `json:"flying"`
	LifetimeSeconds     int             `json:"lifetimeSeconds"`
}

type TransitActionRequest struct {
	RegionID string `json:"regionId"`
	Reason   string `json:"reason,omitempty"`
}

type CreateUserRequest struct {
	Username string `json:"username"`
	Password string `json:"password"`
}

type CreateSessionRequest struct {
	Username       string `json:"username"`
	Password       string `json:"password"`
	SessionSeconds int    `json:"sessionSeconds"`
}

type UpdatePresenceRequest struct {
	RegionID string `json:"regionId"`
}

type UpdateLocationRequest struct {
	RegionID string          `json:"regionId"`
	Position transit.Vector3 `json:"position"`
	LookAt   transit.Vector3 `json:"lookAt"`
	Flying   bool            `json:"flying"`
}

type PresenceList struct {
	Presence []presence.Presence `json:"presence"`
}

type InventoryFolderList struct {
	Folders []inventory.Folder `json:"folders"`
}

type CreateInventoryFolderRequest struct {
	ID          string `json:"id"`
	ParentID    string `json:"parentId"`
	Name        string `json:"name"`
	TypeDefault int    `json:"typeDefault"`
}

type CreateInventoryItemRequest struct {
	ID                  string `json:"id"`
	CreatorUserID       string `json:"creatorUserId"`
	FolderID            string `json:"folderId"`
	AssetID             string `json:"assetId"`
	AssetType           int    `json:"assetType"`
	InventoryType       int    `json:"inventoryType"`
	Name                string `json:"name"`
	Description         string `json:"description"`
	Flags               uint32 `json:"flags"`
	BasePermissions     uint32 `json:"basePermissions"`
	CurrentPermissions  uint32 `json:"currentPermissions"`
	EveryonePermissions uint32 `json:"everyonePermissions"`
	NextPermissions     uint32 `json:"nextPermissions"`
}

type CopyLibraryInventoryItemRequest struct {
	SourceItemID        string `json:"sourceItemId"`
	DestinationFolderID string `json:"destinationFolderId"`
	Name                string `json:"name"`
}

type CopyInventoryItemRequest struct {
	SourceItemID        string `json:"sourceItemId"`
	DestinationFolderID string `json:"destinationFolderId"`
	Name                string `json:"name"`
}

type MoveInventoryFolderRequest struct {
	ParentID string `json:"parentId"`
}

type MoveInventoryItemRequest struct {
	FolderID string `json:"folderId"`
	Name     string `json:"name"`
}

type RegisterAssetRequest struct {
	ID            string `json:"id"`
	CreatorUserID string `json:"creatorUserId"`
	SHA256        string `json:"sha256"`
	Size          int64  `json:"size"`
	Endpoint      string `json:"endpoint"`
	Origin        bool   `json:"origin"`
}
