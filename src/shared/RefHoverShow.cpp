/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"
#include "gui/Dpi.h"

#include "gui/UIModels.h"

#include "DocController.h"
#include "EngineBase.h"
#include "RefHover.h"

enum class MissingEntry {
    Empty,
    Landscape,
};

// the page's text with watermark glyphs dropped and glyphs snapped to lines
TempWStr RefHoverPageTextTemp(EngineBase* engine, int pageNo, Rect** coordsOut) {
    Rect* coords = nullptr;
    int textLen = 0;
    Str textUtf8 = engine->GetTextForPage(pageNo, &textLen, &coords);
    TempWStr text = RefHoverPageTextToWStrTemp(textUtf8);
    *coordsOut = coords;
    if (!coords || len(text) == 0) {
        return text;
    }
    WCHAR* cleanText = AllocArrayTemp<WCHAR>(textLen);
    Rect* cleanCoords = AllocArrayTemp<Rect>(textLen);
    int cleanLen = StripWatermarkGlyphs(text, coords, cleanText, cleanCoords);
    Rect* normCoords = AllocArrayTemp<Rect>(cleanLen);
    NormalizeGlyphLines(cleanCoords, normCoords, cleanLen);
    *coordsOut = normCoords;
    return WStr(cleanText, cleanLen);
}

static RectF DetectNoteRegion(EngineBase* engine, int pageNo, RectF mediabox, float destY) {
    AutoArenaSavepoint tempScope;
    Rect* coords = nullptr;
    TempWStr text = RefHoverPageTextTemp(engine, pageNo, &coords);
    return NoteBox(mediabox, destY, text, coords);
}

static RectF DetectRegion(EngineBase* engine, int pageNo, RectF mediabox, float destX, float destY, RectF* continuation,
                          MissingEntry missingEntry, bool* isBibEntry, EntryPageWrap* pageWrap) {
    *isBibEntry = false;
    *pageWrap = {};
    AutoArenaSavepoint tempScope;

    Rect* normCoords = nullptr;
    TempWStr text = RefHoverPageTextTemp(engine, pageNo, &normCoords);

    RectF region = DetectEquationBox(text, normCoords, mediabox, destX, destY);
    if (region.dx <= 0.f || region.dy <= 0.f) {
        region = DetectEntryBox(text, normCoords, mediabox, destX, destY, continuation, isBibEntry, pageWrap);
    }
    if ((region.dx <= 0.f || region.dy <= 0.f) && missingEntry == MissingEntry::Landscape) {
        region = LandscapeBox(mediabox, destX, destY, text, normCoords);
    }
    return region;
}

// pageZoom is the destination page's current display zoom (px-per-pt) —
// used as the initial render zoom so popup text height matches the page.
void RefHoverOnTimer(RefHoverState* s, EngineBase* engine, float pageZoom) {
    if (!s || !engine || s->pending.destPage <= 0) {
        return;
    }
#if !defined(SUMATRA_NG)
    if (s->pending.bibProbe) {
        s->pending.bibProbe = false;
        RefHoverBibProbeOnTimer(s, engine);
        return;
    }
#endif
    if (s->hitEngine != engine) {
        if (s->hitEngine) {
            s->hitEngine->Release();
        }
        s->hitEngine = engine;
        engine->AddRef();
    }
    int destPage = s->pending.destPage;
    float destX = s->pending.destX;
    float destY = s->pending.destY;

    RectF mediabox = engine->PageMediabox(destPage);
    if (mediabox.dx <= 0.f || mediabox.dy <= 0.f) {
        return;
    }
    if (destY <= 0.f || destY >= mediabox.dy - 1.f) {
        destY = 0.f;
        float resolved = RefHoverResolveDestYFromSourceText(engine, s->pending.srcPage, s->pending.srcRect, destPage);
        if (resolved >= 0.f) {
            destY = resolved;
            destX = std::max(destX, 0.f);
        }
    }

    float linkZoom = s->pending.destZoom;
    bool useLinkZoom = (linkZoom > 0.f);
    if (useLinkZoom && pageZoom > linkZoom) {
        linkZoom = pageZoom;
    }

    RectF region;
    // Only set true when the destination is a fitted bibliography / glossary
    // entry (DetectEntryBox path) — gates the JabRef push existence check.
    bool isBibEntry = false;
    // Set when a bracket-style entry wraps across a 2-column page break (e.g.
    // "[63]"): a second crop, stitched below `region` in the delivered
    // bitmap. Empty (dx/dy <= 0) otherwise.
    RectF continuation{};
    EntryPageWrap pageWrap;
    RectF noteRegion = s->pending.noteRef ? DetectNoteRegion(engine, destPage, mediabox, destY) : RectF{};
    if (noteRegion.dx > 0.f && noteRegion.dy > 0.f) {
        region = noteRegion;
    } else if (useLinkZoom) {
        region = RectF{0.f, destY, mediabox.dx, mediabox.dy - destY};
    } else {
        region = DetectRegion(engine, destPage, mediabox, destX, destY, &continuation, MissingEntry::Empty, &isBibEntry,
                              &pageWrap);
        bool regionEmpty = region.dx <= 0.f || region.dy <= 0.f;
        bool searchNext = regionEmpty || ShouldSearchNextPage(mediabox, destY);
        if (searchNext && !regionEmpty) {
            float currentY = RefHoverResolveDestYFromSourceText(engine, s->pending.srcPage, s->pending.srcRect,
                                                                destPage, RefHoverTextMatch::Best);
            searchNext = currentY < 0.f;
        }

        float nextY = -1.f;
        if (searchNext) {
            int nextPage = destPage + 1;
            if (nextPage <= engine->PageCount()) {
                nextY = RefHoverResolveDestYFromSourceText(engine, s->pending.srcPage, s->pending.srcRect, nextPage,
                                                           RefHoverTextMatch::Best);
            }
            if (nextY >= 0.f) {
                destPage = nextPage;
                destY = nextY;
                destX = std::max(destX, 0.f);
                mediabox = engine->PageMediabox(destPage);
                region = DetectRegion(engine, destPage, mediabox, destX, destY, &continuation, MissingEntry::Landscape,
                                      &isBibEntry, &pageWrap);
            }
        }
        if (regionEmpty && nextY < 0.f) {
            region = DetectRegion(engine, destPage, mediabox, destX, destY, &continuation, MissingEntry::Landscape,
                                  &isBibEntry, &pageWrap);
        }
    }
    // The entry ends its column with nothing below: look for its tail at the
    // top of the next page's reference column and stitch it below the popup.
    int continuationPageNo = 0;
    if (isBibEntry && pageWrap.mayWrap && continuation.dx <= 0.f && destPage < engine->PageCount()) {
        AutoArenaSavepoint tempScope;
        Rect* nextCoords = nullptr;
        TempWStr nextText = RefHoverPageTextTemp(engine, destPage + 1, &nextCoords);
        RectF nextMediabox = engine->PageMediabox(destPage + 1);
        RectF tail = FindPageWrapContinuation(nextText, nextCoords, nextMediabox, pageWrap.indentDx);
        if (tail.dx > 0.f && tail.dy > 0.f) {
            continuation = tail;
            continuationPageNo = destPage + 1;
        }
    }
    bool hasContinuation = continuation.dx > 0.f && continuation.dy > 0.f;
    s->displayed.userZoom = 1.f;
    float baseZoom = kRefHoverRenderZoom;
    if (useLinkZoom) {
        baseZoom = linkZoom;
    } else if (pageZoom > 0.f) {
        baseZoom = pageZoom;
    }

    int popupWCap = RefHoverPopupWidthCap(s, DpiScale(kRefHoverMaxPopupWidth));
    // Combined content extent (region stacked above continuation, if any) used
    // for sizing below; req.region itself stays just the primary crop.
    float contentDy = region.dy + (hasContinuation ? continuation.dy : 0.f);
    float contentDx = hasContinuation && continuation.dx > region.dx ? continuation.dx : region.dx;

    int popupHCap;
    int cursorPad = DpiScale(kRefHoverCursorPad);
    if (contentDy > 250.f && s->pending.pageScreenRect.dy > 0) {
        Rect pr = s->pending.pageScreenRect;
        int curY = s->pending.screenPt.y;
        int spaceAbove = curY - pr.y - cursorPad;
        int spaceBelow = (pr.y + pr.dy) - curY - cursorPad;
        int maxSpace = (spaceAbove > spaceBelow) ? spaceAbove : spaceBelow;
        maxSpace = std::max(maxSpace, 0);
        int pageBased = pr.dy * 75 / 100;
        popupHCap = (pageBased > maxSpace) ? pageBased : maxSpace;
    } else {
        popupHCap = DpiScale(kRefHoverMaxPopupHeight);
        if (s->pending.pageScreenRect.dy > 0) {
            int pageBased = s->pending.pageScreenRect.dy * 45 / 100;
            popupHCap = std::min(pageBased, popupHCap);
        }
    }
    int border = DpiScale(kRefHoverBorder);
    float availH = (float)(popupHCap - (2 * border));
    float availW = (float)(popupWCap - (2 * border));
    if (useLinkZoom) {
        float wantW = availW / baseZoom;
        float wantH = availH / baseZoom;
        float maxW = mediabox.dx - region.x;
        float maxH = mediabox.dy - region.y;
        wantW = std::min(wantW, maxW);
        wantH = std::min(wantH, maxH);
        wantW = std::max(wantW, 1.f);
        wantH = std::max(wantH, 1.f);
        region.dx = wantW;
        region.dy = wantH;
    } else {
        if (contentDy > 0.f && contentDy * baseZoom > availH) {
            baseZoom = availH / contentDy;
        }
        if (contentDx > 0.f && contentDx * baseZoom > availW) {
            baseZoom = availW / contentDx;
        }
    }
    baseZoom = std::max(baseZoom, kRefHoverMinUserZoom);
    s->displayed.baseZoom = baseZoom;

    RefHoverState::RenderRequest req;
    req.pageNo = destPage;
    req.zoom = s->displayed.baseZoom * s->displayed.userZoom;
    req.region = region;
    if (hasContinuation) {
        req.continuationRegion = continuation;
        req.continuationPageNo = continuationPageNo;
    }
    req.showPopup = true;
    req.screenPt = s->pending.screenPt;
    req.destPageRaw = s->pending.destPage;
    req.destXRaw = s->pending.destX;
    req.destYRaw = s->pending.destY;
    req.srcPageRaw = s->pending.srcPage;
    req.srcRectRaw = s->pending.srcRect;
    RefHoverRequestRender(s, engine, req);

    // JabRef push: capture the bib entry text and fire the existence check
    // (lights the popup-corner badge) for the freshly-shown destination.
#if !defined(SUMATRA_NG)
    RefHoverCaptureEntryAndCheck(s, engine, destPage, region, isBibEntry, continuationPageNo, continuation);
#endif
}

#if !defined(SUMATRA_NG)
// Bib-probe timer: the cursor rests on plain bibliography text (no link, no
// citation pattern). Detect the entry under it, light the corner badge via the
// usual capture + existence check, and remember the rect so Ctrl+J and the
// marker dot anchor at the entry itself. No popup — the entry is on screen.
void RefHoverBibProbeOnTimer(RefHoverState* s, EngineBase* engine) {
    int pageNo = s->pending.destPage;
    float x = s->pending.destX;
    float y = s->pending.destY;
    s->pending.destPage = -1;

    RectF mediabox = engine->PageMediabox(pageNo);
    if (mediabox.dx <= 0.f || mediabox.dy <= 0.f) {
        RefHoverClearBibHover(s, s->hwndCanvas);
        return;
    }
    AutoArenaSavepoint tempScope;
    Rect* coords = nullptr;
    TempWStr text = RefHoverPageTextTemp(engine, pageNo, &coords);
    bool isBibEntry = false;
    RectF region = DetectEntryBoxAt(text, coords, mediabox, x, y, &isBibEntry);
    if (!isBibEntry) {
        RefHoverClearBibHover(s, s->hwndCanvas);
        return;
    }
    if (s->bibHoverPage == pageNo && s->bibHoverRect == region) {
        return; // same entry: badge already live, don't re-check
    }

    s->bibHoverPage = pageNo;
    s->bibHoverRect = region;
    s->pending.srcPage = pageNo;
    s->pending.srcRect = region;
    RefHoverCaptureEntryAndCheck(s, engine, pageNo, region, true);
    InvalidateRect(s->hwndCanvas, nullptr, FALSE);
}
#endif
