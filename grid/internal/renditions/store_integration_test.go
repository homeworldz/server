package renditions

import (
	"bytes"
	"context"
	"database/sql"
	"errors"
	"io"
	"os"
	"testing"
	"time"

	_ "github.com/jackc/pgx/v5/stdlib"
)

// registerAsset creates the minimal canonical asset a rendition derives from:
// a blob and an asset row, cleaned up in reverse order.
func registerAsset(t *testing.T, db *sql.DB) string {
	t.Helper()
	var blobID, assetID string
	if err := db.QueryRow(`
		INSERT INTO blobs (byte_length, checksum, checksum_algorithm)
		VALUES (24, '00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff', 'sha256')
		RETURNING blob_id`).Scan(&blobID); err != nil {
		t.Fatal(err)
	}
	if err := db.QueryRow(`
		INSERT INTO assets (asset_id, blob_id, creator_user_id)
		VALUES (gen_random_uuid(), $1, gen_random_uuid()) RETURNING asset_id`, blobID).
		Scan(&assetID); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() {
		_, _ = db.Exec(`DELETE FROM rendition_jobs WHERE asset_id = $1`, assetID)
		_, _ = db.Exec(`DELETE FROM blobs WHERE blob_id IN (
			SELECT blob_id FROM asset_renditions WHERE asset_id = $1)`, assetID)
		_, _ = db.Exec(`DELETE FROM asset_renditions WHERE asset_id = $1`, assetID)
		_, _ = db.Exec(`DELETE FROM assets WHERE asset_id = $1`, assetID)
		_, _ = db.Exec(`DELETE FROM blobs WHERE blob_id = $1`, blobID)
	})
	return assetID
}

func TestPostgresRenditionLifecycle(t *testing.T) {
	databaseURL := os.Getenv("HOMEWORLDZ_TEST_DATABASE_URL")
	if databaseURL == "" {
		t.Skip("HOMEWORLDZ_TEST_DATABASE_URL is not configured")
	}
	db, err := sql.Open("pgx", databaseURL)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = db.Close() })
	ctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
	defer cancel()

	store, err := NewPostgresStore(db, t.TempDir())
	if err != nil {
		t.Fatal(err)
	}
	assetID := registerAsset(t, db)

	// Request is idempotent; a repeat returns the same job.
	job, err := store.Request(ctx, assetID, "sl-mesh")
	if err != nil || job.State != "queued" {
		t.Fatalf("request = %#v, %v", job, err)
	}
	repeat, err := store.Request(ctx, assetID, "sl-mesh")
	if err != nil || repeat.ID != job.ID {
		t.Fatalf("repeat request = %#v, %v", repeat, err)
	}
	if _, err := store.Request(ctx, assetID, "stl"); !errors.Is(err, ErrInvalid) {
		t.Fatalf("invalid kind = %v", err)
	}
	if _, err := store.Request(ctx,
		"eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee", "sl-mesh"); !errors.Is(err, ErrUnknownAsset) {
		t.Fatalf("unknown asset = %v", err)
	}

	// Claim leases it; a second claim finds nothing while the lease holds.
	claimed, ok, err := store.Claim(ctx, []string{"sl-mesh", "gltf"}, time.Minute)
	if err != nil || !ok || claimed.ID != job.ID || claimed.State != "leased" ||
		claimed.Attempts != 1 {
		t.Fatalf("claim = %#v %v %v", claimed, ok, err)
	}
	if _, ok, err := store.Claim(ctx, []string{"sl-mesh"}, time.Minute); err != nil || ok {
		t.Fatalf("second claim during lease = %v %v", ok, err)
	}

	// Failure re-queues with the reason; the job is claimable again.
	if err := store.Fail(ctx, claimed.ID, "converter exploded", false); err != nil {
		t.Fatalf("fail = %v", err)
	}
	reclaimed, ok, err := store.Claim(ctx, []string{"sl-mesh"}, time.Minute)
	if err != nil || !ok || reclaimed.Attempts != 2 {
		t.Fatalf("reclaim = %#v %v %v", reclaimed, ok, err)
	}

	// A permanent failure parks the job on the spot, with attempts far below
	// the cap: a worker that has read the bytes and found them unconvertible
	// knows what four more attempts would only rediscover.
	if err := store.Fail(ctx, reclaimed.ID, "the canonical image is neither PNG nor JPEG", true); err != nil {
		t.Fatalf("permanent fail = %v", err)
	}
	if _, ok, err := store.Claim(ctx, []string{"sl-mesh"}, time.Minute); err != nil || ok {
		t.Fatalf("a permanently failed job was claimed again: %v %v", ok, err)
	}
	// Requeued so the rest of the test still has a leased job to finish.
	if _, err := store.Request(ctx, assetID, "sl-mesh"); err != nil {
		t.Fatalf("requeue after permanent failure = %v", err)
	}
	reclaimed, ok, err = store.Claim(ctx, []string{"sl-mesh"}, time.Minute)
	if err != nil || !ok {
		t.Fatalf("reclaim after requeue = %#v %v %v", reclaimed, ok, err)
	}

	// Put stores bytes, mints the blob, records the rendition, completes the
	// job — and a second Put (a regeneration) replaces the record.
	payload := []byte("the derived sl-mesh bytes")
	stored, err := store.Put(ctx, assetID, "sl-mesh", "meshsmith/0.1", bytes.NewReader(payload))
	if err != nil || stored.ByteLength != int64(len(payload)) || stored.BlobID == "" {
		t.Fatalf("put = %#v, %v", stored, err)
	}
	listed, err := store.List(ctx, assetID)
	if err != nil || len(listed) != 1 || listed[0].Generator != "meshsmith/0.1" {
		t.Fatalf("list = %#v, %v", listed, err)
	}
	content, opened, err := store.Open(ctx, assetID, "sl-mesh")
	if err != nil {
		t.Fatalf("open = %v", err)
	}
	served, _ := io.ReadAll(content)
	content.Close()
	if !bytes.Equal(served, payload) || opened.Checksum != stored.Checksum {
		t.Fatalf("served = %q, %#v", served, opened)
	}
	var jobState string
	if err := db.QueryRow(`SELECT state FROM rendition_jobs WHERE id = $1`, job.ID).
		Scan(&jobState); err != nil || jobState != "done" {
		t.Fatalf("job state after put = %q, %v", jobState, err)
	}

	regenerated, err := store.Put(ctx, assetID, "sl-mesh", "meshsmith/0.2",
		bytes.NewReader([]byte("better bytes")))
	if err != nil || regenerated.BlobID == stored.BlobID {
		t.Fatalf("regeneration = %#v, %v", regenerated, err)
	}
	listed, _ = store.List(ctx, assetID)
	if len(listed) != 1 || listed[0].Generator != "meshsmith/0.2" {
		t.Fatalf("list after regeneration = %#v", listed)
	}

	// Missing renditions read as not-yet.
	if _, _, err := store.Open(ctx, assetID, "gltf"); !errors.Is(err, ErrNotFound) {
		t.Fatalf("open missing = %v", err)
	}
}

// TestRequeueStaleRevivesFailures covers the half of the sweep that reads no
// renditions at all.
//
// A job that failed stored nothing, so the stale-generator query cannot see it,
// and for a long time that meant a permanent failure outlived the fix for it:
// Character Creator bodies imported before rig retargeting existed had every
// sl-mesh job parked with "a skin binds joint CC_Base_Head" — precisely what
// the next generator was written to answer — and a sweep left all of them
// alone.
func TestRequeueStaleRevivesFailures(t *testing.T) {
	databaseURL := os.Getenv("HOMEWORLDZ_TEST_DATABASE_URL")
	if databaseURL == "" {
		t.Skip("HOMEWORLDZ_TEST_DATABASE_URL is not configured")
	}
	db, err := sql.Open("pgx", databaseURL)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = db.Close() })
	ctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
	defer cancel()

	store, err := NewPostgresStore(db, t.TempDir())
	if err != nil {
		t.Fatal(err)
	}

	// One asset whose conversion failed for good, and one that converted under
	// a generator that has since been superseded.
	failed := registerAsset(t, db)
	stale := registerAsset(t, db)

	if _, err := store.Request(ctx, failed, "sl-mesh"); err != nil {
		t.Fatal(err)
	}
	job, ok, err := store.Claim(ctx, []string{"sl-mesh"}, time.Minute)
	if err != nil || !ok {
		t.Fatalf("claim = %v, %v", ok, err)
	}
	// Exhaust the attempts so the job parks rather than returning to the queue.
	for attempt := 0; attempt < maxAttempts; attempt++ {
		if err := store.Fail(ctx, job.ID, "a skin binds joint \"CC_Base_Head\"", false); err != nil {
			t.Fatal(err)
		}
		if claimed, more, claimErr := store.Claim(ctx, []string{"sl-mesh"}, time.Minute); claimErr != nil {
			t.Fatal(claimErr)
		} else if more {
			job = claimed
		}
	}
	var parked string
	if err := db.QueryRow(`SELECT state FROM rendition_jobs WHERE asset_id = $1 AND kind = 'sl-mesh'`,
		failed).Scan(&parked); err != nil {
		t.Fatal(err)
	}
	if parked != "failed" {
		t.Fatalf("job state before sweep = %q, wanted failed", parked)
	}

	if _, err := store.Request(ctx, stale, "sl-mesh"); err != nil {
		t.Fatal(err)
	}
	if _, ok, err := store.Claim(ctx, []string{"sl-mesh"}, time.Minute); err != nil || !ok {
		t.Fatalf("claim stale = %v, %v", ok, err)
	}
	if _, err := store.Put(ctx, stale, "sl-mesh", "meshsmith/0.11",
		bytes.NewReader([]byte("converted under the old generator"))); err != nil {
		t.Fatal(err)
	}

	if _, err := store.RequeueStale(ctx, "sl-mesh", "meshsmith/0.12"); err != nil {
		t.Fatal(err)
	}

	// Both are queued again: the stale success because its generator differs,
	// and the parked failure because the converter that recorded it is gone.
	for _, subject := range []struct {
		assetID string
		why     string
	}{{failed, "parked failure"}, {stale, "stale success"}} {
		var state string
		var attempts int
		if err := db.QueryRow(
			`SELECT state, attempts FROM rendition_jobs WHERE asset_id = $1 AND kind = 'sl-mesh'`,
			subject.assetID).Scan(&state, &attempts); err != nil {
			t.Fatal(err)
		}
		if state != "queued" || attempts != 0 {
			t.Fatalf("%s after sweep = %q with %d attempts, wanted queued with 0",
				subject.why, state, attempts)
		}
	}
}
