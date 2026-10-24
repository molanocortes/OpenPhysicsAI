/* image.c - PNG writer using CoreGraphics + ImageIO (C APIs) */
#include "image.h"
#include "common.h"

#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>

bool image_write_png(const char *path, int w, int h, const uint8_t *rgba, bool flip_y) {
    size_t row = (size_t)w * 4;
    uint8_t *buf = malloc(row * (size_t)h);
    if (!buf) return false;
    for (int y = 0; y < h; y++) {
        const uint8_t *src = rgba + (size_t)(flip_y ? h - 1 - y : y) * row;
        memcpy(buf + (size_t)y * row, src, row);
    }
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef ctx = CGBitmapContextCreate(buf, (size_t)w, (size_t)h, 8, row, cs, kCGImageAlphaNoneSkipLast);
    bool ok = false;
    if (ctx) {
        CGImageRef img = CGBitmapContextCreateImage(ctx);
        CFStringRef cfpath = CFStringCreateWithCString(NULL, path, kCFStringEncodingUTF8);
        CFURLRef url = CFURLCreateWithFileSystemPath(NULL, cfpath, kCFURLPOSIXPathStyle, false);
        CGImageDestinationRef dst = CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, NULL);
        if (dst && img) {
            CGImageDestinationAddImage(dst, img, NULL);
            ok = CGImageDestinationFinalize(dst);
        }
        if (dst) CFRelease(dst);
        CFRelease(url);
        CFRelease(cfpath);
        if (img) CGImageRelease(img);
        CGContextRelease(ctx);
    }
    CGColorSpaceRelease(cs);
    free(buf);
    return ok;
}

/* any image ImageIO reads (PNG, JPEG, ...), as w*h*4 RGBA bytes, top row first, alpha premultiplied; free() it */
uint8_t *image_read(const char *path, int *w, int *h) {
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, (CFIndex)strlen(path), false);
    CGImageSourceRef src = url ? CGImageSourceCreateWithURL(url, NULL) : NULL;
    if (url) CFRelease(url);
    CGImageRef img = src ? CGImageSourceCreateImageAtIndex(src, 0, NULL) : NULL;
    if (src) CFRelease(src);
    if (!img) return NULL;
    *w = (int)CGImageGetWidth(img), *h = (int)CGImageGetHeight(img);
    uint8_t *px = calloc((size_t)*w * (size_t)*h, 4);
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = px ? CGBitmapContextCreate(px, (size_t)*w, (size_t)*h, 8, (size_t)*w * 4, cs, kCGImageAlphaPremultipliedLast) : NULL;
    if (ctx) CGContextDrawImage(ctx, CGRectMake(0, 0, *w, *h), img), CGContextRelease(ctx);
    CGColorSpaceRelease(cs);
    CGImageRelease(img);
    if (!ctx) { free(px); return NULL; }
    return px;
}
