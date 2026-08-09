package httpapi

import (
	"context"
	"net/http"
	"net/url"
	"strings"
	"time"

	"github.com/homeworldz/server/grid/internal/inventory"
)

// Telling a region that a wearer changed clothes.
//
// A region bakes a wearer's appearance from the Current Outfit folder, and that
// folder lives here. Arrival re-reads it, so a relog or a crossing has never
// needed telling; the gap is the middle of a session, where an outfit change is
// an inventory write on the grid that nothing carries to the region holding the
// avatar. A client that bakes for itself sends its own AgentSetAppearance and
// needs none of this — one that does not has no way to say anything at all.
//
// Best effort by design. The outfit is already changed and already durable when
// this runs; a region that cannot be reached leaves an avatar wearing what it
// wore until it next arrives somewhere, which is the same state it was in before
// this existed. So a failure is logged and never returned: it must not fail the
// inventory operation that succeeded.

// currentOutfitVersion reads the wearer's Current Outfit folder version, or 0
// when it cannot be read. Callers use it only to compare two readings taken the
// same way, so an unreadable folder simply compares equal to itself and nothing
// is sent.
func (a *API) currentOutfitVersion(ctx context.Context, userID string) int64 {
	if a.inventory == nil {
		return 0
	}
	versions, err := a.inventoryFolderVersions(ctx, userID, inventory.SystemFolderID(userID, 46))
	if err != nil {
		return 0
	}
	return versions[inventory.SystemFolderID(userID, 46)]
}

// notifyOutfitChanged asks the region the wearer is on to re-bake. It resolves
// presence and the region's endpoint itself, because the caller is an inventory
// handler that has no reason to know either.
func (a *API) notifyOutfitChanged(ctx context.Context, userID string) {
	if a.presence == nil || a.regions == nil || a.serviceToken == "" {
		return
	}
	where, err := a.presence.Get(ctx, userID)
	if err != nil {
		// Not logged in, or nowhere: an outfit change with no avatar to apply
		// it to is the ordinary case, not a fault.
		return
	}
	region, err := a.regions.Get(ctx, where.RegionID)
	if err != nil || strings.TrimSpace(region.PublicEndpoint) == "" {
		return
	}
	endpoint, err := url.JoinPath(region.PublicEndpoint, "appearance", "refresh", userID)
	if err != nil {
		return
	}
	// Its own timeout and its own goroutine: this is a side effect of a request
	// that has already been answered, and it must not hold that request open or
	// outlive it by much.
	go func() {
		callCtx, cancel := context.WithTimeout(context.WithoutCancel(ctx), 5*time.Second)
		defer cancel()
		request, err := http.NewRequestWithContext(callCtx, http.MethodPost, endpoint, nil)
		if err != nil {
			return
		}
		request.Header.Set("Authorization", "Bearer "+a.serviceToken)
		response, err := a.outfitHTTP.Do(request)
		if err != nil {
			if a.logger != nil {
				a.logger.Warn("outfit change could not be delivered to the region",
					"userId", userID, "regionId", where.RegionID, "error", err)
			}
			return
		}
		defer response.Body.Close()
		// 404 is the region saying the avatar is not there, which is what a
		// stale presence row looks like and is not worth a warning.
		if response.StatusCode != http.StatusOK && response.StatusCode != http.StatusNotFound {
			if a.logger != nil {
				a.logger.Warn("region refused an outfit change notification",
					"userId", userID, "regionId", where.RegionID, "status", response.StatusCode)
			}
		}
	}()
}
