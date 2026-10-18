// Copyright (c) 2026 Ch1nguya
// Part of the DKR-R Wii port. Distributed under the MIT License.
// Derived in part from DKR-R (MIT licence, (c) DKR-R contributors); see CREDITS.md.
#include "native_audio_mixer.hpp"

#include <cstring>
#include <cstdio>
#include <algorithm>

// librecomp / ultramodern includes for OSTask definition
#include "librecomp/rsp.hpp"

namespace {

// Alignment and clamping helpers
static inline uint32_t align_up(uint32_t val, uint32_t alignment) {
    const uint32_t mask = alignment - 1;
    return (val + mask) & ~mask;
}

static inline int16_t clamp_s16(int32_t val) {
    if (val < -32768) return -32768;
    if (val > 32767)  return 32767;
    return static_cast<int16_t>(val);
}

// Memory helpers for RDRAM (N64 physical address space)
static inline uint16_t rdram_read_u16(const uint8_t* rdram, uint32_t addr) {
    addr &= 0x00FFFFFF;
#if defined(GEKKO)
    return *reinterpret_cast<const uint16_t*>(rdram + addr);
#else
    return static_cast<uint16_t>((static_cast<uint16_t>(rdram[addr]) << 8) | rdram[addr + 1]);
#endif
}

static inline int16_t rdram_read_s16(const uint8_t* rdram, uint32_t addr) {
    return static_cast<int16_t>(rdram_read_u16(rdram, addr));
}

static inline uint32_t rdram_read_u32(const uint8_t* rdram, uint32_t addr) {
    addr &= 0x00FFFFFF;
#if defined(GEKKO)
    return *reinterpret_cast<const uint32_t*>(rdram + addr);
#else
    return (static_cast<uint32_t>(rdram[addr]) << 24) |
           (static_cast<uint32_t>(rdram[addr + 1]) << 16) |
           (static_cast<uint32_t>(rdram[addr + 2]) << 8) |
           static_cast<uint32_t>(rdram[addr + 3]);
#endif
}

static inline int32_t rdram_read_s32(const uint8_t* rdram, uint32_t addr) {
    return static_cast<int32_t>(rdram_read_u32(rdram, addr));
}

static inline void rdram_write_u16(uint8_t* rdram, uint32_t addr, uint16_t val) {
    addr &= 0x00FFFFFF;
#if defined(GEKKO)
    *reinterpret_cast<uint16_t*>(rdram + addr) = val;
#else
    rdram[addr]     = static_cast<uint8_t>(val >> 8);
    rdram[addr + 1] = static_cast<uint8_t>(val);
#endif
}

static inline void rdram_write_s16(uint8_t* rdram, uint32_t addr, int16_t val) {
    rdram_write_u16(rdram, addr, static_cast<uint16_t>(val));
}

static inline void rdram_write_u32(uint8_t* rdram, uint32_t addr, uint32_t val) {
    addr &= 0x00FFFFFF;
#if defined(GEKKO)
    *reinterpret_cast<uint32_t*>(rdram + addr) = val;
#else
    rdram[addr]     = static_cast<uint8_t>(val >> 24);
    rdram[addr + 1] = static_cast<uint8_t>(val >> 16);
    rdram[addr + 2] = static_cast<uint8_t>(val >> 8);
    rdram[addr + 3] = static_cast<uint8_t>(val);
#endif
}

static inline void rdram_write_s32(uint8_t* rdram, uint32_t addr, int32_t val) {
    rdram_write_u32(rdram, addr, static_cast<uint32_t>(val));
}

static void rdram_read_block(const uint8_t* rdram, uint32_t addr, void* dst, size_t size) {
    addr &= 0x00FFFFFF;
#if defined(GEKKO)
    std::memcpy(dst, rdram + addr, size);
#else
    const uint8_t* src = rdram + addr;
    uint8_t* d = static_cast<uint8_t*>(dst);
    for (size_t i = 0; i < size; i += 2) {
        d[i + 0] = src[i + 0];
        d[i + 1] = src[i + 1];
    }
#endif
}

static void rdram_write_block(uint8_t* rdram, uint32_t addr, const void* src, size_t size) {
    addr &= 0x00FFFFFF;
#if defined(GEKKO)
    std::memcpy(rdram + addr, src, size);
#else
    uint8_t* d = rdram + addr;
    const uint8_t* s = static_cast<const uint8_t*>(src);
    for (size_t i = 0; i < size; i += 2) {
        d[i + 0] = s[i + 0];
        d[i + 1] = s[i + 1];
    }
#endif
}

// 64 x 4 tap filter coefficients used by the SGI RSP resampler
static const int16_t RESAMPLE_LUT[64 * 4] = {
    (int16_t)0x0c39, (int16_t)0x66ad, (int16_t)0x0d46, (int16_t)0xffdf,
    (int16_t)0x0b39, (int16_t)0x6696, (int16_t)0x0e5f, (int16_t)0xffd8,
    (int16_t)0x0a44, (int16_t)0x6669, (int16_t)0x0f83, (int16_t)0xffd0,
    (int16_t)0x095a, (int16_t)0x6626, (int16_t)0x10b4, (int16_t)0xffc8,
    (int16_t)0x087d, (int16_t)0x65cd, (int16_t)0x11f0, (int16_t)0xffbf,
    (int16_t)0x07ab, (int16_t)0x655e, (int16_t)0x1338, (int16_t)0xffb6,
    (int16_t)0x06e4, (int16_t)0x64d9, (int16_t)0x148c, (int16_t)0xffac,
    (int16_t)0x0628, (int16_t)0x643f, (int16_t)0x15eb, (int16_t)0xffa1,
    (int16_t)0x0577, (int16_t)0x638f, (int16_t)0x1756, (int16_t)0xff96,
    (int16_t)0x04d1, (int16_t)0x62cb, (int16_t)0x18cb, (int16_t)0xff8a,
    (int16_t)0x0435, (int16_t)0x61f3, (int16_t)0x1a4c, (int16_t)0xff7e,
    (int16_t)0x03a4, (int16_t)0x6106, (int16_t)0x1bd7, (int16_t)0xff71,
    (int16_t)0x031c, (int16_t)0x6007, (int16_t)0x1d6c, (int16_t)0xff64,
    (int16_t)0x029f, (int16_t)0x5ef5, (int16_t)0x1f0b, (int16_t)0xff56,
    (int16_t)0x022a, (int16_t)0x5dd0, (int16_t)0x20b3, (int16_t)0xff48,
    (int16_t)0x01be, (int16_t)0x5c9a, (int16_t)0x2264, (int16_t)0xff3a,
    (int16_t)0x015b, (int16_t)0x5b53, (int16_t)0x241e, (int16_t)0xff2c,
    (int16_t)0x0101, (int16_t)0x59fc, (int16_t)0x25e0, (int16_t)0xff1e,
    (int16_t)0x00ae, (int16_t)0x5896, (int16_t)0x27a9, (int16_t)0xff10,
    (int16_t)0x0063, (int16_t)0x5720, (int16_t)0x297a, (int16_t)0xff02,
    (int16_t)0x001f, (int16_t)0x559d, (int16_t)0x2b50, (int16_t)0xfef4,
    (int16_t)0xffe2, (int16_t)0x540d, (int16_t)0x2d2c, (int16_t)0xfee8,
    (int16_t)0xffac, (int16_t)0x5270, (int16_t)0x2f0d, (int16_t)0xfedb,
    (int16_t)0xff7c, (int16_t)0x50c7, (int16_t)0x30f3, (int16_t)0xfed0,
    (int16_t)0xff53, (int16_t)0x4f14, (int16_t)0x32dc, (int16_t)0xfec6,
    (int16_t)0xff2e, (int16_t)0x4d57, (int16_t)0x34c8, (int16_t)0xfebd,
    (int16_t)0xff0f, (int16_t)0x4b91, (int16_t)0x36b6, (int16_t)0xfeb6,
    (int16_t)0xfef5, (int16_t)0x49c2, (int16_t)0x38a5, (int16_t)0xfeb0,
    (int16_t)0xfedf, (int16_t)0x47ed, (int16_t)0x3a95, (int16_t)0xfeac,
    (int16_t)0xfece, (int16_t)0x4611, (int16_t)0x3c85, (int16_t)0xfeab,
    (int16_t)0xfec0, (int16_t)0x4430, (int16_t)0x3e74, (int16_t)0xfeac,
    (int16_t)0xfeb6, (int16_t)0x424a, (int16_t)0x4060, (int16_t)0xfeaf,
    (int16_t)0xfeaf, (int16_t)0x4060, (int16_t)0x424a, (int16_t)0xfeb6,
    (int16_t)0xfeac, (int16_t)0x3e74, (int16_t)0x4430, (int16_t)0xfec0,
    (int16_t)0xfeab, (int16_t)0x3c85, (int16_t)0x4611, (int16_t)0xfece,
    (int16_t)0xfeac, (int16_t)0x3a95, (int16_t)0x47ed, (int16_t)0xfedf,
    (int16_t)0xfeb0, (int16_t)0x38a5, (int16_t)0x49c2, (int16_t)0xfef5,
    (int16_t)0xfeb6, (int16_t)0x36b6, (int16_t)0x4b91, (int16_t)0xff0f,
    (int16_t)0xfebd, (int16_t)0x34c8, (int16_t)0x4d57, (int16_t)0xff2e,
    (int16_t)0xfec6, (int16_t)0x32dc, (int16_t)0x4f14, (int16_t)0xff53,
    (int16_t)0xfed0, (int16_t)0x30f3, (int16_t)0x50c7, (int16_t)0xff7c,
    (int16_t)0xfedb, (int16_t)0x2f0d, (int16_t)0x5270, (int16_t)0xffac,
    (int16_t)0xfee8, (int16_t)0x2d2c, (int16_t)0x540d, (int16_t)0xffe2,
    (int16_t)0xfef4, (int16_t)0x2b50, (int16_t)0x559d, (int16_t)0x001f,
    (int16_t)0xff02, (int16_t)0x297a, (int16_t)0x5720, (int16_t)0x0063,
    (int16_t)0xff10, (int16_t)0x27a9, (int16_t)0x5896, (int16_t)0x00ae,
    (int16_t)0xff1e, (int16_t)0x25e0, (int16_t)0x59fc, (int16_t)0x0101,
    (int16_t)0xff2c, (int16_t)0x241e, (int16_t)0x5b53, (int16_t)0x015b,
    (int16_t)0xff3a, (int16_t)0x2264, (int16_t)0x5c9a, (int16_t)0x01be,
    (int16_t)0xff48, (int16_t)0x20b3, (int16_t)0x5dd0, (int16_t)0x022a,
    (int16_t)0xff56, (int16_t)0x1f0b, (int16_t)0x5ef5, (int16_t)0x029f,
    (int16_t)0xff64, (int16_t)0x1d6c, (int16_t)0x6007, (int16_t)0x031c,
    (int16_t)0xff71, (int16_t)0x1bd7, (int16_t)0x6106, (int16_t)0x03a4,
    (int16_t)0xff7e, (int16_t)0x1a4c, (int16_t)0x61f3, (int16_t)0x0435,
    (int16_t)0xff8a, (int16_t)0x18cb, (int16_t)0x62cb, (int16_t)0x04d1,
    (int16_t)0xff96, (int16_t)0x1756, (int16_t)0x638f, (int16_t)0x0577,
    (int16_t)0xffa1, (int16_t)0x15eb, (int16_t)0x643f, (int16_t)0x0628,
    (int16_t)0xffac, (int16_t)0x148c, (int16_t)0x64d9, (int16_t)0x06e4,
    (int16_t)0xffb6, (int16_t)0x1338, (int16_t)0x655e, (int16_t)0x07ab,
    (int16_t)0xffbf, (int16_t)0x11f0, (int16_t)0x65cd, (int16_t)0x087d,
    (int16_t)0xffc8, (int16_t)0x10b4, (int16_t)0x6626, (int16_t)0x095a,
    (int16_t)0xffd0, (int16_t)0x0f83, (int16_t)0x6669, (int16_t)0x0a44,
    (int16_t)0xffd8, (int16_t)0x0e5f, (int16_t)0x6696, (int16_t)0x0b39,
    (int16_t)0xffdf, (int16_t)0x0d46, (int16_t)0x66ad, (int16_t)0x0c39
};

static inline int32_t rdot(size_t n, const int16_t *x, const int16_t *y) {
    int32_t accu = 0;
    y += n;
    while (n != 0) {
        accu += static_cast<int32_t>(*x++) * static_cast<int32_t>(*(--y));
        --n;
    }
    return accu;
}

static void adpcm_compute_residuals(int16_t* dst, const int16_t* src,
                                    const int16_t* cb_entry, const int16_t* last_samples, size_t count) {
    const int16_t* const book1 = cb_entry;
    const int16_t* const book2 = cb_entry + 8;
    const int16_t l1 = last_samples[0];
    const int16_t l2 = last_samples[1];

    for (size_t i = 0; i < count; ++i) {
        int32_t accu = static_cast<int32_t>(src[i]) << 11;
        accu += static_cast<int32_t>(book1[i]) * l1 +
                static_cast<int32_t>(book2[i]) * l2 +
                rdot(i, book2, src);
        dst[i] = clamp_s16(accu >> 11);
    }
}

static inline int16_t adpcm_predict_sample(uint8_t byte, uint8_t mask, unsigned lshift, unsigned rshift) {
    int16_t sample = static_cast<int16_t>(static_cast<uint16_t>(byte & mask) << lshift);
    sample >>= rshift;
    return sample;
}

// ABI command IDs
enum AudioCmd : uint8_t {
    A_SPNOOP     = 0,
    A_ADPCM      = 1,
    A_CLEARBUFF  = 2,
    A_ENVMIXER   = 3,
    A_LOADBUFF   = 4,
    A_RESAMPLE   = 5,
    A_SAVEBUFF   = 6,
    A_SEGMENT    = 7,
    A_SETBUFF    = 8,
    A_SETVOL     = 9,
    A_DMEMMOVE   = 10,
    A_LOADADPCM  = 11,
    A_MIXER      = 12,
    A_INTERLEAVE = 13,
    A_POLEF      = 14,
    A_SETLOOP    = 15
};

// ABI flags
enum AudioFlag : uint8_t {
    A_INIT     = 0x01,
    A_CONTINUE = 0x00,
    A_LOOP     = 0x02,
    A_LEFT     = 0x02,
    A_RIGHT    = 0x00,
    A_VOL      = 0x04,
    A_RATE     = 0x00,
    A_AUX      = 0x08
};

enum { DMEM_BASE = 0x5C0 };

struct Ramp {
    int64_t value;
    int64_t step;
    int64_t target;
};

static inline int16_t ramp_step(Ramp* ramp) {
    ramp->value += ramp->step;
    const bool target_reached = (ramp->step <= 0)
        ? (ramp->value <= ramp->target)
        : (ramp->value >= ramp->target);

    if (target_reached) {
        ramp->value = ramp->target;
        ramp->step  = 0;
    }
    return static_cast<int16_t>(ramp->value >> 16);
}

static inline void sample_mix(int16_t* dst, int16_t src, int16_t gain) {
    *dst = clamp_s16(*dst + ((static_cast<int32_t>(src) * gain) >> 15));
}

struct HleAudioState {
    // 4 KB DMEM + padding so that loops starting near the end can never overrun.
    uint8_t  dmem[4096 + 2048];
    uint32_t segments[16];
    int16_t  adpcm_table[128]; // Codebook loaded by A_LOADADPCM
    uint32_t loop_address;

    // Buffer parameters
    uint16_t in_offset;
    uint16_t out_offset;
    uint16_t count;
    uint16_t dry_right;
    uint16_t wet_left;
    uint16_t wet_right;

    int16_t dry;
    int16_t wet;
    int16_t vol[2];
    int16_t target[2];
    int32_t rate[2];

    uint32_t get_address(uint32_t so) const {
        const uint8_t seg = static_cast<uint8_t>(so >> 24) & 0x0F;
        const uint32_t offset = so & 0x00FFFFFF;
        return segments[seg] + offset;
    }

    int16_t* dmem_s16(uint16_t offset) {
        return reinterpret_cast<int16_t*>(dmem + (offset & 0xFFF));
    }

    const int16_t* dmem_s16(uint16_t offset) const {
        return reinterpret_cast<const int16_t*>(dmem + (offset & 0xFFF));
    }
};

} // namespace

bool dkr_native_audio_process(std::uint8_t* rdram, const OSTask* task) {
#if !defined(GEKKO)
    // Only validated on the big-endian Wii build (RECOMP_BYTE_XOR == 0).
    // Other hosts keep the recompiled aspMain microcode.
    (void)rdram; (void)task;
    return false;
#endif
    if (rdram == nullptr || task == nullptr) {
        return false;
    }

    const uint32_t data_ptr = static_cast<uint32_t>(task->t.data_ptr) & 0x00FFFFFF;
    const uint32_t data_size = static_cast<uint32_t>(task->t.data_size);

    if (data_size == 0) {
        return true; // Nothing to execute
    }

    HleAudioState state{};
    std::memset(&state, 0, sizeof(state));

    const uint32_t num_cmds = data_size / 8;
    const uint8_t* cmd_stream = rdram + data_ptr;

    // Pre-scan: refuse the whole list (-> recompiled microcode) rather than
    // falling back halfway, which would apply RDRAM state updates twice.
    for (uint32_t i = 0; i < num_cmds; ++i) {
        if (((rdram_read_u32(cmd_stream, i * 8) >> 24) & 0x7F) > A_SETLOOP) {
            return false;
        }
    }

    for (uint32_t i = 0; i < num_cmds; ++i) {
        const uint32_t w0 = rdram_read_u32(cmd_stream, i * 8 + 0);
        const uint32_t w1 = rdram_read_u32(cmd_stream, i * 8 + 4);

        const uint8_t cmd   = static_cast<uint8_t>(w0 >> 24) & 0x7F;
        const uint8_t flags = static_cast<uint8_t>(w0 >> 16);

        switch (cmd) {
            case A_SPNOOP:
                break;

            case A_CLEARBUFF: {
                const uint16_t dmem  = static_cast<uint16_t>((w0 & 0xFFFF) + DMEM_BASE);
                const uint16_t count = align_up(w1 & 0xFFF, 16);
                if (count > 0 && dmem < sizeof(state.dmem)) {
                    const uint32_t bytes = std::min<uint32_t>(count, sizeof(state.dmem) - dmem);
                    std::memset(state.dmem + dmem, 0, bytes);
                }
                break;
            }

            case A_SETBUFF: {
                if (flags & A_AUX) {
                    state.dry_right = static_cast<uint16_t>((w0 & 0xFFFF) + DMEM_BASE);
                    state.wet_left  = static_cast<uint16_t>((w1 >> 16) + DMEM_BASE);
                    state.wet_right = static_cast<uint16_t>((w1 & 0xFFFF) + DMEM_BASE);
                } else {
                    state.in_offset  = static_cast<uint16_t>((w0 & 0xFFFF) + DMEM_BASE);
                    state.out_offset = static_cast<uint16_t>((w1 >> 16) + DMEM_BASE);
                    state.count      = static_cast<uint16_t>(w1 & 0xFFFF);
                }
                break;
            }

            case A_SETVOL: {
                if (flags & A_AUX) {
                    state.dry = static_cast<int16_t>(w0 & 0xFFFF);
                    state.wet = static_cast<int16_t>(w1 & 0xFFFF);
                } else {
                    const unsigned lr = (flags & A_LEFT) ? 0 : 1;
                    if (flags & A_VOL) {
                        state.vol[lr] = static_cast<int16_t>(w0 & 0xFFFF);
                    } else {
                        state.target[lr] = static_cast<int16_t>(w0 & 0xFFFF);
                        state.rate[lr]   = static_cast<int32_t>(w1);
                    }
                }
                break;
            }

            case A_DMEMMOVE: {
                const uint16_t dmemi = static_cast<uint16_t>((w0 & 0xFFFF) + DMEM_BASE);
                const uint16_t dmemo = static_cast<uint16_t>((w1 >> 16) + DMEM_BASE);
                const uint16_t count = align_up(w1 & 0xFFFF, 16);
                if (count > 0 && dmemi < sizeof(state.dmem) && dmemo < sizeof(state.dmem)) {
                    const uint32_t bytes = std::min({count,
                                                     static_cast<uint16_t>(sizeof(state.dmem) - dmemi),
                                                     static_cast<uint16_t>(sizeof(state.dmem) - dmemo)});
                    std::memmove(state.dmem + dmemo, state.dmem + dmemi, bytes);
                }
                break;
            }

            case A_LOADBUFF: {
                const uint32_t address = state.get_address(w1) & ~7u; // RSP DMA alignment
                const uint16_t dmem    = state.in_offset & ~3u;
                const uint16_t count   = align_up(state.count, 8);
                if (state.count > 0 && dmem < 4096) {
                    const uint32_t bytes = std::min<uint32_t>(count, 4096 - dmem);
                    rdram_read_block(rdram, address, state.dmem + dmem, bytes);
                }
                break;
            }

            case A_SAVEBUFF: {
                const uint32_t address = state.get_address(w1) & ~7u; // RSP DMA alignment
                const uint16_t dmem    = state.out_offset & ~3u;
                const uint16_t count   = align_up(state.count, 8);
                if (state.count > 0 && dmem < 4096) {
                    const uint32_t bytes = std::min<uint32_t>(count, 4096 - dmem);
                    rdram_write_block(rdram, address, state.dmem + dmem, bytes);
                }
                break;
            }

            case A_SEGMENT: {
                const uint8_t seg   = static_cast<uint8_t>(w1 >> 24) & 0x0F;
                const uint32_t base = w1 & 0x00FFFFFF;
                state.segments[seg] = base;
                break;
            }

            case A_SETLOOP: {
                state.loop_address = state.get_address(w1);
                break;
            }

            case A_LOADADPCM: {
                const uint16_t count   = align_up(w0 & 0xFFFF, 8);
                const uint32_t address = state.get_address(w1);
                const uint32_t bytes   = std::min<uint32_t>(count, sizeof(state.adpcm_table));
                rdram_read_block(rdram, address, state.adpcm_table, bytes);
                break;
            }

            case A_INTERLEAVE: {
                const uint16_t left  = static_cast<uint16_t>((w1 >> 16) + DMEM_BASE);
                const uint16_t right = static_cast<uint16_t>((w1 & 0xFFFF) + DMEM_BASE);
                const uint16_t count = align_up(state.count, 16);

                const uint16_t* srcL = reinterpret_cast<const uint16_t*>(state.dmem + left);
                const uint16_t* srcR = reinterpret_cast<const uint16_t*>(state.dmem + right);
                uint16_t* dst        = reinterpret_cast<uint16_t*>(state.dmem + state.out_offset);

                const uint32_t samples = count >> 1; // mono sample count
                for (uint32_t s = 0; s < samples; ++s) {
                    dst[s * 2 + 0] = srcL[s];
                    dst[s * 2 + 1] = srcR[s];
                }
                break;
            }

            case A_MIXER: {
                const int16_t gain   = static_cast<int16_t>(w0 & 0xFFFF);
                const uint16_t dmemi = static_cast<uint16_t>((w1 >> 16) + DMEM_BASE);
                const uint16_t dmemo = static_cast<uint16_t>((w1 & 0xFFFF) + DMEM_BASE);
                const uint16_t count = align_up(state.count, 32);

                int16_t* dst       = state.dmem_s16(dmemo);
                const int16_t* src = state.dmem_s16(dmemi);

                const uint32_t samples = count >> 1;
                for (uint32_t s = 0; s < samples; ++s) {
                    sample_mix(&dst[s], src[s], gain);
                }
                break;
            }

            case A_ADPCM: {
                const bool init = (flags & A_INIT) != 0;
                const bool loop = (flags & A_LOOP) != 0;
                const uint32_t address = state.get_address(w1);
                uint16_t count = align_up(state.count, 32);

                int16_t last_frame[16];
                if (init) {
                    std::memset(last_frame, 0, sizeof(last_frame));
                } else {
                    const uint32_t src_addr = loop ? state.loop_address : address;
                    rdram_read_block(rdram, src_addr, last_frame, sizeof(last_frame));
                }

                uint16_t dmemo = state.out_offset;
                uint16_t dmemi = state.in_offset;

                for (size_t s = 0; s < 16; ++s, dmemo += 2) {
                    *state.dmem_s16(dmemo) = last_frame[s];
                }

                while (count != 0) {
                    int16_t frame[16];
                    const uint8_t code = state.dmem[dmemi++];
                    const unsigned char scale = (code & 0xF0) >> 4;
                    const int16_t* const cb_entry = state.adpcm_table + ((code & 0x0F) << 4);

                    const unsigned int rshift = (scale < 12) ? (12 - scale) : 0;
                    for (int f = 0; f < 8; ++f) {
                        const uint8_t byte = state.dmem[dmemi++];
                        frame[f * 2 + 0] = adpcm_predict_sample(byte, 0xF0, 8, rshift);
                        frame[f * 2 + 1] = adpcm_predict_sample(byte, 0x0F, 12, rshift);
                    }

                    adpcm_compute_residuals(last_frame,     frame,     cb_entry, last_frame + 14, 8);
                    adpcm_compute_residuals(last_frame + 8, frame + 8, cb_entry, last_frame + 6,  8);

                    for (size_t s = 0; s < 16; ++s, dmemo += 2) {
                        *state.dmem_s16(dmemo) = last_frame[s];
                    }

                    count -= 32;
                }

                rdram_write_block(rdram, address, last_frame, sizeof(last_frame));
                break;
            }

            case A_RESAMPLE: {
                const bool init = (flags & A_INIT) != 0;
                const uint32_t pitch = static_cast<uint32_t>(w0 & 0xFFFF) << 1;
                const uint32_t address = state.get_address(w1);
                uint16_t count = align_up(state.count, 16);

                uint16_t ipos = (state.in_offset >> 1) - 4;
                uint16_t opos = state.out_offset >> 1;
                uint32_t pitch_accu = 0;

                if (init) {
                    for (int k = 0; k < 4; ++k) {
                        *state.dmem_s16(static_cast<uint16_t>((ipos + k) << 1)) = 0;
                    }
                    pitch_accu = 0;
                } else {
                    for (int k = 0; k < 4; ++k) {
                        *state.dmem_s16(static_cast<uint16_t>((ipos + k) << 1)) =
                            rdram_read_s16(rdram, address + k * 2);
                    }
                    pitch_accu = rdram_read_u16(rdram, address + 8);
                }

                const uint32_t samples = count >> 1;
                for (uint32_t s = 0; s < samples; ++s) {
                    const int16_t* lut = RESAMPLE_LUT + ((pitch_accu & 0xFC00) >> 8);

                    const int16_t s0 = *state.dmem_s16(static_cast<uint16_t>((ipos + 0) << 1));
                    const int16_t s1 = *state.dmem_s16(static_cast<uint16_t>((ipos + 1) << 1));
                    const int16_t s2 = *state.dmem_s16(static_cast<uint16_t>((ipos + 2) << 1));
                    const int16_t s3 = *state.dmem_s16(static_cast<uint16_t>((ipos + 3) << 1));

                    const int32_t val = (static_cast<int32_t>(s0) * lut[0] +
                                         static_cast<int32_t>(s1) * lut[1] +
                                         static_cast<int32_t>(s2) * lut[2] +
                                         static_cast<int32_t>(s3) * lut[3]) >> 15;

                    *state.dmem_s16(static_cast<uint16_t>(opos << 1)) = clamp_s16(val);
                    ++opos;

                    pitch_accu += pitch;
                    ipos += static_cast<uint16_t>(pitch_accu >> 16);
                    pitch_accu &= 0xFFFF;
                }

                for (int k = 0; k < 4; ++k) {
                    rdram_write_s16(rdram, address + k * 2,
                                    *state.dmem_s16(static_cast<uint16_t>((ipos + k) << 1)));
                }
                rdram_write_u16(rdram, address + 8, static_cast<uint16_t>(pitch_accu));
                break;
            }

            case A_ENVMIXER: {
                const bool init = (flags & A_INIT) != 0;
                const bool aux  = (flags & A_AUX) != 0;
                const size_t num_bus = aux ? 4 : 2;
                const uint32_t address = state.get_address(w1);
                const uint16_t count   = state.count;

                const int16_t* const in = state.dmem_s16(state.in_offset);
                int16_t* const dl = state.dmem_s16(state.out_offset);
                int16_t* const dr = state.dmem_s16(state.dry_right);
                int16_t* const wl = state.dmem_s16(state.wet_left);
                int16_t* const wr = state.dmem_s16(state.wet_right);

                Ramp ramps[2];
                int32_t exp_seq[2];
                int32_t exp_rates[2];
                int16_t wet = state.wet;
                int16_t dry = state.dry;

                int16_t save_buffer[40];
                rdram_read_block(rdram, address, save_buffer, sizeof(save_buffer));

                if (init) {
                    ramps[0].value  = static_cast<int64_t>(state.vol[0]) << 16;
                    ramps[1].value  = static_cast<int64_t>(state.vol[1]) << 16;
                    ramps[0].target = static_cast<int64_t>(state.target[0]) << 16;
                    ramps[1].target = static_cast<int64_t>(state.target[1]) << 16;
                    exp_rates[0]    = state.rate[0];
                    exp_rates[1]    = state.rate[1];
                    exp_seq[0]      = static_cast<int32_t>(state.vol[0]) * state.rate[0];
                    exp_seq[1]      = static_cast<int32_t>(state.vol[1]) * state.rate[1];
                } else {
                    wet             = save_buffer[0];
                    dry             = save_buffer[2];
                    ramps[0].target = *reinterpret_cast<const int32_t*>(save_buffer + 4);
                    ramps[1].target = *reinterpret_cast<const int32_t*>(save_buffer + 6);
                    exp_rates[0]    = *reinterpret_cast<const int32_t*>(save_buffer + 8);
                    exp_rates[1]    = *reinterpret_cast<const int32_t*>(save_buffer + 10);
                    exp_seq[0]      = *reinterpret_cast<const int32_t*>(save_buffer + 12);
                    exp_seq[1]      = *reinterpret_cast<const int32_t*>(save_buffer + 14);
                    ramps[0].value  = *reinterpret_cast<const int32_t*>(save_buffer + 16);
                    ramps[1].value  = *reinterpret_cast<const int32_t*>(save_buffer + 18);
                }

                ramps[0].step = ramps[0].target - ramps[0].value;
                ramps[1].step = ramps[1].target - ramps[1].value;

                uint32_t ptr = 0;
                for (int y = 0; y < count; y += 16) {
                    if (ramps[0].step != 0) {
                        exp_seq[0] = static_cast<int32_t>((static_cast<int64_t>(exp_seq[0]) * exp_rates[0]) >> 16);
                        ramps[0].step = (exp_seq[0] - ramps[0].value) >> 3;
                    }
                    if (ramps[1].step != 0) {
                        exp_seq[1] = static_cast<int32_t>((static_cast<int64_t>(exp_seq[1]) * exp_rates[1]) >> 16);
                        ramps[1].step = (exp_seq[1] - ramps[1].value) >> 3;
                    }

                    for (int x = 0; x < 8; ++x) {
                        const int16_t l_vol = ramp_step(&ramps[0]);
                        const int16_t r_vol = ramp_step(&ramps[1]);

                        const int16_t gains[4] = {
                            clamp_s16((static_cast<int32_t>(l_vol) * dry + 0x4000) >> 15),
                            clamp_s16((static_cast<int32_t>(r_vol) * dry + 0x4000) >> 15),
                            clamp_s16((static_cast<int32_t>(l_vol) * wet + 0x4000) >> 15),
                            clamp_s16((static_cast<int32_t>(r_vol) * wet + 0x4000) >> 15)
                        };

                        int16_t* const buffers[4] = {
                            dl + ptr,
                            dr + ptr,
                            wl + ptr,
                            wr + ptr
                        };

                        const int16_t s_in = in[ptr];
                        for (size_t b = 0; b < num_bus; ++b) {
                            sample_mix(buffers[b], s_in, gains[b]);
                        }
                        ++ptr;
                    }
                }

                save_buffer[0] = wet;
                save_buffer[2] = dry;
                *reinterpret_cast<int32_t*>(save_buffer + 4)  = static_cast<int32_t>(ramps[0].target);
                *reinterpret_cast<int32_t*>(save_buffer + 6)  = static_cast<int32_t>(ramps[1].target);
                *reinterpret_cast<int32_t*>(save_buffer + 8)  = exp_rates[0];
                *reinterpret_cast<int32_t*>(save_buffer + 10) = exp_rates[1];
                *reinterpret_cast<int32_t*>(save_buffer + 12) = exp_seq[0];
                *reinterpret_cast<int32_t*>(save_buffer + 14) = exp_seq[1];
                *reinterpret_cast<int32_t*>(save_buffer + 16) = static_cast<int32_t>(ramps[0].value);
                *reinterpret_cast<int32_t*>(save_buffer + 18) = static_cast<int32_t>(ramps[1].value);

                rdram_write_block(rdram, address, save_buffer, sizeof(save_buffer));
                break;
            }

            case A_POLEF: {
                const bool init = (flags & A_INIT) != 0;
                const uint16_t gain    = static_cast<uint16_t>(w0 & 0xFFFF);
                const uint32_t address = state.get_address(w1);
                uint16_t count         = align_up(state.count, 16);

                int16_t* dst = state.dmem_s16(state.out_offset);
                const int16_t* const h1 = state.adpcm_table;
                int16_t* const h2       = state.adpcm_table + 8;

                int16_t l1 = 0;
                int16_t l2 = 0;
                if (!init) {
                    l1 = rdram_read_s16(rdram, address + 4);
                    l2 = rdram_read_s16(rdram, address + 6);
                }

                int16_t h2_before[8];
                for (int h = 0; h < 8; ++h) {
                    h2_before[h] = h2[h];
                    h2[h] = static_cast<int16_t>((static_cast<int32_t>(h2[h]) * gain) >> 14);
                }

                uint16_t dmemi = state.in_offset;
                do {
                    int16_t frame[8];
                    for (int f = 0; f < 8; ++f, dmemi += 2) {
                        frame[f] = *state.dmem_s16(dmemi);
                    }

                    for (int s = 0; s < 8; ++s) {
                        int32_t accu = static_cast<int32_t>(frame[s]) * gain;
                        accu += static_cast<int32_t>(h1[s]) * l1 +
                                static_cast<int32_t>(h2_before[s]) * l2 +
                                rdot(s, h2, frame);
                        dst[s] = clamp_s16(accu >> 14);
                    }

                    l1 = dst[6];
                    l2 = dst[7];

                    dst += 8;
                    count -= 16;
                } while (count != 0);

                rdram_write_s16(rdram, address + 4, l1);
                rdram_write_s16(rdram, address + 6, l2);
                break;
            }

            default:
                // Unknown ABI command; return false to allow fallback to dkrAspMain
                return false;
        }
    }

    return true;
}
