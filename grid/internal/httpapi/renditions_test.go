package httpapi

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"

	"github.com/homeworldz/server/grid/internal/renditions"
)

// memoryRenditions keeps the real store's contract — idempotent requests,
// lease-based claims, put-completes-job — without Postgres.
type memoryRenditions struct {
	jobs   map[string]renditions.Job // keyed asset/kind
	bytes  map[string][]byte
	stored map[string]renditions.Rendition
}

func newMemoryRenditions() *memoryRenditions {
	return &memoryRenditions{jobs: map[string]renditions.Job{},
		bytes: map[string][]byte{}, stored: map[string]renditions.Rendition{}}
}

func key(assetID, kind string) string { return assetID + "/" + kind }

func (m *memoryRenditions) Request(_ context.Context, assetID, kind string) (renditions.Job, error) {
	if kind != "sl-mesh" && kind != "gltf" && kind != "sl-material" && kind != "j2c-texture" {
		return renditions.Job{}, renditions.ErrInvalid
	}
	if assetID == absentAssetID {
		return renditions.Job{}, renditions.ErrUnknownAsset
	}
	if job, exists := m.jobs[key(assetID, kind)]; exists && job.State != "failed" {
		return job, nil
	}
	job := renditions.Job{ID: "aaaaaaaa-0000-4000-8000-00000000000" +
		string(rune('1'+len(m.jobs))), AssetID: assetID, Kind: kind, State: "queued",
		CreatedAt: time.Unix(1, 0).UTC(), UpdatedAt: time.Unix(1, 0).UTC()}
	m.jobs[key(assetID, kind)] = job
	return job, nil
}

func (m *memoryRenditions) Claim(_ context.Context, kinds []string, _ time.Duration) (renditions.Job, bool, error) {
	if len(kinds) == 0 {
		return renditions.Job{}, false, renditions.ErrInvalid
	}
	for _, kind := range kinds {
		for jobKey, job := range m.jobs {
			if job.Kind == kind && job.State == "queued" {
				job.State = "leased"
				job.Attempts++
				m.jobs[jobKey] = job
				return job, true, nil
			}
		}
	}
	return renditions.Job{}, false, nil
}

func (m *memoryRenditions) Fail(_ context.Context, jobID, reason string, permanent bool) error {
	for jobKey, job := range m.jobs {
		if job.ID == jobID && job.State == "leased" {
			job.State = "queued"
			if permanent {
				job.State = "failed"
			}
			job.Error = reason
			m.jobs[jobKey] = job
			return nil
		}
	}
	return renditions.ErrNotFound
}

func (m *memoryRenditions) Put(_ context.Context, assetID, kind, generator string,
	content io.Reader) (renditions.Rendition, error) {
	if generator == "" {
		return renditions.Rendition{}, renditions.ErrInvalid
	}
	if assetID == absentAssetID {
		return renditions.Rendition{}, renditions.ErrUnknownAsset
	}
	body, err := io.ReadAll(content)
	if err != nil || len(body) == 0 {
		return renditions.Rendition{}, renditions.ErrInvalid
	}
	sum := sha256.Sum256(body)
	value := renditions.Rendition{AssetID: assetID, Kind: kind, BlobID: testBlobID,
		Generator: generator, GeneratedAt: time.Unix(2, 0).UTC(),
		ByteLength: int64(len(body)), Checksum: hex.EncodeToString(sum[:])}
	m.bytes[key(assetID, kind)] = body
	m.stored[key(assetID, kind)] = value
	if job, exists := m.jobs[key(assetID, kind)]; exists {
		job.State = "done"
		m.jobs[key(assetID, kind)] = job
	}
	return value, nil
}

func (m *memoryRenditions) RequeueStale(_ context.Context, kind, currentGenerator string) (int64, error) {
	if (kind != "sl-mesh" && kind != "gltf" && kind != "sl-material" && kind != "j2c-texture") ||
		currentGenerator == "" {
		return 0, renditions.ErrInvalid
	}
	var requeued int64
	for _, value := range m.stored {
		if value.Kind != kind || value.Generator == currentGenerator {
			continue
		}
		jobKey := key(value.AssetID, kind)
		if job, exists := m.jobs[jobKey]; exists && job.State != "done" && job.State != "failed" {
			continue
		}
		job := m.jobs[jobKey]
		job.AssetID, job.Kind, job.State, job.Attempts, job.Error = value.AssetID, kind, "queued", 0, ""
		m.jobs[jobKey] = job
		requeued++
	}
	return requeued, nil
}

func (m *memoryRenditions) List(_ context.Context, assetID string) ([]renditions.Rendition, error) {
	var values []renditions.Rendition
	for _, value := range m.stored {
		if value.AssetID == assetID {
			values = append(values, value)
		}
	}
	return values, nil
}

func (m *memoryRenditions) Open(_ context.Context, assetID, kind string) (io.ReadCloser, renditions.Rendition, error) {
	body, exists := m.bytes[key(assetID, kind)]
	if !exists {
		return nil, renditions.Rendition{}, renditions.ErrNotFound
	}
	return io.NopCloser(bytes.NewReader(body)), m.stored[key(assetID, kind)], nil
}

func newRenditionHandler() (http.Handler, *memoryRenditions) {
	store := newMemoryRenditions()
	handler := New(checker{}, "test", Options{ServiceToken: "secret",
		WorkerToken: "worker-secret",
		Assets:      &memoryRegistry{blobs: nil}, Renditions: store})
	return handler, store
}

func requestRendition(t *testing.T, handler http.Handler, method, path, token string,
	body []byte, generator string, wantStatus int) *httptest.ResponseRecorder {
	t.Helper()
	r := httptest.NewRequest(method, path, bytes.NewReader(body))
	r.Header.Set("Authorization", "Bearer "+token)
	if generator != "" {
		r.Header.Set("X-Homeworldz-Generator", generator)
	}
	w := httptest.NewRecorder()
	handler.ServeHTTP(w, r)
	if w.Code != wantStatus {
		t.Fatalf("%s %s status = %d, want %d: %s", method, path, w.Code, wantStatus, w.Body.String())
	}
	return w
}

// TestRenditionLifecycle walks the M1 conversion loop end to end at the HTTP
// layer: a region requests, a worker claims, converts, and puts, and a region
// reads the bytes back for a viewer.
func TestRenditionLifecycle(t *testing.T) {
	handler, _ := newRenditionHandler()
	base := "/api/v1/assets/" + testAssetID + "/renditions"

	// The region enqueues with the service token.
	recorder := requestRendition(t, handler, http.MethodPost, base, "secret",
		[]byte(`{"kind":"sl-mesh"}`), "", http.StatusOK)
	var job renditions.Job
	if err := json.NewDecoder(recorder.Body).Decode(&job); err != nil || job.State != "queued" {
		t.Fatalf("requested job = %#v, %v", job, err)
	}
	// Requesting again is idempotent, not a second job.
	recorder = requestRendition(t, handler, http.MethodPost, base, "secret",
		[]byte(`{"kind":"sl-mesh"}`), "", http.StatusOK)
	var repeat renditions.Job
	_ = json.NewDecoder(recorder.Body).Decode(&repeat)
	if repeat.ID != job.ID {
		t.Fatalf("repeat request minted a new job: %#v vs %#v", repeat, job)
	}

	// Not converted yet: pending reads as not-yet, and the listing is empty
	// rather than an error.
	requestRendition(t, handler, http.MethodGet, base+"/sl-mesh", "secret", nil, "",
		http.StatusNotFound)
	recorder = requestRendition(t, handler, http.MethodGet, base, "secret", nil, "",
		http.StatusOK)
	if recorder.Body.String() != `{"renditions":[]}`+"\n" {
		t.Fatalf("empty listing = %q", recorder.Body.String())
	}

	// The worker claims with its own credential.
	recorder = requestRendition(t, handler, http.MethodPost, "/api/v1/rendition-jobs/claim",
		"worker-secret", []byte(`{"kinds":["sl-mesh"],"leaseSeconds":60}`), "", http.StatusOK)
	var claimed renditions.Job
	if err := json.NewDecoder(recorder.Body).Decode(&claimed); err != nil ||
		claimed.ID != job.ID || claimed.State != "leased" {
		t.Fatalf("claimed = %#v, %v", claimed, err)
	}
	// An empty queue answers 204, the normal resting state.
	requestRendition(t, handler, http.MethodPost, "/api/v1/rendition-jobs/claim",
		"worker-secret", []byte(`{"kinds":["gltf"],"leaseSeconds":60}`), "", http.StatusNoContent)

	// The worker stores the converted bytes; the job completes with them.
	converted := []byte("type-49 payload stand-in")
	recorder = requestRendition(t, handler, http.MethodPut, base+"/sl-mesh",
		"worker-secret", converted, "meshsmith/0.1", http.StatusOK)
	var stored renditions.Rendition
	if err := json.NewDecoder(recorder.Body).Decode(&stored); err != nil ||
		stored.Generator != "meshsmith/0.1" || stored.ByteLength != int64(len(converted)) {
		t.Fatalf("stored = %#v, %v", stored, err)
	}

	// A region reads the rendition back to serve a viewer.
	recorder = requestRendition(t, handler, http.MethodGet, base+"/sl-mesh", "secret",
		nil, "", http.StatusOK)
	if !bytes.Equal(recorder.Body.Bytes(), converted) {
		t.Fatalf("served rendition = %q", recorder.Body.Bytes())
	}
}

// TestRenditionWritesRequireTheWorkerCredential is ADR 0028 at this surface:
// the service token regions hold must not be able to write renditions or take
// conversion work, because a conversion cannot be checksum-verified.
func TestRenditionWritesRequireTheWorkerCredential(t *testing.T) {
	handler, _ := newRenditionHandler()
	base := "/api/v1/assets/" + testAssetID + "/renditions"

	recorder := requestRendition(t, handler, http.MethodPut, base+"/sl-mesh", "secret",
		[]byte("forged"), "rogue/1", http.StatusForbidden)
	var failure Error
	_ = json.NewDecoder(recorder.Body).Decode(&failure)
	if failure.Code != "worker_token_required" {
		t.Fatalf("service-token put = %#v", failure)
	}
	requestRendition(t, handler, http.MethodPost, "/api/v1/rendition-jobs/claim", "secret",
		[]byte(`{"kinds":["sl-mesh"]}`), "", http.StatusForbidden)

	// With no worker token configured, the endpoints are unavailable, not open.
	unconfigured := New(checker{}, "test", Options{ServiceToken: "secret",
		Assets: &memoryRegistry{}, Renditions: newMemoryRenditions()})
	requestRendition(t, unconfigured, http.MethodPut, base+"/sl-mesh", "secret",
		[]byte("forged"), "rogue/1", http.StatusServiceUnavailable)
}

// TestRenditionRegeneration is the converter-upgrade sweep: a stored
// rendition from an older generator re-queues; one already current does not;
// and the endpoint takes the worker credential only.
func TestRenditionRegeneration(t *testing.T) {
	handler, store := newRenditionHandler()
	base := "/api/v1/assets/" + testAssetID + "/renditions"

	// Convert once under the old generator.
	requestRendition(t, handler, http.MethodPost, base, "secret",
		[]byte(`{"kind":"sl-mesh"}`), "", http.StatusOK)
	requestRendition(t, handler, http.MethodPost, "/api/v1/rendition-jobs/claim",
		"worker-secret", []byte(`{"kinds":["sl-mesh"]}`), "", http.StatusOK)
	requestRendition(t, handler, http.MethodPut, base+"/sl-mesh", "worker-secret",
		[]byte("old bytes"), "meshsmith/0.3", http.StatusOK)

	// The service token may not trigger a sweep.
	requestRendition(t, handler, http.MethodPost, "/api/v1/rendition-jobs/regenerate",
		"secret", []byte(`{"kind":"sl-mesh","generator":"meshsmith/0.4"}`), "",
		http.StatusForbidden)

	// The upgraded worker sweeps: the stale rendition re-queues.
	recorder := requestRendition(t, handler, http.MethodPost, "/api/v1/rendition-jobs/regenerate",
		"worker-secret", []byte(`{"kind":"sl-mesh","generator":"meshsmith/0.4"}`), "",
		http.StatusOK)
	var swept struct {
		Requeued int64 `json:"requeued"`
	}
	if err := json.NewDecoder(recorder.Body).Decode(&swept); err != nil || swept.Requeued != 1 {
		t.Fatalf("sweep = %#v, %v", swept, err)
	}
	if job := store.jobs[key(testAssetID, "sl-mesh")]; job.State != "queued" || job.Attempts != 0 {
		t.Fatalf("requeued job = %#v", job)
	}

	// Reconvert under the current generator; a second sweep finds nothing.
	requestRendition(t, handler, http.MethodPost, "/api/v1/rendition-jobs/claim",
		"worker-secret", []byte(`{"kinds":["sl-mesh"]}`), "", http.StatusOK)
	requestRendition(t, handler, http.MethodPut, base+"/sl-mesh", "worker-secret",
		[]byte("current bytes"), "meshsmith/0.4", http.StatusOK)
	recorder = requestRendition(t, handler, http.MethodPost, "/api/v1/rendition-jobs/regenerate",
		"worker-secret", []byte(`{"kind":"sl-mesh","generator":"meshsmith/0.4"}`), "",
		http.StatusOK)
	swept.Requeued = -1
	if err := json.NewDecoder(recorder.Body).Decode(&swept); err != nil || swept.Requeued != 0 {
		t.Fatalf("second sweep = %#v, %v", swept, err)
	}

	// A missing generator or unknown kind is refused.
	requestRendition(t, handler, http.MethodPost, "/api/v1/rendition-jobs/regenerate",
		"worker-secret", []byte(`{"kind":"sl-mesh"}`), "", http.StatusBadRequest)
	requestRendition(t, handler, http.MethodPost, "/api/v1/rendition-jobs/regenerate",
		"worker-secret", []byte(`{"kind":"stl","generator":"meshsmith/0.4"}`), "",
		http.StatusBadRequest)
}

func TestRenditionValidationAndFailure(t *testing.T) {
	handler, store := newRenditionHandler()
	base := "/api/v1/assets/" + testAssetID + "/renditions"

	// Unknown kinds and unregistered assets are refused.
	requestRendition(t, handler, http.MethodPost, base, "secret",
		[]byte(`{"kind":"stl"}`), "", http.StatusBadRequest)
	requestRendition(t, handler, http.MethodPost,
		"/api/v1/assets/"+absentAssetID+"/renditions", "secret",
		[]byte(`{"kind":"sl-mesh"}`), "", http.StatusNotFound)
	// A put without a generator has no provenance and is refused.
	requestRendition(t, handler, http.MethodPut, base+"/sl-mesh", "worker-secret",
		[]byte("bytes"), "", http.StatusBadRequest)

	// A worker reporting failure re-queues the job with the reason kept.
	requestRendition(t, handler, http.MethodPost, base, "secret",
		[]byte(`{"kind":"sl-mesh"}`), "", http.StatusOK)
	recorder := requestRendition(t, handler, http.MethodPost, "/api/v1/rendition-jobs/claim",
		"worker-secret", []byte(`{"kinds":["sl-mesh"]}`), "", http.StatusOK)
	var claimed renditions.Job
	_ = json.NewDecoder(recorder.Body).Decode(&claimed)
	requestRendition(t, handler, http.MethodPost,
		"/api/v1/rendition-jobs/"+claimed.ID+"/fail", "worker-secret",
		[]byte(`{"error":"unsupported extension EXT_meshopt_compression"}`), "", http.StatusNoContent)
	if job := store.jobs[key(testAssetID, "sl-mesh")]; job.State != "queued" ||
		job.Error == "" {
		t.Fatalf("failed job = %#v", job)
	}
}
