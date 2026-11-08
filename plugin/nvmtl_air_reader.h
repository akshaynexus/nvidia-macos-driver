/*
 * NullMoth NVIDIA driver for macOS
 * Copyright (c) 2026 NullMoth Systems.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
// nvmtl_air_reader.h - the LLVM C API IR walks the in-process AIR reader (NVMTLLibraryLoad.m, nvmtl_air_in_memory)
// runs on a parsed AIR module before printing it for the translator. Plain C with dlsym only, so the Linux harness
// plugin/tests/air_xlate_check.c runs the same code against a host libLLVM of the release's version (23.1.4) and the
// translator built for the host: nothing here may depend on Objective-C or on macOS.
#ifndef NVMTL_AIR_READER_H
#define NVMTL_AIR_READER_H
#include <dlfcn.h>
#include <stdint.h>
#include <string.h>

// Run on a module whose bitcode names air.simdgroup_matrix_8x8 (nvmtl_air_unroll_wanted), then nvmtl_retype_byte_geps.
#define NVMTL_AIR_UNROLL_PIPELINE "function(sroa,loop(loop-rotate),indvars,loop-unroll<O3>,sroa,simplifycfg,early-cse)"

// NVMTL_CANONICAL_POINTERS (app profiles): always-inline + SROA, then instcombine only when a pointer phi/select exists.
#define NVMTL_AIR_CANON_PIPELINE "always-inline,function(sroa,simplifycfg)"
#define NVMTL_AIR_CHOICE_PIPELINE "function(instcombine,simplifycfg)"

struct nvmtl_llvm_walk {
    void *(*GetFirstGlobal)(void *); void *(*GetNextGlobal)(void *); void *(*GlobalGetValueType)(void *);
    void *(*GetFirstUse)(void *); void *(*GetNextUse)(void *); void *(*GetUser)(void *);
    void *(*IsAConstantExpr)(void *); int (*GetConstOpcode)(void *); void *(*GetGEPSourceElementType)(void *);
    int (*GetNumOperands)(void *); void *(*GetOperand)(void *, unsigned); void *(*IsAConstantInt)(void *);
    long long (*ConstIntGetSExtValue)(void *); int (*GetTypeKind)(void *); unsigned (*GetIntTypeWidth)(void *);
    void *(*GetElementType)(void *); unsigned long long (*GetArrayLength2)(void *); void *(*GetModuleContext)(void *);
    void *(*Int64TypeInContext)(void *); void *(*ConstInt)(void *, unsigned long long, int);
    void *(*ConstInBoundsGEP2)(void *, void *, void **, unsigned); void (*ReplaceAllUsesWith)(void *, void *);
    void *(*GetFirstFunction)(void *); void *(*GetNextFunction)(void *); int (*GetLinkage)(void *);
    void *(*IsACallInst)(void *); void *(*GetCalledValue)(void *);
    unsigned (*GetEnumAttributeKindForName)(const char *, size_t); void *(*CreateEnumAttribute)(void *, unsigned, uint64_t);
    void (*AddAttributeAtIndex)(void *, unsigned, void *); void *(*GetEnumAttributeAtIndex)(void *, unsigned, unsigned);
    void *(*GetInstructionParent)(void *); void *(*GetBasicBlockParent)(void *);
    void *(*GetFirstBasicBlock)(void *); void *(*GetNextBasicBlock)(void *);
    void *(*GetFirstInstruction)(void *); void *(*GetNextInstruction)(void *);
    void *(*IsAPHINode)(void *); void *(*IsASelectInst)(void *); void *(*TypeOf)(void *);
};
typedef struct nvmtl_llvm_walk nvmtl_llvm_walk;
#define NVMTL_LLVM_WALK_NAMES "LLVMGetFirstGlobal", "LLVMGetNextGlobal", "LLVMGlobalGetValueType", "LLVMGetFirstUse", \
    "LLVMGetNextUse", "LLVMGetUser", "LLVMIsAConstantExpr", "LLVMGetConstOpcode", "LLVMGetGEPSourceElementType", \
    "LLVMGetNumOperands", "LLVMGetOperand", "LLVMIsAConstantInt", "LLVMConstIntGetSExtValue", "LLVMGetTypeKind", \
    "LLVMGetIntTypeWidth", "LLVMGetElementType", "LLVMGetArrayLength2", "LLVMGetModuleContext", "LLVMInt64TypeInContext", \
    "LLVMConstInt", "LLVMConstInBoundsGEP2", "LLVMReplaceAllUsesWith", \
    "LLVMGetFirstFunction", "LLVMGetNextFunction", "LLVMGetLinkage", "LLVMIsACallInst", "LLVMGetCalledValue", \
    "LLVMGetEnumAttributeKindForName", "LLVMCreateEnumAttribute", "LLVMAddAttributeAtIndex", "LLVMGetEnumAttributeAtIndex", \
    "LLVMGetInstructionParent", "LLVMGetBasicBlockParent", \
    "LLVMGetFirstBasicBlock", "LLVMGetNextBasicBlock", "LLVMGetFirstInstruction", "LLVMGetNextInstruction", \
    "LLVMIsAPHINode", "LLVMIsASelectInst", "LLVMTypeOf"
static int nvmtl_scalar_bytes(const nvmtl_llvm_walk *w, void *t) {
    switch (w->GetTypeKind(t)) {
    case 1: case 18: return 2; case 2: return 4; case 3: return 8;
    case 8: { unsigned b = w->GetIntTypeWidth(t); return (b == 8 || b == 16 || b == 32 || b == 64) ? (int)(b / 8) : 0; }
    default: return 0;
    }
}
static int nvmtl_retype_byte_geps(const nvmtl_llvm_walk *w, void *module) {
    void *i64 = w->Int64TypeInContext(w->GetModuleContext(module)); int n = 0;
    for (void *g = w->GetFirstGlobal(module); g; g = w->GetNextGlobal(g)) {
        void *vt = w->GlobalGetValueType(g);
        if (w->GetTypeKind(vt) != 11) continue;
        void *et = w->GetElementType(vt); int sz = nvmtl_scalar_bytes(w, et);
        if (sz <= 1) continue;
        unsigned long long len = w->GetArrayLength2(vt);
        void *hits[256]; void *repl[256]; int nh = 0;
        for (void *u = w->GetFirstUse(g); u && nh < 256; u = w->GetNextUse(u)) {
            void *ce = w->GetUser(u);
            if (!w->IsAConstantExpr(ce) || w->GetConstOpcode(ce) != 29 ) continue;
            if (w->GetNumOperands(ce) != 2 || w->GetOperand(ce, 0) != g) continue;
            void *src = w->GetGEPSourceElementType(ce);
            if (w->GetTypeKind(src) != 8 || w->GetIntTypeWidth(src) != 8) continue;
            void *ix = w->GetOperand(ce, 1); if (!w->IsAConstantInt(ix)) continue;
            long long b = w->ConstIntGetSExtValue(ix);
            if (b < 0 || b % sz || (unsigned long long)(b / sz) > len) continue;
            void *idx[2] = { w->ConstInt(i64, 0, 0), w->ConstInt(i64, (unsigned long long)(b / sz), 0) };
            hits[nh] = ce; repl[nh] = w->ConstInBoundsGEP2(vt, g, idx, 2); nh++;
        }
        for (int k = 0; k < nh; k++) if (repl[k] && repl[k] != hits[k]) { w->ReplaceAllUsesWith(hits[k], repl[k]); n++; }
    }
    return n;
}
static int nvmtl_llvm_walk_load(nvmtl_llvm_walk *w, void *library) {
    static const char *names[] = { NVMTL_LLVM_WALK_NAMES }; void **slot = (void **)w;
    for (unsigned k = 0; k < sizeof names / sizeof *names; k++) if (!(slot[k] = dlsym(library, names[k]))) return 0;
    return 1;
}

static int nvmtl_has_pointer_choice(const nvmtl_llvm_walk *w, void *module) {
    for (void *fn = w->GetFirstFunction(module); fn; fn = w->GetNextFunction(fn))
        for (void *bb = w->GetFirstBasicBlock(fn); bb; bb = w->GetNextBasicBlock(bb))
            for (void *i = w->GetFirstInstruction(bb); i; i = w->GetNextInstruction(i))
                if ((w->IsAPHINode(i) || w->IsASelectInst(i)) && w->GetTypeKind(w->TypeOf(i)) == 12) return 1;
    return 0;
}

// Marks every internal helper with exactly one direct caller always-inline, so the pointer it returns or takes is
// visible as SSA to the translator instead of crossing a call. One caller means no helper body is duplicated.
static unsigned nvmtl_inline_single_use_helpers(const nvmtl_llvm_walk *w, void *module) {
    unsigned inl = w->GetEnumAttributeKindForName("alwaysinline", strlen("alwaysinline"));
    unsigned no = w->GetEnumAttributeKindForName("noinline", strlen("noinline"));
    if (!inl || !no) return 0;
    void *context = w->GetModuleContext(module); unsigned count = 0;
    for (void *fn = w->GetFirstFunction(module); fn; fn = w->GetNextFunction(fn)) {
        int linkage = w->GetLinkage(fn);
        if (linkage != 8 && linkage != 9) continue; // LLVMInternalLinkage and LLVMPrivateLinkage
        void *use = w->GetFirstUse(fn);
        if (!use || w->GetNextUse(use)) continue;
        void *call = w->GetUser(use);
        if (!w->IsACallInst(call) || w->GetCalledValue(call) != fn) continue;
        if (w->GetBasicBlockParent(w->GetInstructionParent(call)) == fn) continue;
        if (w->GetEnumAttributeAtIndex(fn, ~0u, no)) continue;
        w->AddAttributeAtIndex(fn, ~0u, w->CreateEnumAttribute(context, inl, 0)); count++;
    }
    return count;
}

#endif
