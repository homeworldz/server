package httpapi

import (
	"context"
	"net/http"
	"testing"

	"github.com/homeworldz/server/grid/internal/attachments"
)

type memoryAttachmentStore struct {
	worn []attachments.Attachment
	err  error
}

func (s *memoryAttachmentStore) ListWorn(
	_ context.Context, _ string,
) ([]attachments.Attachment, error) {
	return s.worn, s.err
}

func (s *memoryAttachmentStore) Wear(_ context.Context, _, itemID string, point int) error {
	for index, worn := range s.worn {
		if worn.ItemID == itemID {
			s.worn[index].AttachmentPoint = point
			return nil
		}
	}
	s.worn = append(s.worn, attachments.Attachment{ItemID: itemID, AttachmentPoint: point})
	return nil
}

func (s *memoryAttachmentStore) TakeOff(_ context.Context, _, itemID string) error {
	for index, worn := range s.worn {
		if worn.ItemID == itemID {
			s.worn = append(s.worn[:index], s.worn[index+1:]...)
			return nil
		}
	}
	return nil
}

func TestWornAttachmentsRoundTrip(t *testing.T) {
	store := &memoryAttachmentStore{}
	handler := New(checker{}, "test", Options{ServiceToken: "secret", Attachments: store})
	const userID = "20000000-0000-4000-8000-000000000001"
	const itemID = "10000000-0000-4000-8000-000000000001"
	const path = "/api/v1/attachments/" + userID

	// Wearing nothing is an empty list, not an error and not an absent body:
	// the region has to tell "wearing nothing" from "could not ask".
	worn := requestRegion[[]attachments.Attachment](t, handler, http.MethodGet, path, "", http.StatusOK)
	if len(worn) != 0 {
		t.Fatalf("initial worn set = %#v", worn)
	}

	requestRegion[struct{}](t, handler, http.MethodPut, path,
		`{"itemId":"`+itemID+`","attachmentPoint":5,"worn":true}`, http.StatusNoContent)
	worn = requestRegion[[]attachments.Attachment](t, handler, http.MethodGet, path, "", http.StatusOK)
	if len(worn) != 1 || worn[0].ItemID != itemID || worn[0].AttachmentPoint != 5 {
		t.Fatalf("worn after wearing = %#v", worn)
	}

	// Wearing the same item again moves it. Two rows here would be two copies
	// of one object hanging off the avatar after the next relog.
	requestRegion[struct{}](t, handler, http.MethodPut, path,
		`{"itemId":"`+itemID+`","attachmentPoint":31,"worn":true}`, http.StatusNoContent)
	worn = requestRegion[[]attachments.Attachment](t, handler, http.MethodGet, path, "", http.StatusOK)
	if len(worn) != 1 || worn[0].AttachmentPoint != 31 {
		t.Fatalf("worn after re-wearing = %#v", worn)
	}

	requestRegion[struct{}](t, handler, http.MethodPut, path,
		`{"itemId":"`+itemID+`","worn":false}`, http.StatusNoContent)
	worn = requestRegion[[]attachments.Attachment](t, handler, http.MethodGet, path, "", http.StatusOK)
	if len(worn) != 0 {
		t.Fatalf("worn after taking off = %#v", worn)
	}
}

func TestWornAttachmentsRejectUnresolvedPoints(t *testing.T) {
	store := &memoryAttachmentStore{}
	handler := New(checker{}, "test", Options{ServiceToken: "secret", Attachments: store})
	const path = "/api/v1/attachments/20000000-0000-4000-8000-000000000001"
	const itemID = "10000000-0000-4000-8000-000000000001"

	// Point 0 is the viewer asking the region to choose. Storing it would
	// record a question as an answer, and the next login would rez the item
	// onto a point that does not exist.
	requestRegion[Error](t, handler, http.MethodPut, path,
		`{"itemId":"`+itemID+`","attachmentPoint":0,"worn":true}`, http.StatusBadRequest)
	requestRegion[Error](t, handler, http.MethodPut, path,
		`{"itemId":"`+itemID+`","attachmentPoint":200,"worn":true}`, http.StatusBadRequest)
	requestRegion[Error](t, handler, http.MethodPut, path,
		`{"itemId":"not-a-uuid","attachmentPoint":5,"worn":true}`, http.StatusBadRequest)
	if len(store.worn) != 0 {
		t.Fatalf("rejected requests stored %#v", store.worn)
	}
	// Taking off needs no point: the item names what to remove.
	requestRegion[struct{}](t, handler, http.MethodPut, path,
		`{"itemId":"`+itemID+`","worn":false}`, http.StatusNoContent)
}

func TestWornAttachmentsWithoutAStoreAreUnavailable(t *testing.T) {
	handler := New(checker{}, "test", Options{ServiceToken: "secret"})
	// Not 200-with-nothing: a grid that cannot answer must not look like a
	// grid answering "you are wearing nothing", or arrival silently strips.
	requestRegion[Error](t, handler, http.MethodGet,
		"/api/v1/attachments/20000000-0000-4000-8000-000000000001", "",
		http.StatusServiceUnavailable)
}
