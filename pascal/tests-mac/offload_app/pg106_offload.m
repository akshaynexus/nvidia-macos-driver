/* SPDX-License-Identifier: MIT */
/*
 * pg106_offload - OWNER-RUNBOOK.md step 4: the GP106 renders, the Intel
 * HD 630 composites (research/evidence/OFFLOAD-DESIGN.md sections 1-2).
 *
 * A 640x400 window on the internal panel shows frames the GP106 rendered
 * into IOSurfaces. WindowServer runs on the Intel GPU and reads the frames
 * from the IOSurfaces in system memory. No display is driven by the GP106.
 *
 * Modes:
 *   --vulkan (default)  NVK renders straight into the IOSurface's pages
 *                       (VK_EXT_external_memory_host: zero-copy) or into a
 *                       staging buffer that is copied in (--copy, or when
 *                       host import is unavailable). No Metal involved, so
 *                       this works without the PG106Accel accelerator.
 *   --metal             The same through Metal: MTLCopyAllDevices must list
 *                       the GP106 (published by PG106Accel + NVMTLDriver;
 *                       OFFLOAD-DESIGN.md section 3.3). Clear + triangle into
 *                       an IOSurface-backed texture.
 *   --list-metal        Print every MTLDevice and exit.
 *
 * Per frame, in both modes: lock the surface, render, wait for completion,
 * check a pixel grid on the CPU, unlock (the unlock of a write lock bumps the
 * IOSurface seed), then hand the surface to the layer. That is the section 2.3
 * ordering: completion is observed before the frame is published; no fence
 * travels with the surface.
 *
 * Options: --seconds N (10), --copy, --driver PATH, --any (any Vulkan device)
 * Exit: 0 all frames correct, 1 a wrong frame or a GPU error, 2 no GP106.
 */
#import <Cocoa/Cocoa.h>
#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#include <mach/mach_time.h>
#include <string.h>

#include "offload_vk.h"

#define W 640u
#define H 400u
#define NSURF 2

static struct {
   int metal, list_metal, force_copy, any;
   double seconds;
   const char *driver;
} opt = { .seconds = 10.0, .driver = "/Library/GPUBundles/nvmtl/libvulkan_nouveau.dylib" };

static IOSurfaceRef surf[NSURF];
static CALayer *layer;
static uint32_t frame_no, bad_frames, gpu_errors;
static uint64_t gpu_sum_ns, gpu_worst_ns, copy_sum_ns, present_sum_ns;
static uint32_t seed_bumps;

/* Vulkan */
static struct ovk *ovk;
static uint32_t vk_index[NSURF];

/* Metal */
static id<MTLDevice> mdev;
static id<MTLCommandQueue> mqueue;
static id<MTLRenderPipelineState> mpipe;
static id<MTLTexture> mtex[NSURF];

static uint64_t
now_ns(void)
{
   return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

static IOSurfaceRef
make_surface(void)
{
   const size_t bpr = IOSurfaceAlignProperty(kIOSurfaceBytesPerRow, W * 4);
   const size_t size = IOSurfaceAlignProperty(kIOSurfaceAllocSize, bpr * H);
   NSDictionary *props = @{
      (id)kIOSurfaceWidth: @(W), (id)kIOSurfaceHeight: @(H),
      (id)kIOSurfaceBytesPerElement: @4, (id)kIOSurfaceBytesPerRow: @(bpr),
      (id)kIOSurfaceAllocSize: @(size), (id)kIOSurfacePixelFormat: @((uint32_t)'BGRA'),
   };
   return IOSurfaceCreate((__bridge CFDictionaryRef)props);
}

/* ---- Metal ----------------------------------------------------------- */

static void
list_metal(void)
{
   NSArray<id<MTLDevice>> *all = MTLCopyAllDevices();
   printf("metal devices: %lu\n", (unsigned long)all.count);
   for (id<MTLDevice> d in all) {
      printf("  %-40s registryID 0x%llx lowPower %d headless %d removable %d location %lu\n",
             d.name.UTF8String, (unsigned long long)d.registryID, d.lowPower, d.headless,
             d.removable, (unsigned long)d.location);
   }
}

static id<MTLDevice>
pick_metal(void)
{
   const char *want = getenv("PG106_METAL_DEVICE");
   for (id<MTLDevice> d in MTLCopyAllDevices()) {
      const char *n = d.name.UTF8String;
      if (want ? strstr(n, want) != NULL : (strstr(n, "NVIDIA") || strstr(n, "1060") || strstr(n, "1050")))
         return d;
   }
   return nil;
}

static NSString *const kShader =
   @"#include <metal_stdlib>\n"
   "using namespace metal;\n"
   "struct V { float4 pos [[position]]; };\n"
   "vertex V vs(uint id [[vertex_id]]) {\n"
   "  const float2 p[3] = { float2(-0.6, -0.6), float2(0.6, -0.6), float2(0.0, 0.7) };\n"
   "  V v; v.pos = float4(p[id], 0.0, 1.0); return v;\n"
   "}\n"
   "fragment float4 fs(V in [[stage_in]], constant float &t [[buffer(0)]]) {\n"
   "  return float4(1.0, t, 0.0, 1.0);\n"
   "}\n";

static int
metal_setup(void)
{
   mdev = pick_metal();
   if (!mdev) {
      printf("FAIL no GP106 MTLDevice. Metal lists only accelerators that publish MetalPluginName;\n"
             "     for the GP106 that is PG106Accel (OFFLOAD-DESIGN.md section 3.3). Use --vulkan.\n");
      list_metal();
      return 2;
   }
   printf("metal device: %s (registryID 0x%llx, headless %d)\n", mdev.name.UTF8String,
          (unsigned long long)mdev.registryID, mdev.headless);
   mqueue = [mdev newCommandQueue];
   NSError *err = nil;
   id<MTLLibrary> lib = [mdev newLibraryWithSource:kShader options:nil error:&err];
   if (!lib) { printf("FAIL shader: %s\n", err.description.UTF8String); return 1; }
   MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
   pd.vertexFunction = [lib newFunctionWithName:@"vs"];
   pd.fragmentFunction = [lib newFunctionWithName:@"fs"];
   pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
   mpipe = [mdev newRenderPipelineStateWithDescriptor:pd error:&err];
   if (!mpipe) { printf("FAIL pipeline: %s\n", err.description.UTF8String); return 1; }
   for (int i = 0; i < NSURF; i++) {
      MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
                                  MTLPixelFormatBGRA8Unorm width:W height:H mipmapped:NO];
      td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
      td.storageMode = MTLStorageModeManaged;
      mtex[i] = [mdev newTextureWithDescriptor:td iosurface:surf[i] plane:0];
      if (!mtex[i]) { printf("FAIL IOSurface texture %d\n", i); return 1; }
   }
   return 0;
}

/* The clear colour of frame f, as BGRA8: r = f*4, g = 0x40, b = 0x80. */
static void
metal_clear(uint32_t f, double rgba[4])
{
   rgba[0] = ((f * 4u) & 0xffu) / 255.0; rgba[1] = 0x40 / 255.0; rgba[2] = 0x80 / 255.0; rgba[3] = 1.0;
}

static int
near8(uint32_t a, uint32_t b)
{
   for (int s = 0; s < 32; s += 8) {
      int d = (int)((a >> s) & 0xff) - (int)((b >> s) & 0xff);
      if (d < -2 || d > 2) return 0;
   }
   return 1;
}

static int
metal_frame(int i, uint32_t f, uint64_t *gpu_ns)
{
   double c[4];
   metal_clear(f, c);
   const float t = (float)((f % 64u) / 63.0);
   MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
   rp.colorAttachments[0].texture = mtex[i];
   rp.colorAttachments[0].loadAction = MTLLoadActionClear;
   rp.colorAttachments[0].storeAction = MTLStoreActionStore;
   rp.colorAttachments[0].clearColor = MTLClearColorMake(c[0], c[1], c[2], c[3]);
   id<MTLCommandBuffer> cb = [mqueue commandBuffer];
   id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
   [enc setRenderPipelineState:mpipe];
   [enc setFragmentBytes:&t length:sizeof t atIndex:0];
   [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
   [enc endEncoding];
   id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
   [bl synchronizeResource:mtex[i]];   /* managed: make the CPU view current */
   [bl endEncoding];
   const uint64_t t0 = now_ns();
   [cb commit];
   [cb waitUntilCompleted];
   *gpu_ns = now_ns() - t0;
   if (cb.status != MTLCommandBufferStatusCompleted) {
      printf("FAIL frame %u: command buffer status %ld error %s\n", f, (long)cb.status,
             cb.error.description.UTF8String);
      return -1;
   }
   /* corner = clear colour, centre = triangle (1, t, 0) */
   IOSurfaceLock(surf[i], kIOSurfaceLockReadOnly, NULL);
   const uint32_t *p = IOSurfaceGetBaseAddress(surf[i]);
   const size_t pitch = IOSurfaceGetBytesPerRow(surf[i]) / 4;
   const uint32_t corner = p[2 * pitch + 2];
   const uint32_t centre = p[(H / 2) * pitch + W / 2];
   IOSurfaceUnlock(surf[i], kIOSurfaceLockReadOnly, NULL);
   const uint32_t want_corner = 0xff000000u | (((f * 4u) & 0xffu) << 16) | (0x40u << 8) | 0x80u;
   const uint32_t want_centre = 0xff000000u | (0xffu << 16) | ((uint32_t)(t * 255.0f + 0.5f) << 8);
   return (near8(corner, want_corner) && near8(centre, want_centre)) ? 0 : 1;
}

/* ---- per-frame ------------------------------------------------------- */

static void
tick(void)
{
   const int i = (int)(frame_no % NSURF);
   const uint32_t seed0 = IOSurfaceGetSeed(surf[i]);
   uint64_t gpu = 0, cpy = 0;
   int bad;

   uint32_t seed1 = 0;
   if (opt.metal) {
      /* The Metal plugin owns the surface while it renders and publishes it
       * itself (copy-out + unlock, plugin/NVMTLObjects.m nvmtlSurfaceOut);
       * only read it here, after completion. */
      bad = metal_frame(i, frame_no, &gpu);
      seed1 = IOSurfaceGetSeed(surf[i]);
   } else {
      /* The app is the writer: hold a write lock while the GPU fills the
       * pages, release it once the fence has signalled. */
      IOSurfaceLock(surf[i], 0, NULL);
      bad = ovk_frame(ovk, vk_index[i], frame_no, &gpu, &cpy) != 0 ? -1
          : ovk_verify(IOSurfaceGetBaseAddress(surf[i]), W, H,
                       (uint32_t)IOSurfaceGetBytesPerRow(surf[i]), frame_no) != 0;
      IOSurfaceUnlock(surf[i], 0, &seed1);
   }
   seed_bumps += seed1 != seed0;

   if (bad < 0) gpu_errors++;
   else if (bad) bad_frames++;
   gpu_sum_ns += gpu;
   copy_sum_ns += cpy;
   if (gpu > gpu_worst_ns) gpu_worst_ns = gpu;

   const uint64_t t0 = now_ns();
   [CATransaction begin];
   [CATransaction setDisableActions:YES];
   layer.contents = (__bridge id)surf[i];
   [CATransaction commit];
   present_sum_ns += now_ns() - t0;
   frame_no++;
}

static void
finish(void)
{
   const double n = frame_no ? frame_no : 1;
   printf("frames %u, wrong %u, gpu errors %u, seed bumped on %u of %u unlocks\n",
          frame_no, bad_frames, gpu_errors, seed_bumps, frame_no);
   printf("gpu (submit->complete) mean %.3f ms worst %.3f ms, copy-in mean %.3f ms, publish mean %.3f ms\n",
          gpu_sum_ns / n / 1e6, gpu_worst_ns / 1e6, copy_sum_ns / n / 1e6, present_sum_ns / n / 1e6);
   const int ok = frame_no > 0 && bad_frames == 0 && gpu_errors == 0;
   printf("pg106_offload: %s\n", ok ? "PASS" : "FAIL");
   exit(ok ? 0 : 1);
}

@interface Delegate : NSObject <NSApplicationDelegate>
@end
@implementation Delegate
- (void)applicationDidFinishLaunching:(NSNotification *)n {
   NSWindow *win = [[NSWindow alloc] initWithContentRect:NSMakeRect(120, 120, W / 2.0, H / 2.0)
      styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
      backing:NSBackingStoreBuffered defer:NO];
   win.title = opt.metal ? @"pg106_offload (Metal on GP106)" : @"pg106_offload (NVK on GP106)";
   NSView *v = win.contentView;
   v.wantsLayer = YES;
   layer = v.layer;
   layer.contentsGravity = kCAGravityResize;
   [win makeKeyAndOrderFront:nil];
   [NSApp activateIgnoringOtherApps:YES];
   const uint64_t end = now_ns() + (uint64_t)(opt.seconds * 1e9);
   [NSTimer scheduledTimerWithTimeInterval:1.0 / 60.0 repeats:YES block:^(NSTimer *t) {
      if (now_ns() >= end) { [t invalidate]; finish(); }
      tick();
   }];
}
@end

int
main(int argc, char **argv)
{
   for (int i = 1; i < argc; i++) {
      if (!strcmp(argv[i], "--metal")) opt.metal = 1;
      else if (!strcmp(argv[i], "--vulkan")) opt.metal = 0;
      else if (!strcmp(argv[i], "--list-metal")) opt.list_metal = 1;
      else if (!strcmp(argv[i], "--copy")) opt.force_copy = 1;
      else if (!strcmp(argv[i], "--any")) opt.any = 1;
      else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) opt.seconds = atof(argv[++i]);
      else if (!strcmp(argv[i], "--driver") && i + 1 < argc) opt.driver = argv[++i];
      else { fprintf(stderr, "usage: %s [--vulkan|--metal|--list-metal] [--seconds N] [--copy] [--driver PATH] [--any]\n", argv[0]); return 2; }
   }
   setvbuf(stdout, NULL, _IOLBF, 0);
   @autoreleasepool {
      if (opt.list_metal) { list_metal(); return 0; }
      for (int i = 0; i < NSURF; i++) {
         surf[i] = make_surface();
         if (!surf[i]) { printf("FAIL IOSurfaceCreate\n"); return 1; }
      }
      printf("surfaces: %d x %ux%u BGRA, %zu bytes per row, alloc %zu, ids %u %u\n", NSURF, W, H,
             IOSurfaceGetBytesPerRow(surf[0]), IOSurfaceGetAllocSize(surf[0]),
             IOSurfaceGetID(surf[0]), IOSurfaceGetID(surf[1]));
      if (opt.metal) {
         int rc = metal_setup();
         if (rc) return rc;
      } else {
         int rc = ovk_open(&ovk, opt.driver, 1, opt.any);
         if (rc) return rc == -2 ? 2 : 1;
         for (int i = 0; i < NSURF; i++) {
            /* The base address is stable for a system-memory IOSurface; take
             * it under a lock once and import those pages. */
            IOSurfaceLock(surf[i], 0, NULL);
            rc = ovk_attach(ovk, IOSurfaceGetBaseAddress(surf[i]), IOSurfaceGetAllocSize(surf[i]),
                            W, H, (uint32_t)IOSurfaceGetBytesPerRow(surf[i]), opt.force_copy,
                            &vk_index[i]);
            IOSurfaceUnlock(surf[i], 0, NULL);
            if (rc) { printf("FAIL attaching surface %d\n", i); return 1; }
         }
         printf("vulkan device: %s, mode: %s\n", ovk_device_name(ovk), ovk_mode(ovk, vk_index[0]));
      }
      NSApplication *app = [NSApplication sharedApplication];
      app.activationPolicy = NSApplicationActivationPolicyRegular;
      Delegate *d = [Delegate new];
      app.delegate = d;
      [app run];
   }
   return 0;
}
