/*
 * NullMoth NVIDIA driver for macOS
 * Copyright (c) 2026 NullMoth Systems.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
// air_xlate_check.c - the plugin's shader path from AIR bitcode to SPIR-V, on Linux.
//
//   air_xlate_check <libLLVM.so> <libnvmtl_translate.so> <out-dir> <canonical 0|1> <module.bc>...
//
// What the plugin does per Metal library function (NVMTLLibraryLoad.m: nvmtl_bitcode_reader_ready,
// nvmtl_air_in_memory, nvmtl_entry_of, nvmtl_translate), with the same LLVM C API calls through dlsym and the same IR
// walks (nvmtl_air_reader.h, shared with the plugin):
//   1. parse the bitcode with an error-severity diagnostic handler;
//   2. a module naming air.simdgroup_matrix_8x8: NVMTL_AIR_UNROLL_PIPELINE, then the byte-GEP retype;
//   3. canonical = 1 (NVMTL_CANONICAL_POINTERS, set by app profiles): single-use helpers always-inline,
//      NVMTL_AIR_CANON_PIPELINE, NVMTL_AIR_CHOICE_PIPELINE when a pointer phi/select exists, the byte-GEP retype again
//      on an unrolled module, "icmp samesign " -> "icmp ";
//   4. print the module; the stage is the first of !air.vertex/fragment/kernel present;
//   5. nvmtl_translate_set_caps(3|4) as nvmtl_xlate_ready does, then nvmtl_translate(text, stage).
// Writes <out>/<base>.ll, and <base>.spv or <base>.err. One line per module on stdout:
//   ok   <base> <stage> <ir bytes> -> <spv bytes>
//   FAIL <base> <stage> <reason>
// The SPIR-V is validated by the caller (air_xlate_check.py runs spirv-val --target-env vulkan1.2).
// Exit 0 when every module translated, 1 otherwise, 2 usage / load errors.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../nvmtl_air_reader.h"

static void *(*ContextCreate)(void);
static void (*ContextDispose)(void *);
static void (*ContextSetDiagnosticHandler)(void *, void (*)(void *, void *), void *);
static void *(*CreateMemoryBufferWithMemoryRangeCopy)(const char *, size_t, const char *);
static void (*DisposeMemoryBuffer)(void *);
static int (*ParseBitcodeInContext2)(void *, void *, void **);
static char *(*PrintModuleToString)(void *);
static void (*DisposeModule)(void *);
static void (*DisposeMessage)(char *);
static char *(*GetDiagInfoDescription)(void *);
static int (*GetDiagInfoSeverity)(void *);
static void *(*RunPasses)(void *, const char *, void *, void *);
static void *(*CreatePassBuilderOptions)(void);
static void (*DisposePassBuilderOptions)(void *);
static char *(*GetErrorMessage)(void *);
static void (*DisposeErrorMessage)(char *);
static nvmtl_llvm_walk walk;

typedef int (*xlate_fn)(const char *, const char *, uint8_t **, size_t *, char *, size_t);
typedef void (*xfree_fn)(uint8_t *, size_t);
typedef void (*setcaps_fn)(uint32_t);

static int g_error;
static void diag(void *d, void *ctx) {
	(void)ctx;
	if (GetDiagInfoSeverity(d) == 0) g_error = 1;   // LLVMDSError
	char *m = GetDiagInfoDescription(d);
	if (m) { fprintf(stderr, "  bitcode reader: %s\n", m); DisposeMessage(m); }
}

static int passes(void *module, const char *pipeline, char *why, size_t n) {
	void *opts = CreatePassBuilderOptions();
	void *err = RunPasses(module, pipeline, NULL, opts);
	DisposePassBuilderOptions(opts);
	if (!err) return 0;
	char *m = GetErrorMessage(err);
	snprintf(why, n, "passes '%s': %s", pipeline, m ? m : "?");
	if (m) DisposeErrorMessage(m);
	return 1;
}

static void replace_all(char *s, const char *from, const char *to) {   // to is not longer than from
	size_t fl = strlen(from), tl = strlen(to);
	char *r = s, *w = s;
	while (*r) {
		if (!strncmp(r, from, fl)) { memcpy(w, to, tl); w += tl; r += fl; }
		else *w++ = *r++;
	}
	*w = 0;
}

static const char *stage_of(const char *ll) {
	static const char *st[] = { "vertex", "fragment", "kernel" };
	for (unsigned i = 0; i < 3; i++) {
		char key[32];
		snprintf(key, sizeof key, "!air.%s = !{!", st[i]);
		if (strstr(ll, key)) return st[i];
	}
	return NULL;
}

static int put(const char *dir, const char *base, const char *ext, const void *p, size_t n) {
	char path[4096];
	snprintf(path, sizeof path, "%s/%s.%s", dir, base, ext);
	FILE *f = fopen(path, "wb");
	if (!f) return -1;
	size_t w = fwrite(p, 1, n, f);
	return (fclose(f) || w != n) ? -1 : 0;
}

int main(int argc, char **argv) {
	if (argc < 6) {
		fprintf(stderr, "usage: %s libLLVM.so libnvmtl_translate.so out-dir canonical(0|1) module.bc...\n", argv[0]);
		return 2;
	}
	void *h = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
	if (!h) { fprintf(stderr, "air_xlate_check: dlopen %s: %s\n", argv[1], dlerror()); return 2; }
#define L(n) do { *(void **)&n = dlsym(h, "LLVM" #n); if (!n) { fprintf(stderr, "air_xlate_check: LLVM" #n " missing\n"); return 2; } } while (0)
	L(ContextCreate); L(ContextDispose); L(ContextSetDiagnosticHandler); L(CreateMemoryBufferWithMemoryRangeCopy);
	L(DisposeMemoryBuffer); L(ParseBitcodeInContext2); L(PrintModuleToString); L(DisposeModule); L(DisposeMessage);
	L(GetDiagInfoDescription); L(GetDiagInfoSeverity); L(RunPasses); L(CreatePassBuilderOptions);
	L(DisposePassBuilderOptions); L(GetErrorMessage); L(DisposeErrorMessage);
#undef L
	if (!nvmtl_llvm_walk_load(&walk, h)) { fprintf(stderr, "air_xlate_check: libLLVM lacks the IR-walk API\n"); return 2; }
	void *x = dlopen(argv[2], RTLD_NOW | RTLD_LOCAL);
	if (!x) { fprintf(stderr, "air_xlate_check: dlopen %s: %s\n", argv[2], dlerror()); return 2; }
	xlate_fn xlate = (xlate_fn)dlsym(x, "nvmtl_translate");
	xfree_fn xfree = (xfree_fn)dlsym(x, "nvmtl_translate_free");
	setcaps_fn setcaps = (setcaps_fn)dlsym(x, "nvmtl_translate_set_caps");
	if (!xlate || !xfree) { fprintf(stderr, "air_xlate_check: %s has no nvmtl_translate/_free\n", argv[2]); return 2; }
	if (setcaps) setcaps(3u | 4u);
	const char *out = argv[3];
	const int canonical = !strcmp(argv[4], "1");
	int bad = 0, total = 0;
	for (int i = 5; i < argc; i++, total++) {
		const char *base = strrchr(argv[i], '/') ? strrchr(argv[i], '/') + 1 : argv[i];
		char why[8192] = "";
		const char *stage = "?";
		FILE *f = fopen(argv[i], "rb");
		if (!f) { printf("FAIL %s ? cannot open\n", base); bad++; continue; }
		fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
		char *buf = malloc((size_t)n);
		if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(buf); printf("FAIL %s ? short read\n", base); bad++; continue; }
		fclose(f);
		void *ctx = ContextCreate();
		ContextSetDiagnosticHandler(ctx, diag, NULL);
		void *mb = CreateMemoryBufferWithMemoryRangeCopy(buf, (size_t)n, "nvmtl-air");
		void *mod = NULL;
		char *text = NULL;
		int unrolled = 0;
		g_error = 0;
		if (!mb || ParseBitcodeInContext2(ctx, mb, &mod) || !mod || g_error) {
			snprintf(why, sizeof why, "bitcode reader refused the module");
			goto done;
		}
		if (memmem(buf, (size_t)n, "air.simdgroup_matrix_8x8", 24)) {
			if (passes(mod, NVMTL_AIR_UNROLL_PIPELINE, why, sizeof why)) {
				fprintf(stderr, "  %s: air unroll FAILED (%s) - translating as decoded\n", base, why);   // plugin: logged, not fatal
				why[0] = 0;
			} else {
				fprintf(stderr, "  %s: air unroll: %d byte GEPs retyped\n", base, nvmtl_retype_byte_geps(&walk, mod));
				unrolled = 1;
			}
		}
		if (canonical) {
			unsigned helpers = nvmtl_inline_single_use_helpers(&walk, mod);
			if (passes(mod, NVMTL_AIR_CANON_PIPELINE, why, sizeof why)) goto done;
			if (nvmtl_has_pointer_choice(&walk, mod) && passes(mod, NVMTL_AIR_CHOICE_PIPELINE, why, sizeof why)) goto done;
			fprintf(stderr, "  %s: pointer canonicalization exposed %u single-use helpers\n", base, helpers);
			if (unrolled)
				fprintf(stderr, "  %s: %d byte GEPs retyped after canonicalization\n", base, nvmtl_retype_byte_geps(&walk, mod));
		}
		text = PrintModuleToString(mod);
		if (!text) { snprintf(why, sizeof why, "LLVMPrintModuleToString returned NULL"); goto done; }
		if (canonical) replace_all(text, "icmp samesign ", "icmp ");
		put(out, base, "ll", text, strlen(text));
		stage = stage_of(text);
		if (!stage) { stage = "?"; snprintf(why, sizeof why, "no !air.vertex/fragment/kernel entry"); goto done; }
		{
			uint8_t *spv = NULL; size_t len = 0; char e[4096] = "";
			if (xlate(text, stage, &spv, &len, e, sizeof e) || !spv) {
				snprintf(why, sizeof why, "translate: %s", e[0] ? e : "(no message)");
				put(out, base, "err", e, strlen(e));
			} else {
				put(out, base, "spv", spv, len);
				printf("ok   %s %s %zu -> %zu\n", base, stage, strlen(text), len);
				xfree(spv, len);
			}
		}
	done:
		if (why[0]) {
			for (char *c = why; *c; c++) if (*c == '\n') *c = ' ';
			printf("FAIL %s %s %s\n", base, stage, why);
			bad++;
		}
		if (text) DisposeMessage(text);
		if (mod) DisposeModule(mod);
		if (mb) DisposeMemoryBuffer(mb);
		ContextDispose(ctx);
		free(buf);
	}
	fprintf(stderr, "air_xlate_check: %d of %d modules translated\n", total - bad, total);
	return bad ? 1 : 0;
}
