package api

import "time"

// Error is the uniform error body. Code is a stable machine-readable slug.
type Error struct {
	Code    string `json:"code"`
	Message string `json:"message"`
	Field   string `json:"field,omitempty"`
}

// Identity is the avatar identity returned to the account's own session and to
// administrators. It is not a third-party profile: Email is on it, and no route
// serves an Identity to anyone but its owner or an admin.
type Identity struct {
	ID          string    `json:"id"`
	Userid      string    `json:"userid"`
	DisplayName string    `json:"displayName"`
	Email       string    `json:"email,omitempty"`
	RezDate     time.Time `json:"rezDate"`
	Privs       string    `json:"privs"`
}

// Ban is the account-suspension detail in a ManagedUser.
type Ban struct {
	Reason    string     `json:"reason"`
	ExpiresAt *time.Time `json:"expiresAt,omitempty"`
	BannedAt  time.Time  `json:"bannedAt"`
	BannedBy  string     `json:"bannedBy"`
}

// ManagedUser is an Identity plus administrative state.
type ManagedUser struct {
	Identity
	State string `json:"state"`
	Ban   *Ban   `json:"ban,omitempty"`
	Kind  string `json:"kind"`
	Tags  string `json:"tags"`
}

// UserPage is a page of managed users.
type UserPage struct {
	Users      []ManagedUser `json:"users"`
	NextCursor string        `json:"nextCursor,omitempty"`
}

// TokenResponse carries an issued website token and the authenticated identity.
type TokenResponse struct {
	AccessToken string    `json:"accessToken"`
	TokenType   string    `json:"tokenType"`
	ExpiresAt   time.Time `json:"expiresAt"`
	Identity    Identity  `json:"identity"`
}

// RegistrationPending is returned by registration; it echoes the derived userid.
type RegistrationPending struct {
	Userid      string `json:"userid"`
	DisplayName string `json:"displayName"`
}

// ManagedRegion is a provisioned region with derived online state.
type ManagedRegion struct {
	ID             string     `json:"id"`
	Name           string     `json:"name"`
	OwnerUserID    string     `json:"ownerUserId"`
	GridX          *int       `json:"gridX"`
	GridY          *int       `json:"gridY"`
	PublicEndpoint string     `json:"publicEndpoint"`
	ViewerPort     int        `json:"viewerPort"`
	Enabled        bool       `json:"enabled"`
	State          string     `json:"state"`
	LeaseExpiresAt *time.Time `json:"leaseExpiresAt,omitempty"`
	// Size is the square edge for a square region and 0 for a rectangle; it
	// predates sizeX/sizeY (ADR 0036) and is kept so existing clients keep
	// rendering square regions.
	Size       int      `json:"size"`
	SizeX      int      `json:"sizeX"`
	SizeY      int      `json:"sizeY"`
	FacetNames []string `json:"facetNames,omitempty"`
	Kind       string   `json:"kind"`
	Tags       string   `json:"tags"`
}

// RegionList is a list of provisioned regions.
type RegionList struct {
	Regions []ManagedRegion `json:"regions"`
}

// RegionDeployment returns a region together with a one-time access key.
type RegionDeployment struct {
	Region    ManagedRegion `json:"region"`
	AccessKey string        `json:"accessKey"`
}

// Request bodies.

type registerAvatarRequest struct {
	DisplayName string `json:"displayName"`
	Email       string `json:"email"`
}

type verifyRegistrationRequest struct {
	Code     string `json:"code"`
	Password string `json:"password"`
}

type resendVerificationRequest struct {
	Userid string `json:"userid"`
}

type createTokenRequest struct {
	Userid   string `json:"userid"`
	Password string `json:"password"`
}

type updateProfileRequest struct {
	DisplayName *string `json:"displayName"`
}

type changePasswordRequest struct {
	CurrentPassword string `json:"currentPassword"`
	NewPassword     string `json:"newPassword"`
}

type replacePrivilegesRequest struct {
	Privs string `json:"privs"`
}

type banUserRequest struct {
	Reason    string     `json:"reason"`
	ExpiresAt *time.Time `json:"expiresAt"`
}

type createRegionRequest struct {
	Name           string `json:"name"`
	OwnerUserID    string `json:"ownerUserId"`
	GridX          *int   `json:"gridX"`
	GridY          *int   `json:"gridY"`
	PublicEndpoint string `json:"publicEndpoint"`
	ViewerPort     *int   `json:"viewerPort"`
	// Size is the square shorthand; sizeX/sizeY supersede it for rectangles
	// (ADR 0036), which also need one facetName per facet beyond the first.
	Size       *int     `json:"size"`
	SizeX      *int     `json:"sizeX"`
	SizeY      *int     `json:"sizeY"`
	FacetNames []string `json:"facetNames"`
	Kind       string   `json:"kind"`
	Tags       string   `json:"tags"`
}

// setTagsRequest carries a classification update for a user or region.
type setTagsRequest struct {
	Kind string `json:"kind"`
	Tags string `json:"tags"`
}

type sendNoticeRequest struct {
	Message string `json:"message"`
}

// NoticeResult reports how many of the target user's open grid-channel
// connections accepted a notice. Zero means the user was not connected;
// notices are not stored for later.
type NoticeResult struct {
	Delivered int `json:"delivered"`
}

type sendMessageRequest struct {
	To      string `json:"to"`
	Message string `json:"message"`
}

// MessageSender identifies who sent an instant message, resolved for display.
type MessageSender struct {
	ID          string `json:"id"`
	Userid      string `json:"userid"`
	DisplayName string `json:"displayName"`
}

// MessageResult acknowledges a stored instant message. Delivered counts the
// recipient connections it reached live; zero means it waits in the backlog
// for their next channel connection — stored either way.
type MessageResult struct {
	ID        string    `json:"id"`
	SentAt    time.Time `json:"sentAt"`
	Delivered int       `json:"delivered"`
}

type updateRegionRequest struct {
	Name           *string `json:"name"`
	OwnerUserID    *string `json:"ownerUserId"`
	PublicEndpoint *string `json:"publicEndpoint"`
	ViewerPort     *int    `json:"viewerPort"`
}

type mapPositionRequest struct {
	GridX *int `json:"gridX"`
	GridY *int `json:"gridY"`
}

// passwordResetRequest is the body of POST /v1/password-resets. The identifier
// may be an account name or the email address on file; either is accepted so
// somebody who has forgotten one can use the other.
type passwordResetRequest struct {
	Identifier string `json:"identifier"`
}

// passwordResetConsumeRequest is the body of POST /v1/password-resets/{token}.
type passwordResetConsumeRequest struct {
	Password string `json:"password"`
}
