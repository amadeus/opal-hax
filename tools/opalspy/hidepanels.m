// hidepanels: change what some of Opal's SwiftUI views are, in the custom build:
//   - Settings > Updates (version/build, git hash, "Automatically install updates", changelog): hidden; the update
//     machinery itself is patched out of the binary at build time (build_custom_opal.py CODE_PATCHES)
//   - Audio > Noise cancellation: hidden (owner's request)
//   - Feedback (owner, 2026-11-09: the app is abandoned): the "Send Feedback" button at the bottom of every tab becomes
//     Opal's own Quit button (the one on the Settings tab); the feedback form (SendFeedbackView, which submitted a
//     Sentry "User Feedback"), its header and its "email hello@opalcamera.com" footer are hidden. Feedback mode also
//     can't be entered any more (CODE_PATCHES: the isFeedbackViewVisible setter always stores false).
// The tab heights are adjusted at build time (DATA_PATCHES / CODE_PATCHES) so no gap is left.
//
// How: each tab's panel list is a SwiftUI TupleView with the panel as a fixed element. We can't change that type, so
// we change what the panel *is* to SwiftUI: in its `View` conformance witness table we set the associated type
// `Body`, the `Body: View` conformance and the `body` getter. Hidden = EmptyView's (no-op getter: EmptyView has no
// storage). "Acts as another view" = that view's three witnesses, which is only valid when both structs are
// field-less (the getter then never reads `self`; checked below). The witness tables are instantiated at runtime
// (View is a resilient protocol), so we fetch them with swift_getWitnessTable and overwrite the slots before any
// SwiftUI view is built. Already-resolved slots are plain pointers (low bit clear), which is what the runtime expects
// after its own lazy resolution.
//
// Types and conformances are looked up by name in Opal's __swift5_types / __swift5_proto sections, so nothing here is
// tied to addresses; a missing name is logged and skipped.
#import <Foundation/Foundation.h>
#import <dlfcn.h>
#import <mach-o/dyld.h>
#import <mach-o/getsect.h>
#import <string.h>

void opalspy_log(NSString *msg);
extern const void *swift_getWitnessTable(const void *conformance, const void *type, const void *const *args);
extern const void *swift_conformsToProtocol(const void *type, const void *protocol);
typedef struct { const void *metadata; uintptr_t state; } MetadataResponse;
extern MetadataResponse swift_getAssociatedTypeWitness(uintptr_t request, const void *wtable, const void *conformingType,
                                                       const void *reqBase, const void *assocType);
extern const void *swift_getAssociatedConformanceWitness(const void *wtable, const void *conformingType, const void *assocType,
                                                         const void *reqBase, const void *assocConformance);

static const char *kHide[] = {"UpdatesPanelView", "NoiseCancellationPanelView", "SendFeedbackView", "FeedbackModeHeaderView",
                              "FeedbackModeFooterView"};
static const struct { const char *name, *actsAs; } kReplace[] = {{"SendFeedbackButton", "QuitButton"}};

static const char *relptr(const char *at) { return at + *(const int32_t *)at; }

static const void *gViewProto, *gEmptyMeta, *gEmptyWT;

// struct type descriptor by name (kind 17 = struct; indirect entries are imported types, skipped)
static const char *findStruct(const char *name) {
    unsigned long size = 0;
    const char *sec = (const char *)getsectiondata((const struct mach_header_64 *)_dyld_get_image_header(0), "__TEXT", "__swift5_types", &size);
    for(unsigned long off = 0; sec && off + 4 <= size; off += 4) {
        int32_t v = *(const int32_t *)(sec + off);
        if(v & 3) continue;
        const char *td = sec + off + v;
        if((*(const uint32_t *)td & 0x1f) == 17 && strcmp(relptr(td + 8), name) == 0) return td;
    }
    return NULL;
}

// the type's conformance descriptor to SwiftUI.View
static const char *findViewConformance(const char *td) {
    unsigned long size = 0;
    const char *sec = (const char *)getsectiondata((const struct mach_header_64 *)_dyld_get_image_header(0), "__TEXT", "__swift5_proto", &size);
    for(unsigned long off = 0; sec && off + 4 <= size; off += 4) {
        const char *cd = relptr(sec + off);
        if(((*(const uint32_t *)(cd + 12) >> 3) & 7) != 0) continue;  // type reference kind: direct type descriptor
        if(relptr(cd + 4) != td) continue;
        int32_t pr = *(const int32_t *)cd;  // protocol: relative, low bit = indirect through a GOT slot
        const char *proto = (pr & 1) ? *(const char *const *)(cd + (pr & ~1)) : cd + pr;
        if(proto == gViewProto) return cd;
    }
    return NULL;
}

typedef struct {
    const char *td;
    const void *meta;
    void **wt;
    const char *reqs;                          // View's requirements array
    int bodyType, bodyConf, bodyGetter;        // their indexes in it
} ViewInfo;

static BOOL viewInfo(const char *name, ViewInfo *vi) {
    vi->td = findStruct(name);
    const char *cd = vi->td ? findViewConformance(vi->td) : NULL;
    if(!cd) { opalspy_log([NSString stringWithFormat:@"hidepanels: %s (or its View conformance) not found, skipped", name]); return NO; }
    MetadataResponse (*access)(uintptr_t) = (MetadataResponse(*)(uintptr_t))relptr(vi->td + 12);
    vi->meta = access(0).metadata;
    vi->wt = (void **)swift_getWitnessTable(cd, vi->meta, NULL);
    if(!vi->meta || !vi->wt) { opalspy_log([NSString stringWithFormat:@"hidepanels: %s witness table unavailable", name]); return NO; }
    // Protocol descriptor: flags, parent, name, numRequirementsInSignature, numRequirements, assocTypeNames,
    // then the requirement signature (12 B each), then the requirements (8 B: flags, default impl).
    const char *proto = gViewProto;
    uint32_t nSig = *(const uint32_t *)(proto + 12), nReq = *(const uint32_t *)(proto + 16);
    vi->reqs = proto + 24 + nSig * 12;
    vi->bodyType = vi->bodyConf = vi->bodyGetter = -1;
    for(uint32_t i = 0; i < nReq; i++) {
        uint32_t kind = *(const uint32_t *)(vi->reqs + i * 8) & 0xF;
        if(kind == 7) vi->bodyType = i;         // associated type Body
        else if(kind == 8) vi->bodyConf = i;    // Body : View
        else if(kind == 3) vi->bodyGetter = i;  // var body { get }
    }
    if(vi->bodyType < 0 || vi->bodyConf < 0 || vi->bodyGetter < 0) { opalspy_log(@"hidepanels: View requirements not found"); return NO; }
    return YES;
}

// witness table slot of requirement i (slot 0 is the conformance descriptor)
static void **slot(const ViewInfo *vi, int i) { return &vi->wt[1 + i]; }

static void empty_body(void) {}  // body getter for an EmptyView body: nothing to initialize

static void hide(const char *name) {
    ViewInfo v;
    if(!viewInfo(name, &v)) return;
    *slot(&v, v.bodyType) = (void *)gEmptyMeta;
    *slot(&v, v.bodyConf) = (void *)gEmptyWT;
    *slot(&v, v.bodyGetter) = (void *)empty_body;
    opalspy_log([NSString stringWithFormat:@"hidepanels: %s hidden", name]);
}

static void replace(const char *name, const char *actsAs) {
    ViewInfo v, a;
    if(!viewInfo(name, &v) || !viewInfo(actsAs, &a)) return;
    // field-less structs only: the borrowed body getter must not read `self`
    uint32_t nv = *(const uint32_t *)(v.td + 20), na = *(const uint32_t *)(a.td + 20);
    if(nv || na) {
        opalspy_log([NSString stringWithFormat:@"hidepanels: %s/%s have stored properties (%u/%u), not replacing", name, actsAs, nv, na]);
        return;
    }
    // resolve the borrowed view's Body type and Body: View conformance through the runtime (its slots may still be lazy)
    const void *reqBase = a.reqs - 8;
    const void *body = swift_getAssociatedTypeWitness(0, a.wt, a.meta, reqBase, a.reqs + a.bodyType * 8).metadata;
    const void *bodyWT = body ? swift_getAssociatedConformanceWitness(a.wt, a.meta, body, reqBase, a.reqs + a.bodyConf * 8) : NULL;
    if(!body || !bodyWT) { opalspy_log([NSString stringWithFormat:@"hidepanels: %s's Body not resolved", actsAs]); return; }
    *slot(&v, v.bodyType) = (void *)body;
    *slot(&v, v.bodyConf) = (void *)bodyWT;
    *slot(&v, v.bodyGetter) = *slot(&a, a.bodyGetter);
    opalspy_log([NSString stringWithFormat:@"hidepanels: %s now acts as %s", name, actsAs]);
}

void opal_hidepanels_install(void) {
    void *swiftui = dlopen("/System/Library/Frameworks/SwiftUI.framework/SwiftUI", RTLD_NOLOAD | RTLD_NOW);
    gViewProto = swiftui ? dlsym(swiftui, "$s7SwiftUI4ViewMp") : NULL;
    gEmptyMeta = swiftui ? dlsym(swiftui, "$s7SwiftUI9EmptyViewVN") : NULL;
    gEmptyWT = gViewProto && gEmptyMeta ? swift_conformsToProtocol(gEmptyMeta, gViewProto) : NULL;
    if(!gEmptyWT) { opalspy_log(@"hidepanels: SwiftUI symbols not found"); return; }
    for(size_t i = 0; i < sizeof kHide / sizeof kHide[0]; i++) hide(kHide[i]);
    for(size_t i = 0; i < sizeof kReplace / sizeof kReplace[0]; i++) replace(kReplace[i].name, kReplace[i].actsAs);
}
