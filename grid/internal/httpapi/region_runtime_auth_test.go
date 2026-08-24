package httpapi

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"net/http"
	"net/http/httptest"
	"testing"

	"github.com/homeworldz/server/grid/internal/provisioning"
)

// unreachableRegistry is a provisioning store whose Authenticate cannot answer
// — a database that is restarting, which is exactly the case that used to be
// reported to regions as a bad access key.
type unreachableRegistry struct{ provisioning.Store }

func (unreachableRegistry) Authenticate(context.Context, string, string) (provisioning.Region, bool, error) {
	return provisioning.Region{}, false, errors.New("connection refused")
}

// TestRegionRuntimeSeparatesUnreachableFromUnauthorized pins the distinction
// that cost Beta and Homeworldz Strait three days of downtime on 2026-08-21: a
// region told 401 gives up its lease and exits — cleanly, so systemd does not
// restart it — while 503 is something to retry. A store that could not check
// the credential must never produce the first.
func TestRegionRuntimeSeparatesUnreachableFromUnauthorized(t *testing.T) {
	const id = "11111111-1111-4111-8111-111111111111"
	handler := New(checker{}, "test", Options{
		ServiceToken: "secret", Regions: newMemoryRegionStore(),
		Provisioned: unreachableRegistry{},
	})

	request := httptest.NewRequest(http.MethodPut, "/api/v1/region-runtime/"+id+"/lease",
		bytes.NewBufferString(`{"leaseSeconds":300}`))
	request.Header.Set("Authorization", "Bearer any-key")
	request.Header.Set("Content-Type", "application/json")
	response := httptest.NewRecorder()
	handler.ServeHTTP(response, request)

	if response.Code != http.StatusServiceUnavailable {
		t.Fatalf("status = %d, want 503: %s", response.Code, response.Body.String())
	}
	if response.Header().Get("Retry-After") == "" {
		t.Fatal("a retryable answer should say how long to wait")
	}
	// The refusal header belongs to a refusal. Sending it here would tell a
	// region its credential was rejected in the one case where nobody looked.
	if response.Header().Get("WWW-Authenticate") != "" {
		t.Fatal("an unreachable registry must not challenge the credential")
	}
	var body Error
	if err := json.Unmarshal(response.Body.Bytes(), &body); err != nil {
		t.Fatal(err)
	}
	if body.Code != "region_registry_unavailable" {
		t.Fatalf("error code = %q", body.Code)
	}
}
