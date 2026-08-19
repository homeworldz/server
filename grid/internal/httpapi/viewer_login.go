package httpapi

import (
	"bytes"
	"context"
	"crypto/rand"
	"encoding/binary"
	"encoding/json"
	"encoding/xml"
	"errors"
	"fmt"
	"io"
	"math"
	"net"
	"net/http"
	"net/url"
	"strings"
	"time"

	"github.com/homeworldz/server/grid/internal/arrival"
	"github.com/homeworldz/server/grid/internal/gestures"
	"github.com/homeworldz/server/grid/internal/identity"
	"github.com/homeworldz/server/grid/internal/inventory"
	"github.com/homeworldz/server/grid/internal/regions"
)

type rpcMethodCall struct {
	Method string    `xml:"methodName"`
	Params rpcParams `xml:"params"`
}
type rpcParams struct {
	Items []rpcParam `xml:"param"`
}
type rpcParam struct {
	Value rpcValue `xml:"value"`
}

type rpcValue struct {
	Text    string     `xml:",chardata"`
	String  *string    `xml:"string"`
	Integer *string    `xml:"int"`
	I4      *string    `xml:"i4"`
	Boolean *string    `xml:"boolean"`
	Struct  *rpcStruct `xml:"struct"`
	Array   *rpcArray  `xml:"array"`
}
type rpcStruct struct {
	Members []rpcMember `xml:"member"`
}
type rpcMember struct {
	Name  string   `xml:"name"`
	Value rpcValue `xml:"value"`
}
type rpcArray struct {
	Values []rpcValue `xml:"data>value"`
}

func (v rpcValue) text() string {
	if v.String != nil {
		return *v.String
	}
	if v.Integer != nil {
		return *v.Integer
	}
	if v.I4 != nil {
		return *v.I4
	}
	if v.Boolean != nil {
		return *v.Boolean
	}
	return strings.TrimSpace(v.Text)
}

func (v rpcValue) fields() map[string]rpcValue {
	result := make(map[string]rpcValue)
	if v.Struct != nil {
		for _, member := range v.Struct.Members {
			result[member.Name] = member.Value
		}
	}
	return result
}

type rpcOutputValue struct {
	String  *string          `xml:"string,omitempty"`
	Integer *int             `xml:"int,omitempty"`
	Boolean *int             `xml:"boolean,omitempty"`
	Struct  *rpcOutputStruct `xml:"struct,omitempty"`
	Array   *rpcOutputArray  `xml:"array,omitempty"`
}
type rpcOutputStruct struct {
	Members []rpcOutputMember `xml:"member"`
}
type rpcOutputMember struct {
	Name  string         `xml:"name"`
	Value rpcOutputValue `xml:"value"`
}
type rpcOutputArray struct {
	Values []rpcOutputValue `xml:"data>value"`
}
type rpcMethodResponse struct {
	XMLName xml.Name       `xml:"methodResponse"`
	Value   rpcOutputValue `xml:"params>param>value"`
}

func rpcString(value string) rpcOutputValue { return rpcOutputValue{String: &value} }
func rpcInt(value int) rpcOutputValue       { return rpcOutputValue{Integer: &value} }
func rpcBool(value bool) rpcOutputValue {
	number := 0
	if value {
		number = 1
	}
	return rpcOutputValue{Boolean: &number}
}
func rpcArrayValue(values ...rpcOutputValue) rpcOutputValue {
	return rpcOutputValue{Array: &rpcOutputArray{Values: values}}
}
func rpcStructValue(members ...rpcOutputMember) rpcOutputValue {
	return rpcOutputValue{Struct: &rpcOutputStruct{Members: members}}
}
func rpcField(name string, value rpcOutputValue) rpcOutputMember {
	return rpcOutputMember{Name: name, Value: value}
}

func inventoryFolder(folder inventory.Folder) rpcOutputValue {
	return rpcStructValue(
		rpcField("name", rpcString(folder.Name)),
		rpcField("folder_id", rpcString(folder.ID)),
		rpcField("parent_id", rpcString(folder.ParentID)),
		rpcField("version", rpcInt(int(folder.Version))),
		rpcField("type_default", rpcInt(folder.TypeDefault)),
	)
}

func inventorySkeleton(folders []inventory.Folder) (string, []rpcOutputValue) {
	values := make([]rpcOutputValue, 0, len(folders))
	rootID := ""
	for _, folder := range folders {
		if folder.TypeDefault == 8 {
			rootID = folder.ID
		}
		values = append(values, inventoryFolder(folder))
	}
	return rootID, values
}

func (a *API) viewerLogin(w http.ResponseWriter, r *http.Request) {
	if r.Method == http.MethodGet {
		a.welcome(w, r)
		return
	}
	if r.Method != http.MethodPost {
		w.Header().Set("Allow", "GET, POST")
		writeViewerLogin(w, loginFailure("method", "Only GET and POST are supported."))
		return
	}
	if a.identity == nil || a.regions == nil {
		writeViewerLogin(w, loginFailure("unavailable", "The Homeworldz grid is not ready."))
		return
	}
	body, readErr := io.ReadAll(http.MaxBytesReader(w, r.Body, 1024*1024))
	if readErr != nil {
		writeViewerLogin(w, loginFailure("key", "The viewer login request is invalid."))
		return
	}
	// Modern viewers and LibreMetaverse use LLSD login; Firestorm 7.2.4 uses
	// the legacy XML-RPC login_to_simulator. Support both by dispatching on the
	// document type.
	if bytes.Contains(body, []byte("<llsd")) {
		a.viewerLoginLLSD(w, r, body)
		return
	}
	var call rpcMethodCall
	decoder := xml.NewDecoder(bytes.NewReader(body))
	if err := decoder.Decode(&call); err != nil || call.Method != "login_to_simulator" || len(call.Params.Items) != 1 {
		writeViewerLogin(w, loginFailure("key", "The viewer login request is invalid."))
		return
	}
	if err := decoder.Decode(&struct{}{}); !errors.Is(err, io.EOF) {
		writeViewerLogin(w, loginFailure("key", "The viewer login request contains trailing data."))
		return
	}
	callFields := call.Params.Items[0].Value.fields()
	result, reason, message := a.resolveViewerLogin(r,
		callFields["first"].text(), callFields["last"].text(),
		callFields["passwd"].text(), callFields["start"].text())
	if reason != "" {
		writeViewerLogin(w, loginFailure(reason, message))
		return
	}
	writeViewerLogin(w, a.xmlrpcLoginResponse(result))
}

// loginFields is the resolved result of a viewer login, independent of the wire
// format (XML-RPC or LLSD) used to request it.
type loginFields struct {
	agentID, sessionID, secureID string
	first, last                  string
	circuit                      uint32
	simIP                        string
	simPort, regionX, regionY    int
	regionSizeX, regionSizeY     int
	startLocation, lookAt        string
	seedCapability               string
	folders                      []inventory.Folder
	libFolders                   []inventory.Folder
	gestures                     []gestures.Gesture
}

// resolveViewerLogin performs authentication, region resolution, circuit
// allocation, and inventory preparation shared by every login wire format. It
// returns the resolved fields, or a ("", reason, message) failure triple.
func (a *API) resolveViewerLogin(r *http.Request, firstRaw, lastRaw, passwd, start string) (*loginFields, string, string) {
	first := strings.TrimSpace(firstRaw)
	last := strings.TrimSpace(lastRaw)
	username := strings.ToLower(first)
	if last != "" && !strings.EqualFold(last, "Resident") {
		username += "." + strings.ToLower(last)
	}
	passwordHash := strings.TrimPrefix(passwd, "$1$")
	if first == "" || len(passwordHash) != 32 {
		return nil, "key", "The username or password is incorrect."
	}
	session, err := a.identity.CreateViewerSession(r.Context(), username, strings.ToLower(passwordHash), 12*time.Hour)
	if errors.Is(err, identity.ErrInvalidCredentials) {
		return nil, "key", "The username or password is incorrect."
	}
	if err != nil {
		return nil, "unavailable", "The Homeworldz grid could not create a session."
	}
	preferredRegionID := ""
	var storedPosition *[3]float32
	if a.locations != nil {
		if strings.EqualFold(start, "home") {
			if location, locationErr := a.locations.GetHome(r.Context(), session.UserID); locationErr == nil {
				preferredRegionID = location.RegionID
				position := location.Position
				storedPosition = &position
			}
		} else if location, locationErr := a.locations.Get(r.Context(), session.UserID); locationErr == nil {
			preferredRegionID = location.RegionID
			position := location.Position
			storedPosition = &position
		}
	}
	region, err := resolveDestination(r.Context(), a.regions, start, preferredRegionID, a.welcomePoints)
	if err != nil {
		_ = a.identity.RevokeSession(r.Context(), session.ID)
		return nil, "destination", "No online region matches the requested destination."
	}
	endpoint, err := url.Parse(region.PublicEndpoint)
	if err != nil || endpoint.Hostname() == "" {
		_ = a.identity.RevokeSession(r.Context(), session.ID)
		return nil, "unavailable", "The destination region endpoint is invalid."
	}
	simIP, err := simulatorIPv4(r.Context(), endpoint.Hostname())
	if err != nil {
		_ = a.identity.RevokeSession(r.Context(), session.ID)
		return nil, "unavailable", "The destination region address could not be resolved."
	}
	circuit, err := newCircuitCode()
	if err != nil {
		_ = a.identity.RevokeSession(r.Context(), session.ID)
		return nil, "unavailable", "The grid could not allocate a viewer circuit."
	}
	if err := a.identity.AssignViewerDestination(r.Context(), session.ID, circuit, region.ID); err != nil {
		_ = a.identity.RevokeSession(r.Context(), session.ID)
		return nil, "unavailable", "The grid could not assign the viewer circuit."
	}
	folders := inventory.SystemFolders(session.UserID)
	if a.inventory != nil {
		folders, err = a.inventory.EnsureSystemFolders(r.Context(), session.UserID)
		if err != nil {
			_ = a.identity.RevokeSession(r.Context(), session.ID)
			return nil, "unavailable", "The grid could not prepare the viewer inventory."
		}
		existingItems, err := a.inventory.ListItems(r.Context(), session.UserID)
		if err != nil {
			_ = a.identity.RevokeSession(r.Context(), session.ID)
			return nil, "unavailable", "The grid could not inspect the viewer inventory."
		}
		defaultWearables := inventory.DefaultWearables(session.UserID)
		if !inventory.DefaultOutfitInitialized(session.UserID, existingItems) {
			for _, item := range defaultWearables {
				if _, err := a.inventory.EnsureItem(r.Context(), item); err != nil {
					_ = a.identity.RevokeSession(r.Context(), session.ID)
					return nil, "unavailable", "The grid could not prepare the default outfit."
				}
			}
		} else if inventory.DefaultOutfitNeedsRepair(session.UserID, existingItems) {
			for index := 1; index < len(defaultWearables); index += 2 {
				if _, err := a.inventory.EnsureItem(r.Context(), defaultWearables[index]); err != nil {
					_ = a.identity.RevokeSession(r.Context(), session.ID)
					return nil, "unavailable", "The grid could not repair the default outfit."
				}
			}
		}
		folders, err = a.inventory.ListFolders(r.Context(), session.UserID)
		if err != nil {
			_ = a.identity.RevokeSession(r.Context(), session.ID)
			return nil, "unavailable", "The grid could not refresh the viewer inventory."
		}
	}
	lookAt := "[r1,r0,r0]"
	if state, ok := a.regionStartState(r.Context(), region.PublicEndpoint, session.UserID); ok {
		lookAt = fmt.Sprintf("[r%.9g,r%.9g,r%.9g]", state.LookAt[0], state.LookAt[1], state.LookAt[2])
	}
	// A viewer logs into one square facet of the region (ADR 0036): the facet
	// containing the arrival position, or facet 0 when nothing places the
	// avatar more precisely. A square region is exactly its single facet, so
	// nothing changes for it here.
	regionSizeX, regionSizeY := 256, 256
	simPort := region.ViewerPort
	regionX, regionY := region.GridX*256, region.GridY*256
	if a.provisioned != nil {
		if provisioned, provisionErr := a.provisioned.Get(r.Context(), region.ID); provisionErr == nil {
			facet := 0
			if storedPosition != nil && preferredRegionID == region.ID {
				facet = provisioned.FacetAtPosition(float64(storedPosition[0]), float64(storedPosition[1]))
			}
			edge := provisioned.FacetEdge() * 256
			regionSizeX, regionSizeY = edge, edge
			originX, originY := provisioned.FacetOrigin(facet)
			regionX, regionY = originX*256, originY*256
			simPort = region.ViewerPort + facet
		}
	}
	var activeGestures []gestures.Gesture
	if a.gestures != nil {
		if set, gestureErr := a.gestures.ListActive(r.Context(), session.UserID); gestureErr == nil {
			activeGestures = set
		}
	}
	return &loginFields{
		agentID: session.UserID, sessionID: session.ID, secureID: session.SecureID,
		first: first, last: last, circuit: circuit,
		simIP: simIP, simPort: simPort,
		regionX: regionX, regionY: regionY,
		regionSizeX: regionSizeX, regionSizeY: regionSizeY,
		startLocation: normalizeStart(start), lookAt: lookAt,
		seedCapability: strings.TrimRight(region.PublicEndpoint, "/") + "/caps/seed/" + session.ID,
		folders:        folders,
		libFolders:     inventory.LibraryFolders(),
		gestures:       activeGestures,
	}, "", ""
}

// xmlrpcLoginResponse serializes resolved login fields as the legacy XML-RPC
// login_to_simulator response (Firestorm and older viewers).
func (a *API) xmlrpcLoginResponse(f *loginFields) rpcOutputValue {
	rootID, skeleton := inventorySkeleton(f.folders)
	root := rpcStructValue(rpcField("folder_id", rpcString(rootID)))
	libraryRoot := rpcStructValue(rpcField("folder_id", rpcString(inventory.LibraryRootID)))
	libraryOwner := rpcStructValue(rpcField("agent_id", rpcString(inventory.LibraryOwnerID)))
	_, librarySkeleton := inventorySkeleton(f.libFolders)
	gestureValues := make([]rpcOutputValue, 0, len(f.gestures))
	for _, g := range f.gestures {
		gestureValues = append(gestureValues, rpcStructValue(
			rpcField("item_id", rpcString(g.ItemID)), rpcField("asset_id", rpcString(g.AssetID))))
	}
	return rpcStructValue(
		rpcField("login", rpcString("true")), rpcField("message", rpcString(a.welcomeMessage(f.first+" "+f.last))),
		rpcField("agent_id", rpcString(f.agentID)), rpcField("session_id", rpcString(f.sessionID)),
		rpcField("secure_session_id", rpcString(f.secureID)), rpcField("first_name", rpcString(f.first)),
		rpcField("last_name", rpcString(f.last)), rpcField("circuit_code", rpcInt(int(f.circuit))),
		rpcField("sim_ip", rpcString(f.simIP)), rpcField("sim_port", rpcInt(f.simPort)),
		rpcField("region_x", rpcInt(f.regionX)), rpcField("region_y", rpcInt(f.regionY)),
		rpcField("region_size_x", rpcInt(f.regionSizeX)), rpcField("region_size_y", rpcInt(f.regionSizeY)),
		rpcField("start_location", rpcString(f.startLocation)),
		rpcField("look_at", rpcString(f.lookAt)),
		rpcField("seed_capability", rpcString(f.seedCapability)),
		rpcField("seconds_since_epoch", rpcInt(int(time.Now().Unix()))),
		rpcField("inventory-root", rpcArrayValue(root)), rpcField("inventory-skeleton", rpcArrayValue(skeleton...)),
		rpcField("inventory-lib-root", rpcArrayValue(libraryRoot)), rpcField("inventory-lib-owner", rpcArrayValue(libraryOwner)),
		rpcField("inventory-skel-lib", rpcArrayValue(librarySkeleton...)), rpcField("login-flags", rpcArrayValue()),
		rpcField("gestures", rpcArrayValue(gestureValues...)), rpcField("buddy-list", rpcArrayValue()),
	)
}

// simulatorIPv4 resolves a region's endpoint host to a dotted-quad IPv4
// address for sim_ip. That field is a 32-bit address in the viewer login
// protocol: viewers parse it with inet_addr and never resolve it, so a
// hostname there yields a login the viewer accepts and then cannot build a
// circuit from — reported as a bare "Login failed." with nothing in it naming
// the address. Only a literal IP had ever been in that field while region
// endpoints were loopback, which is why configuring real grid URLs broke it.
//
// A resolution failure is returned rather than falling back to the hostname:
// the fallback is precisely the value the viewer rejects, so it would hide the
// failure behind a login that looks well-formed.
func simulatorIPv4(ctx context.Context, host string) (string, error) {
	if ip := net.ParseIP(host); ip != nil {
		v4 := ip.To4()
		if v4 == nil {
			return "", fmt.Errorf("region endpoint %q is IPv6; the viewer circuit is IPv4 only", host)
		}
		return v4.String(), nil
	}
	lookup, cancel := context.WithTimeout(ctx, 2*time.Second)
	defer cancel()
	addresses, err := net.DefaultResolver.LookupIP(lookup, "ip4", host)
	if err != nil {
		return "", err
	}
	if len(addresses) == 0 {
		return "", fmt.Errorf("region endpoint %q has no IPv4 address", host)
	}
	return addresses[0].String(), nil
}

type regionStartState struct {
	LookAt [3]float64 `json:"lookAt"`
}

func (a *API) regionStartState(ctx context.Context, endpoint, userID string) (regionStartState, bool) {
	if a.serviceToken == "" {
		return regionStartState{}, false
	}
	request, err := http.NewRequestWithContext(ctx, http.MethodGet,
		strings.TrimRight(endpoint, "/")+"/api/v1/agents/"+url.PathEscape(userID)+"/start-state", nil)
	if err != nil {
		return regionStartState{}, false
	}
	request.Header.Set("Authorization", "Bearer "+a.serviceToken)
	client := http.Client{Timeout: time.Second}
	response, err := client.Do(request)
	if err != nil {
		return regionStartState{}, false
	}
	defer response.Body.Close()
	if response.StatusCode != http.StatusOK {
		return regionStartState{}, false
	}
	var state regionStartState
	if err := json.NewDecoder(io.LimitReader(response.Body, 4096)).Decode(&state); err != nil {
		return regionStartState{}, false
	}
	length := math.Hypot(state.LookAt[0], state.LookAt[1])
	if !isFinite(state.LookAt[0]) || !isFinite(state.LookAt[1]) || !isFinite(state.LookAt[2]) || length < 0.001 {
		return regionStartState{}, false
	}
	return state, true
}

func isFinite(value float64) bool { return !math.IsNaN(value) && !math.IsInf(value, 0) }

// resolveDestination selects the viewer's login region on the shared arrival
// logic (docs/CLIENT2.md, "Default and fallback arrival points"): an explicit
// uri: destination is honored or refused (never diverted), a stored region
// wins while it is leased, and otherwise the welcome list decides. The legacy
// first-region fallback survives only for grids with no welcome list, so an
// unconfigured development grid still logs a viewer in somewhere.
func resolveDestination(ctx context.Context, store regions.Store, start, preferredRegionID string,
	welcome []arrival.Point) (regions.Region, error) {
	if strings.HasPrefix(strings.ToLower(start), "uri:") {
		name := strings.TrimPrefix(start, "uri:")
		if before, _, found := strings.Cut(name, "&"); found {
			name = before
		}
		destination, err := arrival.ResolveNamed(ctx, store, name)
		if err != nil {
			return regions.Region{}, regions.ErrNotFound
		}
		return destination.Region, nil
	}
	// preferredRegionID is the home region for start=home, else the last region.
	destination, err := arrival.Resolve(ctx, store, preferredRegionID, welcome)
	if err == nil {
		return destination.Region, nil
	}
	items, listErr := store.List(ctx)
	if listErr != nil || len(items) == 0 || len(welcome) > 0 {
		// With a welcome list configured, its exhaustion means no listed
		// region is online: refuse rather than land the user somewhere the
		// operator never named.
		return regions.Region{}, regions.ErrNotFound
	}
	return items[0], nil
}

func normalizeStart(start string) string {
	if strings.HasPrefix(strings.ToLower(start), "uri:") {
		return "url"
	}
	if strings.EqualFold(start, "home") {
		return "home"
	}
	return "last"
}

func newCircuitCode() (uint32, error) {
	var data [4]byte
	if _, err := rand.Read(data[:]); err != nil {
		return 0, err
	}
	value := binary.BigEndian.Uint32(data[:])
	value &= 0x7fffffff
	if value == 0 {
		value = 1
	}
	return value, nil
}

func loginFailure(reason, message string) rpcOutputValue {
	return rpcStructValue(rpcField("login", rpcString("false")), rpcField("reason", rpcString(reason)), rpcField("message", rpcString(message)))
}

// welcomeMessage renders the grid-wide login greeting for one avatar. This is
// the once-per-login message; per-region greetings are the regions' own.
func (a *API) welcomeMessage(displayName string) string {
	message := strings.ReplaceAll(a.welcomeText, "{grid}", a.gridName)
	return strings.ReplaceAll(message, "{user}", displayName)
}

func writeViewerLogin(w http.ResponseWriter, value rpcOutputValue) {
	w.Header().Set("Content-Type", "text/xml; charset=utf-8")
	w.WriteHeader(http.StatusOK)
	_, _ = w.Write([]byte(xml.Header))
	_ = xml.NewEncoder(w).Encode(rpcMethodResponse{Value: value})
}
