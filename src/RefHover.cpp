/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// Citation / reference hover — lifecycle and scheduling. Popup UI, async
// render, region detection, canvas wiring, and plain-text lookup live in
// sibling RefHover*.cpp files.

#include "base/Base.h"
#include "base/Http.h"
#include "base/Pixmap.h"
#include "base/UITask.h"
#include "base/Win.h"

#include "gui/UIModels.h"

#include "DocController.h"
#include "EngineBase.h"
#include "RefHoverInternal.h"
#include "RefHoverText.h"

RefHoverState* RefHoverCreate(HWND hwndCanvas) {
    auto* s = new RefHoverState();
    if (!RefHoverPopupCreate(s, hwndCanvas)) {
        delete s;
        return nullptr;
    }
    RefHoverRegisterLiveState(s);
    return s;
}

void RefHoverDestroy(RefHoverState* s) {
    if (!s) {
        return;
    }
    RefHoverUnregisterLiveState(s);
    RefHoverDropQueuedRender(s);
    if (s->hwndPopup) {
        DestroyWindow(s->hwndPopup);
        s->hwndPopup = nullptr;
    }
    FreePixmap(s->bmp);
    s->bmp = nullptr;
    if (s->hitEngine) {
        s->hitEngine->Release();
        s->hitEngine = nullptr;
    }
    str::Free(Str(s->entryText));
    s->entryText = nullptr;
    str::Free(Str(s->parserCacheKey));
    s->parserCacheKey = nullptr;
    RefHoverFreeLookupCache(s);
    delete s;
}

// delayMs: how long the cursor must hover before the popup shows
// (the CitationHoverDelay advanced setting)
void RefHoverSchedule(RefHoverState* s, HWND hwndCanvas, int delayMs, Point screenPt, int destPage, float destX,
                      float destY, float destZoom, int srcPage, RectF srcRect, Rect pageScreenRect) {
    if (!s || delayMs < 0) {
        return;
    }
    KillTimer(hwndCanvas, kRefHoverTimerID);
    KillTimer(hwndCanvas, kRefHoverHideTimerID);

    bool sameSrc = s->displayed.srcPage == srcPage && s->displayed.srcRect == srcRect;
    if (HwndIsVisible(s->hwndPopup) && s->displayed.destPage == destPage && s->displayed.destX == destX &&
        s->displayed.destY == destY && sameSrc) {
        return;
    }
    s->pending.screenPt = screenPt;
    s->pending.destPage = destPage;
    s->pending.destX = destX;
    s->pending.destY = destY;
    s->pending.destZoom = destZoom;
    s->pending.srcPage = srcPage;
    s->pending.srcRect = srcRect;
    s->pending.pageScreenRect = pageScreenRect;
    if (HwndIsVisible(s->hwndPopup)) {
        delayMs = 0;
    }
    SetTimer(hwndCanvas, kRefHoverTimerID, (UINT)delayMs, nullptr);
}

void RefHoverHide(RefHoverState* s, HWND hwndCanvas) {
    if (!s) {
        return;
    }
    KillTimer(hwndCanvas, kRefHoverTimerID);
    KillTimer(hwndCanvas, kRefHoverHideTimerID);
    s->pending.destPage = -1;
    s->renderGen++;
    RefHoverDropQueuedRender(s);
    if (s->hwndPopup && HwndIsVisible(s->hwndPopup)) {
        ShowWindow(s->hwndPopup, SW_HIDE);
        s->displayed.destPage = -1;
    }
}

static constexpr UINT kRefHoverHidePollMs = 150;
static constexpr int kRefHoverHideMinMs = 250;

// Like RefHoverHide but deferred: cancels any pending show immediately, then
// hides the visible popup after delayMs. While the timer is pending, moving
// the cursor onto the popup (e.g. to click a DOI link inside it) keeps it
// alive. Lets the cursor cross the gap between the link and the popup without
// the popup vanishing. Cancelled by a new RefHoverSchedule / RefHoverHide.
void RefHoverScheduleHide(RefHoverState* s, HWND hwndCanvas, int delayMs) {
    if (!s) {
        return;
    }
    KillTimer(hwndCanvas, kRefHoverTimerID);
    s->pending.destPage = -1;
    s->renderGen++;
    RefHoverDropQueuedRender(s);
    if (!s->hwndPopup || !HwndIsVisible(s->hwndPopup)) {
        s->displayed.destPage = -1;
        return;
    }
    delayMs = std::max(delayMs, kRefHoverHideMinMs);
    SetTimer(hwndCanvas, kRefHoverHideTimerID, (UINT)delayMs, nullptr);
}

// Fired by kRefHoverHideTimerID: hides the popup unless the cursor is now
// over it (in which case it re-arms and keeps the popup up).
void RefHoverOnHideTimer(RefHoverState* s, HWND hwndCanvas) {
    if (!s) {
        return;
    }
    KillTimer(hwndCanvas, kRefHoverHideTimerID);
    if (!s->hwndPopup || !HwndIsVisible(s->hwndPopup)) {
        return;
    }
    POINT pt;
    if (GetCursorPos(&pt)) {
        if (HwndWindowFromPoint(Point(pt.x, pt.y)) == s->hwndPopup) {
            SetTimer(hwndCanvas, kRefHoverHideTimerID, kRefHoverHidePollMs, nullptr);
            return;
        }
    }
    ShowWindow(s->hwndPopup, SW_HIDE);
    s->displayed.destPage = -1;
}

// Open a launch link (external URL / file) hit-tested inside the popup.
void RefHoverHandlePopupClick(RefHoverState* s, IPageDestination* dest) {
    if (!s || !dest || !s->ctrl) {
        return;
    }
    RefHoverHide(s, s->hwndCanvas);
    s->ctrl->HandleLink(dest, s->linkHandler);
}

// ---- JabRef push --------------------------------------------------------
//
// Ctrl+J posts the hovered bibliography entry to the local JabRef HTTP server.
// A hover-time existence check lights the popup-corner badge (RefHoverPopup.cpp
// paints it) so the user sees at a glance which references they already have.

// Collect the UTF-8 text of every glyph whose center falls inside `region` on
// `destPage`, in text-array (reading) order. A separating space is inserted
// between glyphs that sit on different lines so words from wrapped lines don't
// run together, and runs of whitespace are collapsed. Returns a heap string
// (caller frees with str::Free) or null when the region holds no text.
static char* ExtractEntryText(EngineBase* engine, int destPage, RectF region) {
    int textLen = 0;
    Rect* coords = nullptr;
    Str textUtf8 = engine->GetTextForPage(destPage, &textLen, &coords);
    TempWStr wtext = RefHoverPageTextToWStrTemp(textUtf8);
    const WCHAR* text = wtext.s;
    if (!text || textLen <= 0 || !coords) {
        return nullptr;
    }
    int x0 = (int)region.x;
    int y0 = (int)region.y;
    int x1 = (int)(region.x + region.dx);
    int y1 = (int)(region.y + region.dy);
    wstr::Builder out;
    int prevY = INT_MIN;
    for (int i = 0; i < textLen; i++) {
        Rect r = coords[i];
        int cx = r.x + r.dx / 2;
        int cy = r.y + r.dy / 2;
        if (cx < x0 || cx > x1 || cy < y0 || cy > y1) {
            continue;
        }
        WCHAR c = text[i];
        bool isSpace = (c == L' ' || c == L'\t' || c == L'\n' || c == L'\r');
        WCHAR last = (out.len > 0) ? out.LastChar() : 0;
        // A line change inside the region separates words from wrapped lines.
        if (!isSpace && prevY != INT_MIN && r.y > prevY + 3 && last != 0 && last != L' ') {
            out.Append(L" ");
        }
        if (isSpace) {
            if (last != 0 && last != L' ') {
                out.Append(L" ");
            }
            continue;
        }
        prevY = r.y;
        WCHAR buf[2] = {c, 0};
        out.Append(buf);
    }
    while (out.len > 0 && out.LastChar() == L' ') {
        out.RemoveLast();
    }
    if (out.len == 0) {
        return nullptr;
    }
    return str::Dup(ToUtf8Temp(ToWStr(out))).s;
}

// FNV-1a 64-bit. Fast, zero-dep, more than enough collision resistance for a
// short list of citation strings. Hashes byte-wise so it's UTF-8 safe.
u64 RefHoverHashEntryText(const char* s) {
    if (!s) {
        return 0;
    }
    constexpr u64 kFnvOffset = 14695981039346656037ULL;
    constexpr u64 kFnvPrime = 1099511628211ULL;
    u64 h = kFnvOffset;
    for (const unsigned char* p = (const unsigned char*)s; *p; p++) {
        h ^= (u64)*p;
        h *= kFnvPrime;
    }
    return h;
}

void RefHoverMarkPushed(RefHoverState* s, const char* entryText) {
    if (!s || !entryText || !*entryText) {
        return;
    }
    u64 h = RefHoverHashEntryText(entryText);
    // Don't double-record — keeps the vec from growing on repeated re-pushes
    // of the same reference (rare but cheap to guard).
    for (int i = 0; i < s->pushedEntryHashes.len; i++) {
        if (s->pushedEntryHashes[i] == h) {
            return;
        }
    }
    s->pushedEntryHashes.Append(h);
}

bool RefHoverWasPushed(RefHoverState* s, const char* entryText) {
    if (!s || !entryText || !*entryText) {
        return false;
    }
    u64 h = RefHoverHashEntryText(entryText);
    for (int i = 0; i < s->pushedEntryHashes.len; i++) {
        if (s->pushedEntryHashes[i] == h) {
            return true;
        }
    }
    return false;
}

// Crude scan for `"matches": []` (empty) vs `"matches": [{` (one or more
// entries). Full JSON parsing is overkill for one boolean and keeps this TU
// independent of JsonParser.
static bool ResponseHasMatch(const char* json) {
    if (!json) {
        return false;
    }
    const char* p = strstr(json, "\"matches\"");
    if (!p) {
        return false;
    }
    p += 9;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ':') {
        p++;
    }
    if (*p != '[') {
        return false;
    }
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
        p++;
    }
    return *p == '{';
}

// Extract a top-level JSON string field, e.g. `"parserCacheKey":"abc123"` →
// `abc123`. Caller frees with str::Free. Returns nullptr when not present or
// the value contains an escape sequence (the cache tokens are CUID2 strings
// so they never do — keeping this honest about its limits).
static char* ExtractJsonString(const char* json, const char* key) {
    if (!json || !key) {
        return nullptr;
    }
    str::Builder needle;
    needle.Append("\"");
    needle.Append(key);
    needle.Append("\"");
    const char* p = strstr(json, ToCStr(needle));
    if (!p) {
        return nullptr;
    }
    p += strlen(key) + 2; // past closing "
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ':') {
        p++;
    }
    if (*p != '"') {
        return nullptr;
    }
    p++;
    const char* end = p;
    while (*end && *end != '"' && *end != '\\') {
        end++;
    }
    if (*end != '"') {
        return nullptr;
    }
    return str::Dup(Str(p, (int)(end - p))).s;
}

struct ExistenceCheckAsyncData {
    RefHoverState* s;
    int version;
    char* entryText; // owned
};

struct ExistenceCheckUIData {
    RefHoverState* s;
    int version;
    bool ok;              // HTTP request itself succeeded (server reachable + 2xx)
    bool exists;          // 2xx + matches array non-empty
    int httpStatus;       // 0 = TCP/WinINet failure; 4xx/5xx = server reached but errored
    char* parserCacheKey; // owned; null if not returned
    char* matchScope;     // owned: "active" / "other" / "none" (or null on parse miss)
    char* entryText;      // owned; needed to record the positive lookup in the local set
};

static void ExistenceCheckUI(ExistenceCheckUIData* t) {
    // Stale result: a newer entry replaced ours before the check came back.
    // Silently drop so the new entry's badge / cache key isn't overwritten.
    if (t->s && t->version == t->s->entryVersion) {
        if (t->parserCacheKey) {
            str::Free(Str(t->s->parserCacheKey));
            t->s->parserCacheKey = t->parserCacheKey;
            t->parserCacheKey = nullptr; // ownership transferred to RefHoverState
        }
        bool isActiveLibMatch = t->matchScope && str::Eq(t->matchScope, "active");
        if (isActiveLibMatch) {
            // Record the positive answer (active library) so a subsequent
            // flake (server 500, LLM quota, network glitch) can't flip the
            // badge back to NoMatch on the next hover of the same citation.
            // Other-library matches are *not* cached — Ctrl+J would still
            // add to the active library, so the user needs to keep seeing
            // the olive cue.
            RefHoverMarkPushed(t->s, t->entryText);
        }
        // Local set already promoted the badge to Sent in RefHoverOnTimer
        // for previously-confirmed citations — don't downgrade that.
        if (t->s->pushStatus != RefHoverPushStatus::Sent) {
            if (isActiveLibMatch) {
                RefHoverSetPushStatus(t->s, RefHoverPushStatus::Sent);
            } else if (t->exists) {
                // Match in some other open library — olive (related).
                RefHoverSetPushStatus(t->s, RefHoverPushStatus::RelatedMatch);
            } else if (t->ok) {
                // Server replied cleanly with empty matches → genuine no-match.
                RefHoverSetPushStatus(t->s, RefHoverPushStatus::NoMatch);
            } else if (t->httpStatus == 0) {
                // HTTP status 0 = TCP / WinINet error — no response received.
                // JabRef is not running (or server disabled) → orange
                // disconnect badge.
                RefHoverSetPushStatus(t->s, RefHoverPushStatus::Disconnected);
            }
            // Server reachable but errored (4xx/5xx) keeps the Searching
            // square up rather than claiming a NoMatch we don't actually
            // know. The next hover re-issues the lookup.
        }
    }
    str::Free(Str(t->parserCacheKey));
    str::Free(Str(t->matchScope));
    str::Free(Str(t->entryText));
    delete t;
}

static void ExistenceCheckAsync(ExistenceCheckAsyncData* d) {
    str::Builder headers;
    headers.Append("Content-Type: text/plain; charset=utf-8\r\n");
    str::Builder body;
    body.Append(d->entryText);
    str::Builder resp;
    DWORD status = 0;
    // Lookup endpoint parses + duplicate-checks + caches in one round trip.
    // Result is JSON: {matches:[…], parserCacheKey:"…", parsedEntryType:"…"}.
    bool ok = HttpPost("127.0.0.1", 23119, "/libraries/current/citations/lookup", &headers, &body, &resp, &status);
    char* respStr = ToCStr(resp);
    bool exists = ok && ResponseHasMatch(respStr);
    char* parserCacheKey = ok ? ExtractJsonString(respStr, "parserCacheKey") : nullptr;
    char* matchScope = ok ? ExtractJsonString(respStr, "matchScope") : nullptr;
    // entryText ownership transfers to UI task for the local-set update.
    auto* t =
        new ExistenceCheckUIData{d->s, d->version, ok, exists, (int)status, parserCacheKey, matchScope, d->entryText};
    delete d;
    auto fn = MkFunc0<ExistenceCheckUIData>(ExistenceCheckUI, t);
    uitask::Post(fn, "RefHoverExistenceCheck");
}

static void KickoffExistenceCheck(RefHoverState* s) {
    if (!s || !s->entryText) {
        return;
    }
    auto* d = new ExistenceCheckAsyncData{s, s->entryVersion, str::Dup(Str(s->entryText)).s};
    auto fn = MkFunc0<ExistenceCheckAsyncData>(ExistenceCheckAsync, d);
    RunAsync(fn, "RefHoverExistenceCheck");
}

void RefHoverCaptureEntryAndCheck(RefHoverState* s, EngineBase* engine, int destPage, RectF region, bool isBibEntry) {
    if (!s) {
        return;
    }
    // Push-status badge tracks the currently-displayed entry, so reset on
    // every popup transition. Bumping the version token drops any in-flight
    // existence / push UI task aimed at the previous entry.
    s->pushStatus = RefHoverPushStatus::None;
    s->entryVersion++;
    // Only bibliography entries are pushable; figures / tables / landscape
    // views aren't. We only *replace* entryText when the new entry is itself
    // a bib — non-bib hovers leave the previously-captured bib alive so
    // Ctrl+J retry still works after the popup has cycled through unrelated
    // hovers (notification reading, mouse drift over TOC / figure links).
    if (isBibEntry) {
        str::Free(Str(s->entryText));
        s->entryText = ExtractEntryText(engine, destPage, region);
        // New entryText invalidates the parser-cache token issued for the
        // previous one — wait for the fresh lookup to refill it.
        str::Free(Str(s->parserCacheKey));
        s->parserCacheKey = nullptr;
        // Local already-pushed check: if the user pushed this same entry
        // earlier in the session, light the badge immediately. The async
        // server lookup still fires (it refills parserCacheKey for a future
        // re-push) but the badge no longer depends on the server agreeing
        // — which it might not when the LLM parser is rate-limited.
        if (s->entryText && RefHoverWasPushed(s, s->entryText)) {
            s->pushStatus = RefHoverPushStatus::Sent;
        }
    }

    // Hover-time existence check: ask JabRef whether this entry's title is
    // already in the current library. On match the badge lights up mint, so
    // the user sees at a glance which references they have / don't have.
    // Only fired when the *current* hover is a bib — otherwise the badge
    // would mark a figure / TOC popup with the prior bib's existence state.
    if (isBibEntry && s->entryText) {
        // Show the in-flight searching square only when we don't already
        // have a definitive answer from the local pushed-set (Sent set in
        // the RefHoverWasPushed branch above).
        if (s->pushStatus != RefHoverPushStatus::Sent) {
            s->pushStatus = RefHoverPushStatus::Searching;
        }
        KickoffExistenceCheck(s);
    }
}

void RefHoverSetPushStatus(RefHoverState* s, RefHoverPushStatus status) {
    if (!s) {
        return;
    }
    s->pushStatus = status;
    if (s->hwndPopup && HwndIsVisible(s->hwndPopup)) {
        InvalidateRect(s->hwndPopup, nullptr, FALSE);
    }
}

char* RefHoverDupEntryTextIfPushable(RefHoverState* s) {
    // Popup visibility is intentionally not checked: the last-hovered entry
    // stays pushable after the popup hides so Ctrl+J still works while the
    // user is reading the result notification.
    if (!s || !s->entryText) {
        return nullptr;
    }
    return str::Dup(Str(s->entryText)).s;
}

char* RefHoverDupParserCacheKey(RefHoverState* s) {
    if (!s || !s->parserCacheKey) {
        return nullptr;
    }
    return str::Dup(Str(s->parserCacheKey)).s;
}

RefHoverPushResult RefHoverPushToJabRef(const char* entryText, const char* parserCacheKey, int* outHttpStatus,
                                        char** outRespBody) {
    if (outHttpStatus) {
        *outHttpStatus = 0;
    }
    if (outRespBody) {
        *outRespBody = nullptr;
    }
    if (!entryText && !parserCacheKey) {
        return RefHoverPushResult::Failed;
    }

    // Fast path: server already has the parsed BibEntry cached from the
    // hover-time lookup. Add request is a no-body POST against the token URL.
    // 410 Gone means the token expired (TTL or eviction) — fall through to
    // the slow path. Any other status (201, 5xx, 0) is the final answer.
    if (parserCacheKey && *parserCacheKey) {
        str::Builder cacheHeaders;
        cacheHeaders.Append("Content-Length: 0\r\n");
        str::Builder emptyBody;
        str::Builder cacheResp;
        DWORD cacheStatus = 0;
        str::Builder url;
        url.Append("/libraries/current/citations/");
        url.Append(parserCacheKey);
        bool cacheOk = HttpPost("127.0.0.1", 23119, ToStr(url), &cacheHeaders, &emptyBody, &cacheResp, &cacheStatus);
        if (cacheOk || cacheStatus != 410) {
            if (outHttpStatus) {
                *outHttpStatus = (int)cacheStatus;
            }
            if (outRespBody && len(cacheResp) > 0) {
                *outRespBody = str::Dup(ToStr(cacheResp)).s;
            }
            return cacheOk ? RefHoverPushResult::Sent : RefHoverPushResult::Failed;
        }
        // 410 Gone — token expired. Fall through to the plain-text endpoint
        // so the user's Ctrl+J still succeeds (at the cost of one more LLM
        // call) instead of failing on a stale cache token.
    }

    // Slow path: re-parse on the server. Required when no cache token exists
    // (lookup hadn't run, or hadn't returned yet) or the token expired.
    if (!entryText) {
        return RefHoverPushResult::Failed;
    }
    str::Builder headers;
    headers.Append("Content-Type: text/plain; charset=utf-8\r\n");
    str::Builder data;
    data.Append(entryText);
    DWORD status = 0;
    str::Builder resp;
    bool ok = HttpPost("127.0.0.1", 23119, "/libraries/current/entries", &headers, &data, &resp, &status);
    if (outHttpStatus) {
        *outHttpStatus = (int)status;
    }
    if (outRespBody && len(resp) > 0) {
        *outRespBody = str::Dup(ToStr(resp)).s;
    }
    return ok ? RefHoverPushResult::Sent : RefHoverPushResult::Failed;
}
