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
	"strconv"
	"strings"
	"time"

	"github.com/homeworldz/server/grid/internal/arrival"
	"github.com/homeworldz/server/grid/internal/eventlog"
	"github.com/homeworldz/server/grid/internal/gestures"
	"github.com/homeworldz/server/grid/internal/identity"
	"github.com/homeworldz/server/grid/internal/inventory"
	"github.com/homeworldz/server/grid/internal/locations"
	"github.com/homeworldz/server/grid/internal/provisioning"
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
	// "presence" is the reason code a viewer renders as a plain message
	// rather than as a credential problem, which is what this is: the
	// credentials were right and the account is not allowed in. Told plainly,
	// because the person knows they were banned and an incorrect-password
	// answer would only send them to support with a false report.
	if errors.Is(err, identity.ErrBanned) {
		return nil, "presence", "This account is suspended."
	}
	if err != nil {
		return nil, "unavailable", "The Homeworldz grid could not create a session."
	}
	preferredRegionID := ""
	var storedPosition *[3]float32
	var storedLookAt *[3]float32
	if a.locations != nil {
		remember := func(location locations.Location) {
			preferredRegionID = location.RegionID
			position := location.Position
			storedPosition = &position
			// Kept so home and last can restore the facing too, not just the
			// region (operator, 2026-08-23). A degenerate look-at is dropped
			// rather than sent, so the region falls back to its own constant
			// instead of aiming the avatar at nothing.
			if math.Hypot(float64(location.LookAt[0]), float64(location.LookAt[1])) >= 0.001 {
				look := location.LookAt
				storedLookAt = &look
			}
		}
		if strings.EqualFold(start, "home") {
			if location, locationErr := a.locations.GetHome(r.Context(), session.UserID); locationErr == nil {
				remember(location)
			}
		} else if location, locationErr := a.locations.Get(r.Context(), session.UserID); locationErr == nil {
			remember(location)
		}
	}

	region, requestedFacet, err := resolveDestination(r.Context(), a.regions, a.provisioned, start, preferredRegionID, a.welcomePoints)
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
	// The same preparation client world entry does, and for the same reason:
	// an avatar needs its folder skeleton and something to wear before it
	// reaches a region. See inventory.Bootstrap.
	folders, err := inventory.Bootstrap(r.Context(), a.inventory, session.UserID)
	if err != nil {
		a.logger.Error("prepare viewer inventory", "error", err, "user", session.UserID)
		_ = a.identity.RevokeSession(r.Context(), session.ID)
		return nil, "unavailable", "The grid could not prepare the viewer inventory."
	}
	lookAt := "[r1,r0,r0]"
	var spawnPosition *[3]float64
	if state, ok := a.regionStartState(r.Context(), region.PublicEndpoint, session.UserID); ok {
		if state.LookAt != nil {
			lookAt = fmt.Sprintf("[r%.9g,r%.9g,r%.9g]", state.LookAt[0], state.LookAt[1], state.LookAt[2])
		}
		spawnPosition = state.Position
	}
	// The coordinates the login screen named, if any. Delivered to the region
	// before the reply goes out, because the viewer's CompleteAgentMovement
	// follows within seconds and the region spawns from whatever it knows then.
	// A refusal is not fatal: the login proceeds and the avatar appears at its
	// persisted spot, which is the behaviour that existed before this and is
	// better than failing a sign-in over a position.
	// The coordinates a login screen names are local to the region the viewer
	// named — and when that name is a facet, the facet IS the whole region to
	// the viewer (ADR 0036). So "Nova 2/4/4/25" means 4m into Nova 2, which is
	// macro (260,4) in Nova, not (4,4). Passed through unrebased it lands in a
	// different facet: the same mistake the transit arrival path documents and
	// rebases for, which I read and did not apply (found live 2026-08-23).
	//
	// The named facet therefore decides the facet, rather than being recomputed
	// from an unrebased position — recomputing gave facet 0 for every request.
	var requestedPosition *[3]float64
	requestedFacetSpawn := 0
	if requestedFacet > 0 {
		requestedFacetSpawn = requestedFacet
	}
	if requested, ok := parseRequestedStart(start); ok && requested.position != nil && a.provisioned != nil {
		if provisioned, provisionErr := a.provisioned.Get(r.Context(), region.ID); provisionErr == nil &&
			requestedFacetSpawn < provisioned.FacetCount() {
			originX, originY := provisioned.FacetOrigin(requestedFacetSpawn)
			macro := [3]float64{
				requested.position[0] + float64((originX-provisioned.MapX)*256),
				requested.position[1] + float64((originY-provisioned.MapY)*256),
				requested.position[2],
			}
			if a.postLoginSpawn(r.Context(), region.PublicEndpoint, session.UserID, macro, nil) {
				requestedPosition = &macro
				// A login that named coordinates named a place and nothing
				// else, so the facing is decided rather than inherited — the
				// same request must not land two ways depending on where the
				// avatar last looked (operator, 2026-08-23). This is Halcyon's
				// constant for the same case, and the region applies the
				// matching body rotation, so camera and avatar agree.
				// Restoring "last" or "home" keeps the stored facing, which
				// there is part of what was asked for.
				lookAt = "[r0,r1,r0]"
			}
		}
	}

	// Home and last restore the full position and the facing, not just the
	// region (operator, 2026-08-23). Until now they picked a region and the
	// region placed the avatar wherever its own leftover scene entity happened
	// to sit — so asking for Home could land you somewhere you had never set,
	// which is how a slope-trapped entity kept recapturing its owner.
	//
	// Only when the resolved region IS the stored one: a login diverted to the
	// welcome list must not be handed coordinates from somewhere else, which
	// would place the avatar by numbers that mean nothing there.
	if requestedPosition == nil && storedPosition != nil && preferredRegionID == region.ID {
		stored := [3]float64{
			float64(storedPosition[0]), float64(storedPosition[1]), float64(storedPosition[2]),
		}
		var look *[3]float64
		if storedLookAt != nil {
			look = &[3]float64{
				float64(storedLookAt[0]), float64(storedLookAt[1]), float64(storedLookAt[2]),
			}
		}
		if a.postLoginSpawn(r.Context(), region.PublicEndpoint, session.UserID, stored, look) {
			requestedPosition = &stored
			if look != nil {
				lookAt = fmt.Sprintf("[r%.9g,r%.9g,r%.9g]", (*look)[0], (*look)[1], (*look)[2])
			}
		}
	}

	// A viewer logs into one square facet of the region (ADR 0036): the facet
	// containing the arrival position, or facet 0 when nothing places the
	// avatar more precisely. The region's own persisted spawn decides when it
	// is known — CompleteAgentMovement spawns the avatar there regardless of
	// what the grid's locations store remembers, and a login handed the wrong
	// facet arrives standing across an internal line, which fires the crossing
	// ceremony into a viewer still logging in (seen live 2026-08-20: Firestorm
	// reports mangled network data and the session dies). A square region is
	// exactly its single facet, so nothing changes for it here.
	regionSizeX, regionSizeY := 256, 256
	simPort := region.ViewerPort
	regionX, regionY := region.GridX*256, region.GridY*256
	if a.provisioned != nil {
		if provisioned, provisionErr := a.provisioned.Get(r.Context(), region.ID); provisionErr == nil {
			// A position the login screen named outranks both, because it is
			// what the user asked for and the region has been told to spawn
			// them there. Below that, the region's own persisted spawn decides,
			// since CompleteAgentMovement uses it regardless of which facet this
			// reply names — and a login handed the wrong facet arrives standing
			// across an internal line, which fires the crossing ceremony into a
			// viewer still logging in (2026-08-20).
			facet := 0
			if requestedFacet > 0 && requestedFacet < provisioned.FacetCount() {
				facet = requestedFacet
			}
			if requestedPosition != nil {
				facet = provisioned.FacetAtPosition(requestedPosition[0], requestedPosition[1])
			} else if spawnPosition != nil {
				facet = provisioned.FacetAtPosition(spawnPosition[0], spawnPosition[1])
			} else if storedPosition != nil && preferredRegionID == region.ID {
				facet = provisioned.FacetAtPosition(float64(storedPosition[0]), float64(storedPosition[1]))
			}
			edge := provisioned.FacetEdge() * 256
			regionSizeX, regionSizeY = edge, edge
			originX, originY := provisioned.FacetOrigin(facet)
			regionX, regionY = originX*256, originY*256
			simPort = region.ViewerPort + facet
		}
	}
	// The login is complete here and nowhere earlier: every failure above
	// revoked the session, and a login that never reached a region is not one
	// a person made. This is what the active-user figures count.
	eventlog.Note(r.Context(), a.events, a.logger, eventlog.Event{
		Kind: eventlog.KindLogin, UserID: session.UserID, RegionID: region.ID,
		Detail: region.Name,
	})
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
	Position *[3]float64 `json:"position"`
	LookAt   *[3]float64 `json:"lookAt"`
}

// postLoginSpawn tells the region where a login asked to appear, and optionally
// which way to face. The region holds it one-shot for the CompleteAgentMovement
// that follows, and raises it to ground level if it is below the terrain.
//
// A named position (uri:Region&x&y&z) sends no look-at, because it names a place
// and nothing else, and the region applies a deterministic facing. Home and last
// send theirs, because a stored facing is part of what was asked for.
//
// This exists because start-state runs the other way: it asks the region where
// the avatar WOULD spawn so the grid can pick the facet, and there was no way to
// say where it SHOULD. Reusing an avatar transit was the tempting alternative,
// since the region's arrival path already honours a transit's position, look-at
// and flying for free — but avatar_transits requires a source region that
// differs from the destination, and a login has no source at all. That CHECK is
// what makes a transit mean "an avatar is moving from A to B" (ADR 0025), so a
// login gets its own channel rather than weakening one.
func (a *API) postLoginSpawn(ctx context.Context, endpoint, userID string,
	position [3]float64, lookAt *[3]float64) bool {
	if a.serviceToken == "" {
		return false
	}
	document := struct {
		Position [3]float64  `json:"position"`
		LookAt   *[3]float64 `json:"lookAt,omitempty"`
	}{Position: position, LookAt: lookAt}
	body, err := json.Marshal(document)
	if err != nil {
		return false
	}
	request, err := http.NewRequestWithContext(ctx, http.MethodPost,
		strings.TrimRight(endpoint, "/")+"/api/v1/agents/"+url.PathEscape(userID)+"/login-spawn",
		bytes.NewReader(body))
	if err != nil {
		return false
	}
	request.Header.Set("Authorization", "Bearer "+a.serviceToken)
	request.Header.Set("Content-Type", "application/json")
	client := http.Client{Timeout: time.Second}
	response, err := client.Do(request)
	if err != nil {
		return false
	}
	defer response.Body.Close()
	_, _ = io.Copy(io.Discard, io.LimitReader(response.Body, 4096))
	return response.StatusCode == http.StatusOK
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
	// Field validity is independent: a degenerate lookAt must not discard a
	// good position (the facet pick depends on it), and vice versa.
	if state.LookAt != nil {
		length := math.Hypot(state.LookAt[0], state.LookAt[1])
		if !isFinite(state.LookAt[0]) || !isFinite(state.LookAt[1]) || !isFinite(state.LookAt[2]) || length < 0.001 {
			state.LookAt = nil
		}
	}
	if state.Position != nil &&
		(!isFinite(state.Position[0]) || !isFinite(state.Position[1]) || !isFinite(state.Position[2])) {
		state.Position = nil
	}
	if state.LookAt == nil && state.Position == nil {
		return regionStartState{}, false
	}
	return state, true
}

func isFinite(value float64) bool { return !math.IsNaN(value) && !math.IsInf(value, 0) }

// requestedStart is the destination a viewer typed on the login screen:
// "uri:Region&x&y&z", which Firestorm sends for a location field of
// "Region/x/y/z". The name resolves the region; the coordinates were parsed
// and thrown away until 2026-08-23, so every login with a named position
// landed on the avatar's leftover scene entity instead.
type requestedStart struct {
	name     string
	position *[3]float64
}

func parseRequestedStart(start string) (requestedStart, bool) {
	if !strings.HasPrefix(strings.ToLower(start), "uri:") {
		return requestedStart{}, false
	}
	fields := strings.Split(strings.TrimPrefix(start, "uri:"), "&")
	result := requestedStart{name: fields[0]}
	if len(fields) < 4 {
		return result, true
	}
	// All three or none: two good numbers and one bad would place an avatar
	// somewhere nobody asked for, which is worse than ignoring the request.
	var values [3]float64
	for index := 0; index < 3; index++ {
		value, err := strconv.ParseFloat(strings.TrimSpace(fields[index+1]), 64)
		if err != nil || !isFinite(value) || value < 0 {
			return result, true
		}
		values[index] = value
	}
	result.position = &values
	return result, true
}

// resolveDestination selects the viewer's login region on the shared arrival
// logic (docs/CLIENT2.md, "Default and fallback arrival points"): an explicit
// uri: destination is honored or refused (never diverted), a stored region
// wins while it is leased, and otherwise the welcome list decides. The legacy
// first-region fallback survives only for grids with no welcome list, so an
// unconfigured development grid still logs a viewer in somewhere.
//
// The returned facet is the one the uri: named when it named a facet of a
// rectangular region rather than the region itself (ADR 0036: to the viewer
// every facet is a region, so its name must be a valid destination on every
// discovery surface, login included), and -1 otherwise. The caller treats it
// as a default: a persisted spawn position still decides the arrival facet,
// because the region spawns the avatar there regardless and a facet that
// disagrees with the spawn fires the crossing ceremony into a viewer still
// logging in (2026-08-20).
func resolveDestination(ctx context.Context, store regions.Store, provisioned provisioning.Store,
	start, preferredRegionID string, welcome []arrival.Point) (regions.Region, int, error) {
	if strings.HasPrefix(strings.ToLower(start), "uri:") {
		name := strings.TrimPrefix(start, "uri:")
		if before, _, found := strings.Cut(name, "&"); found {
			name = before
		}
		destination, err := arrival.ResolveNamed(ctx, store, name)
		if err == nil {
			return destination.Region, -1, nil
		}
		if provisioned != nil {
			if region, facet, ok := resolveFacetNamed(ctx, store, provisioned, name); ok {
				return region, facet, nil
			}
		}
		return regions.Region{}, -1, regions.ErrNotFound
	}
	// preferredRegionID is the home region for start=home, else the last region.
	destination, err := arrival.Resolve(ctx, store, preferredRegionID, welcome)
	if err == nil {
		return destination.Region, -1, nil
	}
	items, listErr := store.List(ctx)
	if listErr != nil || len(items) == 0 || len(welcome) > 0 {
		// With a welcome list configured, its exhaustion means no listed
		// region is online: refuse rather than land the user somewhere the
		// operator never named.
		return regions.Region{}, -1, regions.ErrNotFound
	}
	return items[0], -1, nil
}

// resolveFacetNamed matches a login destination against the provisioned
// facet names (facet 0 carries the region's own name, which the live lookup
// already answered). The region still has to be online — facet names come
// from the provisioned record, but only the live store can say the region is
// actually there to log into.
func resolveFacetNamed(ctx context.Context, store regions.Store, provisioned provisioning.Store,
	name string) (regions.Region, int, bool) {
	items, err := provisioned.List(ctx)
	if err != nil {
		return regions.Region{}, -1, false
	}
	trimmed := strings.TrimSpace(name)
	for _, item := range items {
		for facet := 1; facet < item.FacetCount(); facet++ {
			if !strings.EqualFold(item.FacetNameAt(facet), trimmed) {
				continue
			}
			live, liveErr := store.Get(ctx, item.ID)
			if liveErr != nil {
				return regions.Region{}, -1, false
			}
			return live, facet, true
		}
	}
	return regions.Region{}, -1, false
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
