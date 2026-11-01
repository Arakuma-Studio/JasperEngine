/*
 * dav1d's asm against its C functions on one architecture (tools/htmlui/dav1d-asm-check/run.sh
 * builds this for aarch64 and armv7 and runs it under QEMU; on x86-64 the engine's
 * Av1Decoder.AsmDecodesBitExactlyAsC test does the same). Decodes each IVF file twice, with dav1d's
 * CPU feature mask 0 (C only) and ~0 (every feature the CPU reports), and compares every plane of
 * every frame byte for byte. Prints the CPU features dav1d found and the time per frame both ways.
 * Exit 0: every file identical. dav1d must be built with TRIM_DSP_FUNCTIONS=0 (CMakeLists.txt
 * does): its default 1 leaves out the C functions of the baseline asm and keeps that asm on.
 *
 *   dav1d-asm-check clip.ivf [clip.ivf ...]
 */
#include <dav1d/dav1d.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void dav1d_set_cpu_flags_mask(unsigned mask);
extern unsigned dav1d_cpu_flags;

typedef struct {
    uint8_t* data;
    size_t size;
} Planes;

static double now(void){
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void keep(Planes* out, const Dav1dPicture* p){
    const int hbd = p->p.bpc > 8;
    const int ss_hor = p->p.layout == DAV1D_PIXEL_LAYOUT_I420 || p->p.layout == DAV1D_PIXEL_LAYOUT_I422;
    const int ss_ver = p->p.layout == DAV1D_PIXEL_LAYOUT_I420;
    const int bytes = hbd ? 2 : 1;
    const int cw = (p->p.w + ss_hor) >> ss_hor, ch = (p->p.h + ss_ver) >> ss_ver;
    size_t size = (size_t)p->p.w * p->p.h * bytes;
    if (p->p.layout != DAV1D_PIXEL_LAYOUT_I400)
        size += 2 * (size_t)cw * ch * bytes;
    out->data = realloc(out->data, out->size + size);
    uint8_t* dst = out->data + out->size;
    for (int y = 0; y < p->p.h; y++, dst += (size_t)p->p.w * bytes)
        memcpy(dst, (const uint8_t*)p->data[0] + y * p->stride[0], (size_t)p->p.w * bytes);
    if (p->p.layout != DAV1D_PIXEL_LAYOUT_I400){
        for (int plane = 1; plane <= 2; plane++){
            for (int y = 0; y < ch; y++, dst += (size_t)cw * bytes)
                memcpy(dst, (const uint8_t*)p->data[plane] + y * p->stride[1], (size_t)cw * bytes);
        }
    }
    out->size += size;
}

/* every frame of an IVF file, its planes appended to `out`; the frame count, -1 on an error */
static int decode(const char* path, unsigned mask, Planes* out, double* seconds){
    FILE* f = fopen(path, "rb");
    if (!f)
        return -1;
    uint8_t header[32];
    if (fread(header, 1, 32, f) != 32 || memcmp(header, "DKIF", 4)){
        fclose(f);
        return -1;
    }
    dav1d_set_cpu_flags_mask(mask);
    Dav1dSettings settings;
    dav1d_default_settings(&settings);
    settings.n_threads = 1;
    settings.max_frame_delay = 1;
    Dav1dContext* c = NULL;
    if (dav1d_open(&c, &settings) < 0){
        fclose(f);
        return -1;
    }
    int frames = 0;
    *seconds = 0.0;
    uint8_t frameHeader[12];
    while (fread(frameHeader, 1, 12, f) == 12){
        const uint32_t size = frameHeader[0] | frameHeader[1] << 8 | frameHeader[2] << 16 | (uint32_t)frameHeader[3] << 24;
        Dav1dData data;
        uint8_t* buffer = dav1d_data_create(&data, size);
        if (!buffer || fread(buffer, 1, size, f) != size)
            break;
        const double start = now();
        while (data.sz > 0){
            int r = dav1d_send_data(c, &data);
            if (r < 0 && r != DAV1D_ERR(EAGAIN))
                break;
            Dav1dPicture p = {0};
            r = dav1d_get_picture(c, &p);
            if (r == 0){
                keep(out, &p);
                dav1d_picture_unref(&p);
                frames++;
            }
        }
        *seconds += now() - start;
        dav1d_data_unref(&data);
    }
    for (;;){ /* the frames still in the decoder */
        Dav1dPicture p = {0};
        if (dav1d_get_picture(c, &p) != 0)
            break;
        keep(out, &p);
        dav1d_picture_unref(&p);
        frames++;
    }
    dav1d_close(&c);
    fclose(f);
    dav1d_set_cpu_flags_mask(~0u);
    return frames;
}

int main(int argc, char** argv){
    int failed = 0;
    for (int i = 1; i < argc; i++){
        Planes cOnly = {0}, withAsm = {0};
        double cSeconds = 0.0, asmSeconds = 0.0;
        const int cFrames = decode(argv[i], 0u, &cOnly, &cSeconds);
        const int asmFrames = decode(argv[i], ~0u, &withAsm, &asmSeconds);
        const int same = cFrames > 0 && cFrames == asmFrames && cOnly.size == withAsm.size && !memcmp(cOnly.data, withAsm.data, cOnly.size);
        printf("%s %s: %d frames, %zu bytes of planes; C %.2f ms/frame, asm %.2f ms/frame (x%.2f)\n", same ? "SAME" : "DIFF", argv[i],
               cFrames, cOnly.size, cFrames > 0 ? cSeconds * 1000.0 / cFrames : 0.0, asmFrames > 0 ? asmSeconds * 1000.0 / asmFrames : 0.0,
               asmSeconds > 0.0 ? cSeconds / asmSeconds : 0.0);
        failed |= !same;
        free(cOnly.data);
        free(withAsm.data);
    }
    printf("dav1d CPU features: 0x%x\n", dav1d_cpu_flags);
    return failed;
}
