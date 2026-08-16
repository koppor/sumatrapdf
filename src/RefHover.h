/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

class EngineBase;
struct DocController;
struct DisplayModel;
struct ILinkHandler;
struct IPageDestination;
struct IPageElement;
struct RenderedBitmap;
struct Pixmap;
struct RefLookupCache;

// Status badge rendered in the corner of the citation hover popup. Hue + shape
// cues form a consistent palette (mint = positive, olive = partial, grey =
// neutral, orange = error). Shape distinguishes "in flight" (small filled
// square) from "no match" (hollow ring) — same hue, different stage.
enum class RefHoverPushStatus {
    None,         // no badge (popup is hiding a non-bib or hasn't been checked yet)
    Searching,    // small grey square (#BBBBBB) — hover-time existence check in flight
    NoMatch,      // hollow grey ring (#BBBBBB) — lookup completed, not in any open library
    Sending,      // small grey square (#BBBBBB) — Ctrl+J push in flight
    Sent,         // mint filled circle (#44BB99) — entry in *active* library (Ctrl+J would duplicate)
    RelatedMatch, // olive filled circle (#AAAA00) — entry in another open library, not the active one
    Failed,       // orange filled circle (#EE7733) — push HTTP error (non-2xx response)
    Disconnected, // orange filled circle (#EE7733) — JabRef HTTP server unreachable
};

struct RefHoverState {
    HWND hwndPopup = nullptr;
    HWND hwndCanvas = nullptr;
    // kept current by Canvas on mouse-move so popup clicks can open links
    DocController* ctrl = nullptr;
    ILinkHandler* linkHandler = nullptr;
    // currently shown rendered destination strip (owned)
    Pixmap* bmp = nullptr;
    // engine for the currently displayed page, AddRef'd while shown so the
    // popup can hit-test links under the cursor (hand cursor + click-to-open)
    EngineBase* hitEngine = nullptr;

    // cache of plain-text citation lookups (lazy-init on first use)
    RefLookupCache* lookupCache = nullptr;

    // Pending hover request: set by RefHoverSchedule, consumed by
    // RefHoverOnTimer when the hover-delay timer fires.
    struct Pending {
        Point screenPt;
        int destPage = -1;
        float destX = -1.f;
        float destY = -1.f;
        // /XYZ zoom hint from the link (1.0 = 100%). 0 means "no zoom hint";
        // RefHover then falls back to its auto-fit DetectEntryBox heuristic.
        // When non-zero, the popup renders the destination region centred on
        // (destX, destY) at this zoom — honouring the link author's intent
        // (e.g. "goto top-left at 2x").
        float destZoom = 0.f;
        // Source link location, used to recover a more specific destY when
        // the PDF link is page-level (destY < 0). We extract the source
        // link's text from srcPage at srcRect and search for that text on
        // destPage to find the matching entry's Y. Without this, page-level
        // abbreviation / glossary links render the whole abbreviations page
        // from top.
        int srcPage = -1;
        RectF srcRect;
        // Screen rect of the source page (visible portion). Used to clamp
        // the popup so it stays within the document area and doesn't drift
        // into the gray margins outside the page.
        Rect pageScreenRect;
    } pending;

    // Async rendering: renders run on a background thread (a complex page
    // would otherwise stall the UI for the duration of the render) and the
    // bitmap is delivered back on the UI thread via uitask.
    struct RenderRequest {
        bool valid = false;
        // matched against renderGen on completion, stale results are dropped
        int gen = 0;
        EngineBase* engine = nullptr; // AddRef()'ed for the render duration
        int pageNo = -1;
        float zoom = 0.f;
        RectF region;
        // Second crop stitched below `region` in the delivered bitmap, for a
        // bracket-style entry that wraps across a 2-column page break (see
        // DetectEntryBox's continuationOut). Empty (dx/dy <= 0) when there's
        // none. Only ever set by the initial DetectEntryBox-driven show —
        // wheel-zoom/scroll re-renders build a fresh request from just
        // displayed.region and never populate this, so the stitched strip is
        // dropped as soon as the user interacts (region-shift math for a
        // composited bitmap isn't supported).
        RectF continuationRegion;
        // initial show: commit displayed.* and show the popup on completion.
        // false for wheel zoom / scroll re-renders, which update displayed.*
        // optimistically and only need the new bitmap.
        bool showPopup = false;
        Point screenPt;
        float destXRaw = -1.f;
        float destYRaw = -1.f;
        // source link location (page coords) that triggered this show; carried
        // through so RefHoverSchedule can tell two occurrences of the same
        // reference apart and reposition the popup to the new one
        int srcPageRaw = -1;
        RectF srcRectRaw;
    };
    // bumped on every new request and on hide, invalidating older results
    int renderGen = 0;
    bool renderInFlight = false;
    // the latest request that arrived while another was in flight; started
    // when that one completes (coalesces wheel-scroll / zoom streams)
    RenderRequest queuedRender;

    // Currently-displayed bitmap context. Compared against incoming hover
    // requests to skip a re-render when the destination hasn't changed, and
    // re-used by the wheel-zoom / wheel-scroll handlers so they can re-render
    // without re-running detection.
    struct Displayed {
        int destPage = -1;
        float destX = -1.f;
        float destY = -1.f;
        // Region of the page rendered into the popup bitmap, kept so the
        // wheel handlers can shift / scale it without re-running detection.
        RectF region;
        // baseZoom matches the document's current page zoom on first show
        // so popup text height is comparable to page text. userZoom is the
        // multiplier driven by the user's mouse-wheel.
        float baseZoom = 1.f;
        float userZoom = 1.f;
        // source link that produced this popup (page coords). Compared in
        // RefHoverSchedule so hovering a different occurrence of the same
        // reference re-positions the popup instead of skipping as a no-op.
        int srcPage = -1;
        RectF srcRect;
    } displayed;

    // UTF-8 text of the bibliography entry currently shown in the popup, or
    // null when the popup shows a non-bibliography target (figure, table,
    // landscape view). Set during RefHoverOnTimer; consumed by
    // RefHoverPushToJabRef when the user presses Ctrl+J.
    char* entryText = nullptr;

    // Server-issued cache token returned by the hover-time existence check.
    // When set, Ctrl+J uses it to add the citation without paying for the
    // (expensive) LLM parse a second time. Null until the existence check
    // completes; reset whenever entryText is replaced.
    char* parserCacheKey = nullptr;

    // FNV-1a 64-bit hashes of entryText for citations the user has already
    // pushed in this session. Used to light the mint badge immediately on
    // hover without waiting on a server-side lookup that may flake (LLM
    // quota exhausted, JabRef restarted, etc.). Bounded by the natural pace
    // of manual citation imports — no eviction needed.
    Vec<u64> pushedEntryHashes;

    // Push-status badge for the currently-shown entry. Reset to None whenever
    // the popup transitions to a new entry; advanced by RefHoverSetPushStatus
    // when the user fires Ctrl+J.
    RefHoverPushStatus pushStatus = RefHoverPushStatus::None;
    // Bumped on every transition to a new entry. Async existence-check
    // workers snapshot the value at dispatch time and discard their result
    // when the version no longer matches — so a slow check for a previously-
    // hovered entry can't paint a stale badge over the current one.
    int entryVersion = 0;
};

constexpr UINT_PTR kRefHoverTimerID = 9;
constexpr UINT_PTR kRefHoverHideTimerID = 10;

RefHoverState* RefHoverCreate(HWND hwndCanvas);
void RefHoverDestroy(RefHoverState* s);
bool RefHoverIsInternalLink(IPageElement* el, DisplayModel* dm);
void RefHoverOnCanvasMouseMove(RefHoverState*& s, HWND hwndCanvas, DocController* ctrl, ILinkHandler* linkHandler,
                               DisplayModel* dm, int x, int y, IPageElement* el, int srcPageNo, int hoverDelayMs);
void RefHoverOnCanvasMouseLeave(RefHoverState* s, HWND hwndCanvas, int hoverDelayMs);
void RefHoverOnCanvasLeftButtonDown(RefHoverState* s, HWND hwndCanvas);
bool RefHoverOnCanvasTimer(RefHoverState* s, HWND hwndCanvas, DisplayModel* dm, UINT_PTR timerId);
void RefHoverSchedule(RefHoverState* s, HWND hwndCanvas, int delayMs, Point screenPt, int destPage, float destX,
                      float destY, float destZoom, int srcPage, RectF srcRect, Rect pageScreenRect);
void RefHoverHide(RefHoverState* s, HWND hwndCanvas);
void RefHoverScheduleHide(RefHoverState* s, HWND hwndCanvas, int delayMs);
void RefHoverOnHideTimer(RefHoverState* s, HWND hwndCanvas);
void RefHoverHandlePopupClick(RefHoverState* s, IPageDestination* dest);
void RefHoverOnTimer(RefHoverState* s, HWND hwndCanvas, EngineBase* engine, float pageZoom);
bool RefHoverWheelZoom(RefHoverState* s, EngineBase* engine, int wheelDelta);
bool RefHoverWheelScroll(RefHoverState* s, EngineBase* engine, int wheelDelta);

// Result of an attempt to push the hovered reference to JabRef.
enum class RefHoverPushResult {
    Failed, // JabRef could not be reached / returned a non-2xx status
    Sent,   // the entry was posted to JabRef's local HTTP server
};

// Set the push-status badge and invalidate the popup so the badge repaints.
// Called by SumatraPDF.cpp around the async push: Sending before RunAsync,
// Sent / Failed when the UI task fires.
void RefHoverSetPushStatus(RefHoverState* s, RefHoverPushStatus status);
// Returns a heap-allocated UTF-8 copy of the bibliography entry currently
// shown in the popup, or nullptr when the popup isn't visible / isn't showing
// a pushable bibliography entry. Caller frees with str::Free. Snapshotting
// lets the HTTP POST run on a background thread without racing with the
// hover-detection thread that mutates RefHoverState.
char* RefHoverDupEntryTextIfPushable(RefHoverState* s);
// Returns a heap-allocated UTF-8 copy of the parser-cache token issued by the
// last hover-time existence check, or nullptr when none is available. Caller
// frees with str::Free. Used by Ctrl+J so the add request reuses the cached
// BibEntry (no LLM call) instead of re-parsing entryText from scratch.
char* RefHoverDupParserCacheKey(RefHoverState* s);

// Records `entryText` as already-pushed in this RefHoverState. Called by the
// Ctrl+J UI task after a successful POST. Subsequent hovers of any citation
// whose extracted entryText matches will light the mint badge from the local
// record alone (no server roundtrip).
void RefHoverMarkPushed(RefHoverState* s, const char* entryText);

// Returns true when `entryText` has been recorded by RefHoverMarkPushed in
// this session.
bool RefHoverWasPushed(RefHoverState* s, const char* entryText);

// FNV-1a 64-bit hash of `entryText` (UTF-8). Exposed so callers outside
// RefHover (in-PDF marker scan, page-paint) can derive the same identity
// key RefHoverMarkPushed uses, without duplicating the hash implementation.
u64 RefHoverHashEntryText(const char* entryText);

// One in-text citation the user has pushed (or that the server reports is
// already in the active library). Lives on WindowTab so the marker dot
// survives popup hide / page scroll / tab switch within a session.
//
// `srcPage` + `srcRect` locate the in-text reference on the source PDF page
// (the `[Kar+24b]` / `(Smith 2020)` link rectangle) — used to position the
// dot during page paint. `entryTextHash` is the FNV-1a hash of the resolved
// bibliography entry text; lets a second occurrence of the same citation
// elsewhere in the doc be marked too when the page-scan resolves it.
struct PushedCitation {
    int srcPage = -1;
    RectF srcRect{};
    u64 entryTextHash = 0;
    // Match scope reported by JabRef: "active" = in the library Ctrl+J would
    // add to (paint mint), "other" = in some other open library (paint olive).
    // Stored as the same enum used by the popup badge so the paint loop can
    // share colour logic with RefHover.cpp.
    RefHoverPushStatus status = RefHoverPushStatus::Sent;
};
// Synchronously POSTs to JabRef's local HTTP server. Tries the cached-parse
// fast path first (POST /libraries/current/citations/{parserCacheKey}) when
// parserCacheKey is non-null; falls back to the plain-citation endpoint
// (POST /libraries/current/entries) when the cache key has expired (HTTP 410)
// or wasn't issued (parserCacheKey nullptr). Blocks the calling thread for up
// to ~15s, so call from a worker thread.
//
// outHttpStatus (optional): raw HTTP status code from the final attempt that
// determined success / failure. 0 indicates a TCP / WinINet error.
// outRespBody (optional): heap-allocated copy of the response body from the
// final attempt (JabRef returns its exception message on 5xx). Caller frees.
RefHoverPushResult RefHoverPushToJabRef(const char* entryText, const char* parserCacheKey, int* outHttpStatus = nullptr,
                                        char** outRespBody = nullptr);
