// noupdates: hide Opal's "Updates" settings panel (version/build, git hash, "Automatically install updates",
// changelog) in the custom build. The update machinery itself is patched out of the binary at build time
// (build_custom_opal.py CODE_PATCHES); this only removes the panel from the UI.
//
// How: SettingsView's panel list is a SwiftUI TupleView whose 5th element is UpdatesPanelView. We can't change
// that type, so we change what UpdatesPanelView *is* to SwiftUI: in its `View` conformance witness table we make
// `Body` = EmptyView, `Body: View` = EmptyView's conformance and `body` = a no-op getter (EmptyView has no
// storage). SwiftUI then renders nothing and reserves no space for it. The witness table is instantiated at
// runtime (View is a resilient protocol), so we fetch it with swift_getWitnessTable and overwrite the slots
// before any SwiftUI view is built. Already-resolved slots are plain pointers (low bit clear), which is what the
// runtime expects after its own lazy resolution.
//
// Addresses are for Opal 0.26 (25.992) arm64; checked against the type descriptor's name before touching anything.
#import <Foundation/Foundation.h>
#import <dlfcn.h>
#import <mach-o/dyld.h>
#import <string.h>

void opalspy_log(NSString *msg);
extern const void *swift_getWitnessTable(const void *conformance, const void *type, const void *const *args);
extern const void *swift_conformsToProtocol(const void *type, const void *protocol);

static const uintptr_t kConformance = 0x10049c100;  // UpdatesPanelView : SwiftUI.View (protocol conformance descriptor)
static const uintptr_t kTypeDesc = 0x10049c1e4;     // UpdatesPanelView nominal type descriptor
static const uintptr_t kMetadata = 0x100543e40;     // UpdatesPanelView type metadata

static void empty_body(void) {}  // body getter for an EmptyView body: nothing to initialize

static const char *relptr(const char *at) { return at + *(const int32_t *)at; }

void opal_noupdates_install(void) {
    intptr_t slide = _dyld_get_image_vmaddr_slide(0);
    const char *td = (const char *)(kTypeDesc + slide);
    const char *name = relptr(td + 8);
    if(strcmp(name, "UpdatesPanelView") != 0) {
        opalspy_log([NSString stringWithFormat:@"noupdates: unexpected binary (type at %#lx is '%.40s'), not patching",
                     (unsigned long)kTypeDesc, name]);
        return;
    }
    const char *conf = (const char *)(kConformance + slide);
    if(relptr(conf + 4) != td) { opalspy_log(@"noupdates: conformance does not belong to UpdatesPanelView"); return; }
    int32_t pr = *(const int32_t *)conf;  // protocol: relative, low bit = indirect through a GOT slot
    const char *proto = (pr & 1) ? *(const char *const *)(conf + (pr & ~1)) : conf + pr;

    void *swiftui = dlopen("/System/Library/Frameworks/SwiftUI.framework/SwiftUI", RTLD_NOLOAD | RTLD_NOW);
    const void *viewProto = swiftui ? dlsym(swiftui, "$s7SwiftUI4ViewMp") : NULL;
    const void *emptyMeta = swiftui ? dlsym(swiftui, "$s7SwiftUI9EmptyViewVN") : NULL;
    if(!viewProto || proto != viewProto || !emptyMeta) { opalspy_log(@"noupdates: SwiftUI symbols not found"); return; }
    const void *emptyWT = swift_conformsToProtocol(emptyMeta, viewProto);
    void **wt = (void **)swift_getWitnessTable(conf, (const void *)(kMetadata + slide), NULL);
    if(!emptyWT || !wt) { opalspy_log(@"noupdates: witness tables unavailable"); return; }

    // Protocol descriptor: flags, parent, name, numRequirementsInSignature, numRequirements, assocTypeNames,
    // then the requirement signature (12 B each), then the requirements (8 B: flags, default impl).
    uint32_t nSig = *(const uint32_t *)(proto + 12), nReq = *(const uint32_t *)(proto + 16);
    const uint32_t *reqs = (const uint32_t *)(proto + 24 + nSig * 12);
    void **bodyType = NULL, **bodyConf = NULL, **bodyGetter = NULL;  // slot 0 is the conformance descriptor
    for(uint32_t i = 0; i < nReq; i++) {
        uint32_t kind = reqs[i * 2] & 0xF;
        if(kind == 7) bodyType = &wt[1 + i];         // associated type Body
        else if(kind == 8) bodyConf = &wt[1 + i];    // Body : View
        else if(kind == 3) bodyGetter = &wt[1 + i];  // var body { get }
    }
    if(!bodyType || !bodyConf || !bodyGetter) { opalspy_log(@"noupdates: View requirements not found"); return; }
    *bodyType = (void *)emptyMeta;
    *bodyConf = (void *)emptyWT;
    *bodyGetter = (void *)empty_body;
    opalspy_log(@"noupdates: Updates settings panel hidden");
}
