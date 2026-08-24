package api

import (
	"context"
	"errors"
	"net/http"
	"net/url"
	"strings"
	"time"

	"github.com/homeworldz/server/grid/internal/eventlog"
	"github.com/homeworldz/server/grid/internal/mailer"
	"github.com/homeworldz/server/grid/internal/webaccount"
)

// registrations handles POST /v1/registrations (public, rate-limited).
func (a *API) registrations(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		methodNotAllowed(w, http.MethodPost)
		return
	}
	if a.rateLimit(w, r) {
		return
	}
	if a.accounts == nil {
		writeError(w, http.StatusServiceUnavailable, Error{Code: "account_store_unavailable", Message: "account storage is unavailable"})
		return
	}
	var request registerAvatarRequest
	if !decodeJSON(w, r, &request) {
		return
	}
	account, code, err := a.accounts.Register(r.Context(), request.DisplayName, request.Email)
	switch {
	case errors.Is(err, webaccount.ErrInvalidDisplayName):
		writeError(w, http.StatusBadRequest, Error{Code: "invalid_display_name", Message: "display name must be two words that form a 3-32 character userid", Field: "displayName"})
		return
	case errors.Is(err, webaccount.ErrInvalidEmail):
		writeError(w, http.StatusBadRequest, Error{Code: "invalid_email", Message: "a valid email address is required", Field: "email"})
		return
	case errors.Is(err, webaccount.ErrConflict):
		writeError(w, http.StatusConflict, Error{Code: "userid_taken", Message: "that display name is already registered"})
		return
	case err != nil:
		a.internalError(w, r, "register account", err)
		return
	}
	eventlog.Note(r.Context(), a.events, a.logger, eventlog.Event{
		Kind: eventlog.KindRegistration, UserID: account.ID,
	})
	a.sendVerificationEmail(r.Context(), account.Userid, request.Email, code)
	w.Header().Set("Location", "/v1/account")
	writeJSON(w, http.StatusCreated, RegistrationPending{Userid: account.Userid, DisplayName: account.DisplayName})
}

// verifications handles POST /v1/verifications (public, rate-limited).
func (a *API) verifications(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		methodNotAllowed(w, http.MethodPost)
		return
	}
	if a.rateLimit(w, r) {
		return
	}
	if a.accounts == nil {
		writeError(w, http.StatusServiceUnavailable, Error{Code: "account_store_unavailable", Message: "account storage is unavailable"})
		return
	}
	var request verifyRegistrationRequest
	if !decodeJSON(w, r, &request) {
		return
	}
	if strings.TrimSpace(request.Code) == "" || len(request.Code) > 128 {
		writeError(w, http.StatusBadRequest, Error{Code: "invalid_code", Message: "a confirmation code is required", Field: "code"})
		return
	}
	if !validPassword(w, request.Password) {
		return
	}
	account, err := a.accounts.Verify(r.Context(), request.Code, request.Password)
	switch {
	case errors.Is(err, webaccount.ErrInvalidCode):
		writeError(w, http.StatusConflict, Error{Code: "invalid_code", Message: "the confirmation code is invalid or expired"})
		return
	case err != nil:
		a.internalError(w, r, "verify registration", err)
		return
	}
	a.issueToken(w, r, account)
}

// resendVerification handles POST /v1/verifications/resend (public,
// rate-limited). It always responds 202 to avoid disclosing account state.
func (a *API) resendVerification(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		methodNotAllowed(w, http.MethodPost)
		return
	}
	if a.rateLimit(w, r) {
		return
	}
	var request resendVerificationRequest
	if !decodeJSON(w, r, &request) {
		return
	}
	if strings.TrimSpace(request.Userid) == "" {
		writeError(w, http.StatusBadRequest, Error{Code: "invalid_userid", Message: "a userid is required", Field: "userid"})
		return
	}
	if a.accounts != nil {
		if code, email, err := a.accounts.ResendVerification(r.Context(), request.Userid); err == nil {
			a.sendVerificationEmail(r.Context(), request.Userid, email, code)
		}
	}
	w.WriteHeader(http.StatusAccepted)
}

// tokens handles POST /v1/tokens (public, rate-limited).
func (a *API) tokens(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		methodNotAllowed(w, http.MethodPost)
		return
	}
	if a.rateLimit(w, r) {
		return
	}
	if a.accounts == nil {
		writeError(w, http.StatusServiceUnavailable, Error{Code: "account_store_unavailable", Message: "account storage is unavailable"})
		return
	}
	var request createTokenRequest
	if !decodeJSON(w, r, &request) {
		return
	}
	if strings.TrimSpace(request.Userid) == "" || request.Password == "" || len(request.Password) > 128 {
		writeError(w, http.StatusUnauthorized, Error{Code: "invalid_credentials", Message: "the userid or password is incorrect"})
		return
	}
	account, err := a.accounts.Authenticate(r.Context(), request.Userid, request.Password)
	if errors.Is(err, webaccount.ErrInvalidCredentials) {
		writeError(w, http.StatusUnauthorized, Error{Code: "invalid_credentials", Message: "the userid or password is incorrect"})
		return
	}
	// A ban has to refuse the next sign-in, not only end the sessions the
	// account already had. Bumping the authorization version does the second,
	// and did it alone until this: a banned account could sign in again
	// immediately and be handed a fresh token.
	if errors.Is(err, webaccount.ErrBanned) {
		writeError(w, http.StatusForbidden, Error{Code: "account_banned",
			Message: "this account is suspended"})
		return
	}
	if err != nil {
		a.internalError(w, r, "authenticate", err)
		return
	}
	a.issueToken(w, r, account)
}

// issueToken signs a website token for the account and writes a TokenResponse
// with no-store caching.
func (a *API) issueToken(w http.ResponseWriter, r *http.Request, account webaccount.Account) {
	token, expiresAt, err := a.signer.Sign(time.Now(), account.ID, account.Userid,
		account.DisplayName, account.RezDate, account.Privileges, account.AuthVersion)
	if err != nil {
		a.internalError(w, r, "issue token", err)
		return
	}
	w.Header().Set("Cache-Control", "no-store")
	writeJSON(w, http.StatusOK, TokenResponse{
		AccessToken: token,
		TokenType:   "Bearer",
		ExpiresAt:   expiresAt,
		Identity:    identityOf(account),
	})
}

// validPassword enforces the 8-128 character password bounds, writing a 400 on
// failure.
func validPassword(w http.ResponseWriter, password string) bool {
	if len(password) < 8 || len(password) > 128 {
		writeError(w, http.StatusBadRequest, Error{Code: "invalid_password", Message: "password must be 8-128 characters", Field: "password"})
		return false
	}
	return true
}

// sendVerificationEmail delivers the confirmation code. Delivery failures are
// logged (without the recipient address) and swallowed: the account exists and
// the code can be re-requested via resend.
// passwordResets handles POST /v1/password-resets (public, rate-limited).
//
// Sends a single-use reset link to the address on file. **Every outcome answers
// 202** — unknown account, unverified account, no address, mail failure, success —
// because any difference makes this an oracle for which accounts exist
// (ADR 0034). Failures are logged rather than returned.
func (a *API) passwordResets(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		methodNotAllowed(w, http.MethodPost)
		return
	}
	if a.rateLimit(w, r) {
		return
	}
	if a.accounts == nil {
		writeError(w, http.StatusServiceUnavailable, Error{Code: "account_store_unavailable", Message: "account storage is unavailable"})
		return
	}
	var request passwordResetRequest
	if !decodeJSON(w, r, &request) {
		return
	}
	// A malformed body is the one thing worth reporting: it says nothing about
	// any account, and silently accepting it would hide a client bug.
	if strings.TrimSpace(request.Identifier) == "" || len(request.Identifier) > 254 {
		writeError(w, http.StatusBadRequest, Error{Code: "invalid_identifier", Message: "an avatar name is required", Field: "identifier"})
		return
	}
	token, email, err := a.accounts.RequestPasswordReset(r.Context(), request.Identifier)
	switch {
	case errors.Is(err, webaccount.ErrNotFound):
		// Deliberately indistinguishable from success.
		if a.logger != nil {
			a.logger.Info("password reset requested for unknown or ineligible account")
		}
	case err != nil:
		// Also indistinguishable, but this one is our fault and is logged as such.
		if a.logger != nil {
			a.logger.Error("request password reset", "error", err)
		}
	default:
		a.sendPasswordResetEmail(r.Context(), email, token)
	}
	w.WriteHeader(http.StatusAccepted)
}

// consumePasswordReset handles POST /v1/password-resets/{token} (public,
// rate-limited).
//
// A password change with the token standing in for knowing the current password;
// after that proof it writes the digests and nothing else, leaving sessions alone
// (ADR 0034).
func (a *API) consumePasswordReset(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		methodNotAllowed(w, http.MethodPost)
		return
	}
	if a.rateLimit(w, r) {
		return
	}
	if a.accounts == nil {
		writeError(w, http.StatusServiceUnavailable, Error{Code: "account_store_unavailable", Message: "account storage is unavailable"})
		return
	}
	token := strings.TrimPrefix(r.URL.Path, "/v1/password-resets/")
	if token == "" || strings.Contains(token, "/") {
		writeError(w, http.StatusBadRequest, Error{Code: "invalid_token", Message: "a reset token is required"})
		return
	}
	var request passwordResetConsumeRequest
	if !decodeJSON(w, r, &request) {
		return
	}
	if !validPassword(w, request.Password) {
		return
	}
	err := a.accounts.ConsumePasswordReset(r.Context(), token, request.Password)
	switch {
	case errors.Is(err, webaccount.ErrInvalidCode):
		// One reply for unknown, used and expired: telling them apart would say
		// whether a token had ever existed.
		writeError(w, http.StatusBadRequest, Error{Code: "invalid_token", Message: "the reset link is invalid or has expired"})
		return
	case err != nil:
		a.internalError(w, r, "consume password reset", err)
		return
	}
	w.WriteHeader(http.StatusNoContent)
}

// sendPasswordResetEmail mails the reset link. The token is only ever sent to the
// address the store returned, never to one supplied by the caller.
func (a *API) sendPasswordResetEmail(ctx context.Context, email, token string) {
	var body strings.Builder
	body.WriteString("A password reset was requested for your Homeworldz account.\n\n")
	if a.resetURL != "" {
		body.WriteString("Open this link to choose a new password:\n")
		body.WriteString(a.resetURL + "/" + url.PathEscape(token) + "\n\n")
	} else {
		body.WriteString("Your reset token is:\n\n    " + token + "\n\n")
	}
	body.WriteString("The link expires in one hour and can be used once.\n")
	body.WriteString("If you did not request it, ignore this email — nothing has changed,\n")
	body.WriteString("and your existing sessions are unaffected.\n")
	if err := a.mailer.Send(ctx, mailer.Message{
		To:      email,
		Subject: "Reset your Homeworldz password",
		Body:    body.String(),
	}); err != nil && a.logger != nil {
		a.logger.Error("send password reset email", "error", err)
	}
}

func (a *API) sendVerificationEmail(ctx context.Context, userid, email, code string) {
	var body strings.Builder
	body.WriteString("Welcome to Homeworldz.\n\n")
	body.WriteString("Your confirmation code for avatar \"" + userid + "\" is:\n\n")
	body.WriteString("    " + code + "\n\n")
	if a.verificationURL != "" {
		body.WriteString("Or open this link to finish setting up your account:\n")
		body.WriteString(a.verificationURL + "?code=" + url.QueryEscape(code) + "\n\n")
	}
	body.WriteString("This code expires in 24 hours. If you did not request it, ignore this email.\n")
	if err := a.mailer.Send(ctx, mailer.Message{
		To:      email,
		Subject: "Confirm your Homeworldz avatar",
		Body:    body.String(),
	}); err != nil && a.logger != nil {
		a.logger.Error("send verification email", "userid", userid, "error", err)
	}
}

// internalError logs the underlying error and writes a generic 500.
func (a *API) internalError(w http.ResponseWriter, r *http.Request, operation string, err error) {
	if a.logger != nil {
		a.logger.Error("website api error",
			"requestId", requestIDFromContext(r.Context()), "operation", operation, "error", err)
	}
	writeError(w, http.StatusInternalServerError, Error{Code: "internal_error", Message: "an internal error occurred"})
}
