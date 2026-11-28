/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Host regression tests for the font bridge (TextAttr/TTextAttr and TextFont names):
    (1) Baseline OpenFont/OpenDiskFont/CloseFont/AskFont round-trip, TextFont
        shadow fields and ln_Name population (including NULL tf_CharLoc),
        field fidelity, missing fonts, valid TTextAttr TAG_MORE chains,
        8-byte TextAttr FSF_TAGGED stripping, bounds/NUL/empty checks, and
        CloseFont rollback when shadow_map is full;
    (2) Strict TA_DeviceDPI allowlist (blocks pointer-bearing OT_Indirect and
        unknown TAG_USER tags even when paired with TA_DeviceDPI), NULL TAG_MORE
        terminator, >35 TAG_IGNORE before TA_DeviceDPI, duplicate TA_DeviceDPI
        first-wins, and malformed/OOB/wrapping/cyclic TAG_MORE and TAG_SKIP;
    (3) m68k_font_name() / ctx->fontnames[16] + SHADOW_FONTNAME lifecycle across
        CloseFont, shadow_destroy, recycled native TextFont* and ln_Name buffers,
        repeated flip recycle zero-leak, same-name deduplication across sizes,
        >16-font AskFont/reopen/saturation, NULL/empty zero-name fonts,
        SHADOW_OWNRP coexistence, and containment-pinned shadow native_ptr.
*/
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

using UBYTE = uint8_t;
using BYTE = int8_t;
using UWORD = uint16_t;
using WORD = int16_t;
using ULONG = uint32_t;
using LONG = int32_t;
using IPTR = uintptr_t;
using BOOL = int;
using BPTR = uintptr_t;
using STRPTR = char *;
using CONST_STRPTR = const char *;
using Tag = ULONG;

struct AnyPtr {
    void *p;
    explicit AnyPtr(void *ptr = nullptr) : p(ptr) {}
    template <typename T> operator T *() const { return static_cast<T *>(p); }
    explicit operator bool() const { return p != nullptr; }
    bool operator!() const { return p == nullptr; }
};
using APTR = AnyPtr;

#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif

#if defined(__SANITIZE_ADDRESS__)
#define UNDER_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define UNDER_ASAN 1
#else
#define UNDER_ASAN 0
#endif
#else
#define UNDER_ASAN 0
#endif

#define MEMF_PUBLIC 1U
#define MEMF_CHIP   (1U << 1)
#define MEMF_FAST   (1U << 2)
#define MEMF_CLEAR  (1U << 16)

#define FS_NORMAL      0x00U
#define FSF_BOLD       0x02U
#define FSF_ITALIC     0x04U
#define FSF_TAGGED     0x80U

#define FPF_ROMFONT    0x01U
#define FPF_DISKFONT   0x02U
#define FPF_DESIGNED   0x40U

#define TAG_DONE       0UL
#define TAG_IGNORE     1UL
#define TAG_MORE       2UL
#define TAG_SKIP       3UL
#define TAG_USER       0x80000000UL
#define TA_DeviceDPI   (TAG_USER + 1UL)
#define OT_Indirect    0x8000UL
#define OT_OTagPath    (TAG_USER | OT_Indirect | 0x14UL)
#define OT_Engine      (TAG_USER | 0x1000UL | OT_Indirect | 0x02UL)
#define TA_CharSet     (TAG_USER + 2UL)

#define M68K_MH_FIRST 16
#define M68K_MH_FREE  28

enum { SF_END = 0, SF_BYTE, SF_WORD, SF_LONG, SF_PTR };

#include "font_consts.inc"

struct Node {
    struct Node *ln_Succ;
    struct Node *ln_Pred;
    UBYTE ln_Type;
    BYTE ln_Pri;
    char *ln_Name;
};
struct Message {
    struct Node mn_Node;
    void *mn_ReplyPort;
    UWORD mn_Length;
};
struct TagItem {
    Tag ti_Tag;
    IPTR ti_Data;
};
struct TextAttr {
    STRPTR ta_Name;
    UWORD ta_YSize;
    UBYTE ta_Style;
    UBYTE ta_Flags;
};
struct TTextAttr {
    STRPTR tta_Name;
    UWORD tta_YSize;
    UBYTE tta_Style;
    UBYTE tta_Flags;
    struct TagItem *tta_Tags;
};
struct TextFont {
    struct Message tf_Message;
    UWORD tf_YSize;
    UBYTE tf_Style;
    UBYTE tf_Flags;
    UWORD tf_XSize;
    UWORD tf_Baseline;
    UWORD tf_BoldSmear;
    UWORD tf_Accessors;
    UBYTE tf_LoChar;
    UBYTE tf_HiChar;
    APTR tf_CharData;
    UWORD tf_Modulo;
    APTR tf_CharLoc;
    APTR tf_CharSpace;
    APTR tf_CharKern;
};
struct RastPort { struct TextFont *Font; };
struct GfxBaseStruct { struct TextFont *DefaultFont; };
struct Screen { int dummy; };
struct DrawInfo { int dummy; };

struct M68KStructLayout;
struct M68KFieldMap {
    UWORD m68k_off;
    UWORD native_off;
    UBYTE type;
    UWORD shadow_type;
    UWORD sub_alloc;
    const struct M68KStructLayout *sub_layout;
};
struct M68KStructLayout {
    const char *name;
    UWORD m68k_size;
    UWORD shadow_type;
    const struct M68KFieldMap *fields;
};

struct FakeCPU { ULONG d[8]; ULONG a[8]; };
static ULONG M68KEmu_GetD(void *cpu, int n) { return static_cast<FakeCPU *>(cpu)->d[n]; }
static ULONG M68KEmu_GetA(void *cpu, int n) { return static_cast<FakeCPU *>(cpu)->a[n]; }
#define THUNK_D(n) M68KEmu_GetD(cpu, n)
#define THUNK_A(n) M68KEmu_GetA(cpu, n)

struct M68KHeapRegion { ULONG start, end, free, avail, mh; };
struct M68KFakeLibBase { ULONG m68k_addr; };

struct M68KEmuContext {
    UBYTE *mem;
    ULONG mem_size;
    ULONG heap_start;
    ULONG heap_end;
    M68KHeapRegion rgn[2];
    char program_name[64];
    BOOL trace_calls;
    BOOL shadow_full_logged;
    UWORD num_gobjs;
    struct { void *font; ULONG name; } fontnames[16];
    M68KFakeLibBase libs[8];
    UWORD num_libs;
    struct {
        ULONG m68k_addr;
        void *native_ptr;
        UWORD type;
        UBYTE streams_valid;
        BPTR stream_native[3];
        ULONG stream_handle[3];
    } shadow_map[M68KEMU_MAX_SHADOWS];
    UWORD num_shadows;
};

static inline AnyPtr m68k_to_host(struct M68KEmuContext *ctx, ULONG addr)
{
    if (addr == 0 || addr >= ctx->mem_size) return AnyPtr();
    return AnyPtr(ctx->mem + addr);
}

static inline void *m68k_to_host_or_shadow(struct M68KEmuContext *ctx, ULONG addr)
{
    if (addr == 0) return nullptr;
    for (UWORD i = 0; i < ctx->num_shadows; i++)
        if (ctx->shadow_map[i].m68k_addr == addr)
            return ctx->shadow_map[i].native_ptr;
    if (addr >= ctx->mem_size) return nullptr;
    return ctx->mem + addr;
}

static inline UWORD m68k_read16(struct M68KEmuContext *ctx, ULONG a)
{
    return (UWORD)((ctx->mem[a] << 8) | ctx->mem[a + 1]);
}
static inline ULONG m68k_read32(struct M68KEmuContext *ctx, ULONG a)
{
    return ((ULONG)ctx->mem[a] << 24) | ((ULONG)ctx->mem[a + 1] << 16) |
           ((ULONG)ctx->mem[a + 2] << 8) | (ULONG)ctx->mem[a + 3];
}
static inline void m68k_write16(struct M68KEmuContext *ctx, ULONG a, UWORD v)
{
    ctx->mem[a] = (UBYTE)(v >> 8);
    ctx->mem[a + 1] = (UBYTE)v;
}
static inline void m68k_write32(struct M68KEmuContext *ctx, ULONG a, ULONG v)
{
    ctx->mem[a] = (UBYTE)(v >> 24);
    ctx->mem[a + 1] = (UBYTE)(v >> 16);
    ctx->mem[a + 2] = (UBYTE)(v >> 8);
    ctx->mem[a + 3] = (UBYTE)v;
}

/* ── Native model ── */
static void bug(const char *, ...) {}
static void m68k_log(struct M68KEmuContext *, const char *, ...) {}
#define D(x) do { (void)0; } while (0)
static void M68KEmu_GuestObjectsFreed(struct M68KEmuContext *, ULONG, ULONG) {}
static void m68k_screen_extras(struct M68KEmuContext *, ULONG, struct Screen *) {}
static void m68k_drawinfo_extras(struct M68KEmuContext *, ULONG, struct DrawInfo *) {}

static ULONG g_loc[4] = { 0x00000008U, 0x00080008U, 0x00100008U, 0x00180008U };
static UBYTE g_data[64] = { 0xAA };
static struct TextFont g_recycled_tf;
static char g_recycled_name_buf[32];
static struct TextFont g_outline_times;
static struct TextFont g_topaz[4];
static struct TextFont g_many[24];
static char g_many_names[24][32];
static struct RastPort g_rp = { &g_recycled_tf };
static struct GfxBaseStruct g_gfxbase = { &g_topaz[0] };
static struct GfxBaseStruct *GfxBase = &g_gfxbase;

static int g_openfont_calls = 0;
static int g_opendiskfont_calls = 0;
static int g_closefont_calls = 0;
static std::string g_last_name;
static UWORD g_last_ysize = 0;
static UBYTE g_last_style = 0;
static UBYTE g_last_flags = 0;
static std::vector<std::pair<Tag, IPTR>> g_last_tags;
static bool g_inspect_tags = true;

static struct RastPort *resolve_rp(struct M68KEmuContext *, ULONG mrp) { return mrp ? &g_rp : nullptr; }

static void init_tf(struct TextFont &tf, char *name, UWORD ysize, UBYTE style, UBYTE flags)
{
    std::memset(&tf, 0, sizeof(tf));
    tf.tf_Message.mn_Node.ln_Name = name;
    tf.tf_YSize = ysize;
    tf.tf_Style = style;
    tf.tf_Flags = flags;
    tf.tf_XSize = 8;
    tf.tf_Baseline = static_cast<UWORD>(ysize > 2 ? ysize - 2 : 1);
    tf.tf_LoChar = 32;
    tf.tf_HiChar = 34;
    tf.tf_CharLoc = AnyPtr(g_loc);
    tf.tf_CharData = AnyPtr(g_data);
    tf.tf_Modulo = 4;
}

static void init_fonts()
{
    for (int i = 0; i < 4; i++)
        init_tf(g_topaz[i], const_cast<char *>("topaz.font"), static_cast<UWORD>(8 + i * 2),
                i == 0 ? FSF_TAGGED : FS_NORMAL, FPF_ROMFONT | FPF_DESIGNED);
    std::strcpy(g_recycled_name_buf, "helvetica.font");
    init_tf(g_recycled_tf, g_recycled_name_buf, 13, FS_NORMAL, FPF_DISKFONT | FPF_DESIGNED);
    init_tf(g_outline_times, const_cast<char *>("times.font"), 16, FSF_TAGGED, FPF_DISKFONT | FPF_DESIGNED);
    g_outline_times.tf_CharLoc = AnyPtr(); /* verify ln_Name populates before early return */
    for (int i = 0; i < 24; i++)
    {
        std::snprintf(g_many_names[i], sizeof(g_many_names[i]), "fam%02d.font", i);
        init_tf(g_many[i], g_many_names[i], 10, FS_NORMAL, FPF_DISKFONT);
    }
    g_rp.Font = &g_recycled_tf;
}

static struct TextFont *OpenFont(const struct TextAttr *ta)
{
    g_openfont_calls++;
    if (!ta || !ta->ta_Name) return nullptr;
    if (std::strcmp(ta->ta_Name, "topaz.font") == 0)
    {
        int idx = (ta->ta_YSize >= 14) ? 3 : (ta->ta_YSize >= 12) ? 2 : (ta->ta_YSize >= 10) ? 1 : 0;
        g_topaz[idx].tf_Accessors++;
        return &g_topaz[idx];
    }
    if (g_recycled_tf.tf_Message.mn_Node.ln_Name &&
        std::strcmp(ta->ta_Name, g_recycled_tf.tf_Message.mn_Node.ln_Name) == 0)
    {
        g_recycled_tf.tf_Accessors++;
        return &g_recycled_tf;
    }
    if (std::strcmp(ta->ta_Name, "times.font") == 0)
    {
        g_outline_times.tf_Accessors++;
        return &g_outline_times;
    }
    for (int i = 0; i < 24; i++)
        if (std::strcmp(ta->ta_Name, g_many_names[i]) == 0)
        {
            g_many[i].tf_Accessors++;
            return &g_many[i];
        }
    return nullptr;
}

static struct TextFont *OpenDiskFont(struct TextAttr *ta)
{
    g_opendiskfont_calls++;
    g_last_tags.clear();
    if (!ta || !ta->ta_Name) return nullptr;
    g_last_name = ta->ta_Name;
    g_last_ysize = ta->ta_YSize;
    g_last_style = ta->ta_Style;
    g_last_flags = ta->ta_Flags;
    if (ta->ta_Style & FSF_TAGGED)
    {
        const auto *tta = reinterpret_cast<const struct TTextAttr *>(ta);
        const struct TagItem *tags = tta->tta_Tags;
        if (g_inspect_tags && tags)
            for (const struct TagItem *ti = tags; ti->ti_Tag != TAG_DONE; ti++)
                g_last_tags.emplace_back(ti->ti_Tag, ti->ti_Data);
    }
    return OpenFont(ta);
}

static void CloseFont(struct TextFont *tf)
{
    g_closefont_calls++;
    if (tf && tf->tf_Accessors > 0) tf->tf_Accessors--;
}

#include "font_extracted.inc"

/* ── Test harness ── */
static int g_checks = 0;
static int g_failed = 0;
static void check(bool cond, const char *desc)
{
    g_checks++;
    if (!cond)
    {
        g_failed++;
        std::printf("FAIL: %s\n", desc);
    }
}

static void reset_env(struct M68KEmuContext &ctx, std::vector<UBYTE> &mem)
{
    init_fonts();
    g_openfont_calls = g_opendiskfont_calls = g_closefont_calls = 0;
    g_last_name.clear();
    g_last_ysize = 0;
    g_last_style = 0;
    g_last_flags = 0;
    g_last_tags.clear();
    g_inspect_tags = true;

    std::memset(&ctx, 0, sizeof(ctx));
    const ULONG logical_size = 0x240000U;
    /* In plain builds add 16 tail guard bytes so negative-control OOB reads fail
       deterministically without SIGSEGV; under ASan keep exact size so ASan catches real OOB. */
    mem.assign(UNDER_ASAN ? logical_size : (logical_size + 16U), 0);
    ctx.mem = mem.data();
    ctx.mem_size = logical_size;
    ctx.heap_start = 0x010400U;
    ctx.heap_end = 0x230000U;

    ctx.rgn[RGN_CHIP].start = 0x010400U;
    ctx.rgn[RGN_CHIP].end = M68KEMU_CHIP_END;
    ctx.rgn[RGN_CHIP].free = 0x010400U;
    ctx.rgn[RGN_CHIP].avail = M68KEMU_CHIP_END - 0x010400U;
    m68k_write32(&ctx, 0x010400U + FB_NEXT, 0);
    m68k_write32(&ctx, 0x010400U + FB_SIZE, ctx.rgn[RGN_CHIP].avail);

    ctx.rgn[RGN_FAST].start = M68KEMU_CHIP_END;
    ctx.rgn[RGN_FAST].end = 0x230000U;
    ctx.rgn[RGN_FAST].free = M68KEMU_CHIP_END;
    ctx.rgn[RGN_FAST].avail = 0x230000U - M68KEMU_CHIP_END;
    m68k_write32(&ctx, M68KEMU_CHIP_END + FB_NEXT, 0);
    m68k_write32(&ctx, M68KEMU_CHIP_END + FB_SIZE, ctx.rgn[RGN_FAST].avail);
}

/* A font name of the fixture, at most 31 characters, with its NUL. */
static ULONG put_cstr(struct M68KEmuContext &ctx, ULONG at, const char *s)
{
    std::memcpy(ctx.mem + at, s, ::strnlen(s, 31) + 1);
    return at;
}

static void put_ta(struct M68KEmuContext &ctx, ULONG at, ULONG name, UWORD ysize, UBYTE style, UBYTE flags)
{
    m68k_write32(&ctx, at + M68K_TA_NAME, name);
    m68k_write16(&ctx, at + M68K_TA_YSIZE, ysize);
    ctx.mem[at + M68K_TA_STYLE] = style;
    ctx.mem[at + M68K_TA_FLAGS] = flags;
}

static void put_tta(struct M68KEmuContext &ctx, ULONG at, ULONG name, UWORD ysize, UBYTE style, UBYTE flags, ULONG tags)
{
    put_ta(ctx, at, name, ysize, style, flags);
    m68k_write32(&ctx, at + 8, tags);
}

static ULONG call_openfont(struct M68KEmuContext &ctx, ULONG mta)
{
    FakeCPU cpu{}; cpu.a[0] = mta;
    return static_cast<ULONG>(thunk_gfx_OpenFont(&ctx, &cpu));
}
static ULONG call_opendiskfont(struct M68KEmuContext &ctx, ULONG mta)
{
    FakeCPU cpu{}; cpu.a[0] = mta;
    return static_cast<ULONG>(thunk_diskfont_OpenDiskFont_m(&ctx, &cpu));
}
static void call_closefont(struct M68KEmuContext &ctx, ULONG mtf)
{
    FakeCPU cpu{}; cpu.a[1] = mtf;
    thunk_graphics_CloseFont(&ctx, &cpu);
}
static void call_askfont(struct M68KEmuContext &ctx, ULONG mrp, ULONG mta)
{
    FakeCPU cpu{}; cpu.a[1] = mrp; cpu.a[0] = mta;
    thunk_gfx_AskFont(&ctx, &cpu);
}

int main()
{
    struct M68KEmuContext ctx;
    std::vector<UBYTE> mem;

    /* ── Group 1: Baseline OpenFont/OpenDiskFont round-trip, field fidelity, and missing fonts ── */
    reset_env(ctx, mem);
    ULONG s_topaz = put_cstr(ctx, 0x002000U, "topaz.font");
    ULONG ta1 = 0x002100U;
    put_ta(ctx, ta1, s_topaz, 8, FS_NORMAL, FPF_ROMFONT | FPF_DESIGNED);
    ULONG mf1 = call_openfont(ctx, ta1);
    check(mf1 != 0, "baseline: OpenFont(topaz.font/8) succeeds");
    check(m68k_read16(&ctx, mf1 + 20) == 8 && m68k_read16(&ctx, mf1 + 26) == 6,
          "baseline: TextFont shadow exposes tf_YSize=8 and tf_Baseline=6");
    check(g_topaz[0].tf_Accessors == 1, "baseline: native topaz/8 tf_Accessors == 1");
    ULONG mf1_name = m68k_read32(&ctx, mf1 + 10);
    check(mf1_name != 0 && mf1_name < ctx.mem_size &&
          std::strcmp(reinterpret_cast<const char *>(ctx.mem + mf1_name), "topaz.font") == 0,
          "baseline: TextFont shadow preserves tf_Message.mn_Node.ln_Name (+10) as 68k string");

    ULONG ta_reopen = 0x002140U;
    put_ta(ctx, ta_reopen, mf1_name, 10, FS_NORMAL, FPF_ROMFONT);
    ULONG mf2 = call_openfont(ctx, ta_reopen);
    check(mf2 != 0 && m68k_read16(&ctx, mf2 + 20) == 10,
          "baseline: follow-up OpenFont using shadow->ln_Name succeeds (topaz.font/10)");
    int odf_before = g_opendiskfont_calls;
    ULONG mf3 = call_opendiskfont(ctx, ta_reopen);
    check(mf3 != 0 && g_opendiskfont_calls == odf_before + 1,
          "baseline: follow-up OpenDiskFont using shadow->ln_Name reaches native and succeeds");
    call_closefont(ctx, mf3);
    call_closefont(ctx, mf2);
    call_closefont(ctx, mf1);
    check(g_topaz[0].tf_Accessors == 0 && g_topaz[1].tf_Accessors == 0,
          "baseline: CloseFont balances tf_Accessors back to 0");

    /* OpenDiskFont field fidelity + missing font */
    reset_env(ctx, mem);
    ULONG s_helv_base = put_cstr(ctx, 0x002000U, "helvetica.font");
    ULONG ta_helv_base = 0x002100U;
    put_ta(ctx, ta_helv_base, s_helv_base, 13, FSF_BOLD | FSF_ITALIC, FPF_DISKFONT | FPF_DESIGNED);
    ULONG mfh_base = call_opendiskfont(ctx, ta_helv_base);
    check(mfh_base != 0 && g_last_name == "helvetica.font" && g_last_ysize == 13 &&
          g_last_style == (FSF_BOLD | FSF_ITALIC) &&
          g_last_flags == (FPF_DISKFONT | FPF_DESIGNED),
          "baseline: OpenDiskFont forwards exact ta_Name, ta_YSize, ta_Style, ta_Flags");

    ULONG s_missing = put_cstr(ctx, 0x002040U, "missing.font");
    ULONG ta_missing = 0x002120U;
    put_ta(ctx, ta_missing, s_missing, 12, FS_NORMAL, 0);
    check(call_openfont(ctx, ta_missing) == 0 && call_opendiskfont(ctx, ta_missing) == 0,
          "baseline: missing font returns 0 from both OpenFont and OpenDiskFont");

    /* ── Group 2: TTextAttr chaining, NULL tf_CharLoc, AskFont 8-byte TextAttr, and strict TA_DeviceDPI allowlist ── */
    reset_env(ctx, mem);
    ULONG s_times_base = put_cstr(ctx, 0x002000U, "times.font");
    ULONG tag1 = 0x003000U, tag2 = 0x003100U;
    m68k_write32(&ctx, tag1 + 0, TAG_IGNORE);   m68k_write32(&ctx, tag1 + 4, 0x12345678U);
    m68k_write32(&ctx, tag1 + 8, TAG_SKIP);     m68k_write32(&ctx, tag1 + 12, 1U);
    m68k_write32(&ctx, tag1 + 16, TA_DeviceDPI); m68k_write32(&ctx, tag1 + 20, 0xBAD0BAD0U);
    m68k_write32(&ctx, tag1 + 24, TAG_MORE);    m68k_write32(&ctx, tag1 + 28, tag2);
    m68k_write32(&ctx, tag2 + 0, TA_DeviceDPI); m68k_write32(&ctx, tag2 + 4, 0x00480048U);
    m68k_write32(&ctx, tag2 + 8, TAG_DONE);     m68k_write32(&ctx, tag2 + 12, 0);
    ULONG tta_chain = 0x002200U;
    put_tta(ctx, tta_chain, s_times_base, 16, FSF_TAGGED | FSF_BOLD, FPF_DISKFONT, tag1);
    ULONG mft_chain = call_opendiskfont(ctx, tta_chain);
    check(mft_chain != 0, "ttextattr: OpenDiskFont(TTextAttr times.font/16 FSF_TAGGED) succeeds");
    check((g_last_style & FSF_TAGGED) != 0 && (g_last_style & FSF_BOLD) != 0,
          "ttextattr: FSF_TAGGED and FSF_BOLD preserved for valid TTextAttr");
    check(g_last_tags.size() == 1 && g_last_tags[0].first == TA_DeviceDPI &&
          g_last_tags[0].second == 0x00480048U,
          "ttextattr: TA_DeviceDPI (0x00480048) translated intact across TAG_MORE chain");
    ULONG mft_chain_name = mft_chain ? m68k_read32(&ctx, mft_chain + 10) : 0;
    check(mft_chain_name != 0 &&
          std::strcmp(reinterpret_cast<const char *>(ctx.mem + mft_chain_name), "times.font") == 0,
          "ttextattr: ln_Name populated even when tf_CharLoc == NULL");

    /* AskFont on FSF_TAGGED ROM font -> 8-byte TextAttr with uninitialized word at +8 */
    reset_env(ctx, mem);
    g_rp.Font = &g_topaz[0];
    g_inspect_tags = false; /* avoid deref on unpatched garbage in plain mode; ASan still checks stack read */
    ULONG ta_ask_8 = 0x002300U;
    m68k_write32(&ctx, ta_ask_8 + 8, 0xDEADBEEFU);
    call_askfont(ctx, 0x002400U, ta_ask_8);
    check((ctx.mem[ta_ask_8 + M68K_TA_STYLE] & FSF_TAGGED) != 0,
          "ttextattr: AskFont on tagged ROM font copies FSF_TAGGED into 8-byte TextAttr");
    ULONG mfa = call_opendiskfont(ctx, ta_ask_8);
    check(mfa != 0, "ttextattr: OpenDiskFont on AskFont 8-byte TextAttr succeeds");
    check((g_last_style & FSF_TAGGED) == 0,
          "ttextattr: FSF_TAGGED stripped when +8 is not a valid 68k tag list (no stack OOB)");
    struct TextAttr plain_ta{};
    check(textattr_in(&ctx, ta_ask_8, &plain_ta) && (plain_ta.ta_Style & FSF_TAGGED) == 0,
          "ttextattr: plain textattr_in strips FSF_TAGGED");

    /* Strict TA_DeviceDPI allowlist (blocks OT_Indirect & all other TAG_USER) */
    reset_env(ctx, mem);
    s_topaz = put_cstr(ctx, 0x002000U, "topaz.font");
    ULONG tl = 0x003000U;
    m68k_write32(&ctx, tl + 0, OT_OTagPath); m68k_write32(&ctx, tl + 4, 0x00002000U);
    m68k_write32(&ctx, tl + 8, OT_Engine);   m68k_write32(&ctx, tl + 12, 0x00002000U);
    m68k_write32(&ctx, tl + 16, TAG_DONE);   m68k_write32(&ctx, tl + 20, 0);
    put_tta(ctx, 0x002100U, s_topaz, 8, FSF_TAGGED, FPF_ROMFONT, tl);
    check(call_opendiskfont(ctx, 0x002100U) != 0, "ttextattr: OpenDiskFont opens untagged when tag list has OT_Indirect");
    check((g_last_style & FSF_TAGGED) == 0 && g_last_tags.empty(),
          "ttextattr: pointer-bearing OT_Indirect tags NOT accepted or forwarded");

    tl = 0x003100U;
    m68k_write32(&ctx, tl + 0, TA_DeviceDPI); m68k_write32(&ctx, tl + 4, 0x00480048U);
    m68k_write32(&ctx, tl + 8, OT_OTagPath);  m68k_write32(&ctx, tl + 12, 0x00002000U);
    m68k_write32(&ctx, tl + 16, TAG_DONE);    m68k_write32(&ctx, tl + 20, 0);
    put_tta(ctx, 0x002120U, s_topaz, 8, FSF_TAGGED | FSF_BOLD, FPF_ROMFONT, tl);
    check(call_opendiskfont(ctx, 0x002120U) != 0, "ttextattr: OpenDiskFont falls back safely on mixed OT_OTagPath list");
    check((g_last_style & FSF_TAGGED) == 0 && (g_last_style & FSF_BOLD) != 0 && g_last_tags.empty(),
          "ttextattr: strict TA_DeviceDPI allowlist rejects tag list containing unknown TAG_USER while preserving FSF_BOLD");

    tl = 0x003140U;
    m68k_write32(&ctx, tl + 0, TA_CharSet);   m68k_write32(&ctx, tl + 4, 4U);
    m68k_write32(&ctx, tl + 8, TAG_DONE);     m68k_write32(&ctx, tl + 12, 0);
    put_tta(ctx, 0x002128U, s_topaz, 8, FSF_TAGGED, FPF_ROMFONT, tl);
    check(call_opendiskfont(ctx, 0x002128U) != 0 && (g_last_style & FSF_TAGGED) == 0 && g_last_tags.empty(),
          "ttextattr: unknown numeric TAG_USER (TA_CharSet) not forwarded");

    /* Duplicate TA_DeviceDPI (first wins) + TAG_SKIP + >35 TAG_IGNORE + NULL TAG_MORE */
    tl = 0x003200U;
    for (int i = 0; i < 36; i++)
    {
        m68k_write32(&ctx, tl + i * 8, TAG_IGNORE);
        m68k_write32(&ctx, tl + i * 8 + 4, static_cast<ULONG>(i));
    }
    m68k_write32(&ctx, tl + 36 * 8, TAG_SKIP);     m68k_write32(&ctx, tl + 36 * 8 + 4, 1U);
    m68k_write32(&ctx, tl + 37 * 8, OT_OTagPath);  m68k_write32(&ctx, tl + 37 * 8 + 4, 0x00002000U); /* skipped */
    m68k_write32(&ctx, tl + 38 * 8, TA_DeviceDPI); m68k_write32(&ctx, tl + 38 * 8 + 4, 0x00500050U);
    m68k_write32(&ctx, tl + 39 * 8, TA_DeviceDPI); m68k_write32(&ctx, tl + 39 * 8 + 4, 0x00600060U); /* second ignored */
    m68k_write32(&ctx, tl + 40 * 8, TAG_MORE);     m68k_write32(&ctx, tl + 40 * 8 + 4, 0); /* NULL TAG_MORE terminates */
    put_tta(ctx, 0x002140U, s_topaz, 8, FSF_TAGGED, FPF_ROMFONT, tl);
    check(call_opendiskfont(ctx, 0x002140U) != 0, "ttextattr: OpenDiskFont succeeds on TAG_IGNORE+TAG_SKIP+TA_DeviceDPI+NULL TAG_MORE");
    check((g_last_style & FSF_TAGGED) != 0 && g_last_tags.size() == 1 &&
          g_last_tags[0].first == TA_DeviceDPI && g_last_tags[0].second == 0x00500050U,
          "ttextattr: first TA_DeviceDPI (0x00500050) preserved past 36 TAG_IGNORE + TAG_SKIP + NULL TAG_MORE terminator");

    /* Malformed TAG_MORE cycles */
    tl = 0x003600U;
    m68k_write32(&ctx, tl + 0, TA_DeviceDPI); m68k_write32(&ctx, tl + 4, 0x00480048U);
    m68k_write32(&ctx, tl + 8, TAG_MORE);     m68k_write32(&ctx, tl + 12, tl); /* self-cycle */
    put_tta(ctx, 0x002160U, s_topaz, 8, FSF_TAGGED, FPF_ROMFONT, tl);
    check(call_opendiskfont(ctx, 0x002160U) != 0 && (g_last_style & FSF_TAGGED) == 0 && g_last_tags.empty(),
          "ttextattr: self-cyclic TAG_MORE terminates cleanly and drops tags");

    ULONG cyc1 = 0x003640U, cyc2 = 0x003680U;
    m68k_write32(&ctx, cyc1 + 0, TA_DeviceDPI); m68k_write32(&ctx, cyc1 + 4, 0x00480048U);
    m68k_write32(&ctx, cyc1 + 8, TAG_MORE);     m68k_write32(&ctx, cyc1 + 12, cyc2);
    m68k_write32(&ctx, cyc2 + 0, TAG_IGNORE);   m68k_write32(&ctx, cyc2 + 4, 0);
    m68k_write32(&ctx, cyc2 + 8, TAG_MORE);     m68k_write32(&ctx, cyc2 + 12, cyc1);
    put_tta(ctx, 0x002180U, s_topaz, 8, FSF_TAGGED, FPF_ROMFONT, cyc1);
    check(call_opendiskfont(ctx, 0x002180U) != 0 && (g_last_style & FSF_TAGGED) == 0 && g_last_tags.empty(),
          "ttextattr: 2-node cyclic TAG_MORE terminates cleanly and drops tags");

    /* Malformed OOB TAG_MORE & wrapping/OOB TAG_SKIP */
    tl = 0x003700U;
    m68k_write32(&ctx, tl + 0, TA_DeviceDPI); m68k_write32(&ctx, tl + 4, 0x00480048U);
    m68k_write32(&ctx, tl + 8, TAG_MORE);     m68k_write32(&ctx, tl + 12, ctx.mem_size - 4);
    put_tta(ctx, 0x0021A0U, s_topaz, 8, FSF_TAGGED, FPF_ROMFONT, tl);
    check(call_opendiskfont(ctx, 0x0021A0U) != 0 && (g_last_style & FSF_TAGGED) == 0 && g_last_tags.empty(),
          "ttextattr: OOB TAG_MORE rejected cleanly without OOB read");

    tl = 0x003740U;
    m68k_write32(&ctx, tl + 0, TA_DeviceDPI); m68k_write32(&ctx, tl + 4, 0x00480048U);
    m68k_write32(&ctx, tl + 8, TAG_SKIP);     m68k_write32(&ctx, tl + 12, 0xFFFFFFFFU); /* wrap hazard */
    put_tta(ctx, 0x0021C0U, s_topaz, 8, FSF_TAGGED, FPF_ROMFONT, tl);
    check(call_opendiskfont(ctx, 0x0021C0U) != 0 && (g_last_style & FSF_TAGGED) == 0 && g_last_tags.empty(),
          "ttextattr: TAG_SKIP=0xFFFFFFFF rejected cleanly without wrap");

    tl = 0x003780U;
    m68k_write32(&ctx, tl + 0, TA_DeviceDPI); m68k_write32(&ctx, tl + 4, 0x00480048U);
    m68k_write32(&ctx, tl + 8, TAG_SKIP);     m68k_write32(&ctx, tl + 12, 0x00080000U); /* skips past mem_size */
    put_tta(ctx, 0x0021E0U, s_topaz, 8, FSF_TAGGED, FPF_ROMFONT, tl);
    check(call_opendiskfont(ctx, 0x0021E0U) != 0 && (g_last_style & FSF_TAGGED) == 0 && g_last_tags.empty(),
          "ttextattr: TAG_SKIP past mem_size rejected cleanly");

    /* ── Group 3: Bounds, NUL-termination, and empty ta_Name guards ── */
    ULONG s_empty = put_cstr(ctx, 0x002040U, "");
    put_tta(ctx, 0x002200U, s_empty, 8, FS_NORMAL, 0, 0);
    int of_b = g_openfont_calls, odf_b = g_opendiskfont_calls;
    check(call_openfont(ctx, 0x002200U) == 0 && call_opendiskfont(ctx, 0x002200U) == 0 &&
          g_openfont_calls == of_b && g_opendiskfont_calls == odf_b,
          "bounds: empty ta_Name rejected in textattr_in before calling native");

    reset_env(ctx, mem);
    check(call_openfont(ctx, 0) == 0 && call_opendiskfont(ctx, 0) == 0,
          "bounds: NULL TextAttr returns 0");
    ULONG ta_nullname = 0x002500U;
    put_ta(ctx, ta_nullname, 0, 8, 0, 0);
    check(call_openfont(ctx, ta_nullname) == 0 && call_opendiskfont(ctx, ta_nullname) == 0,
          "bounds: NULL ta_Name returns 0 without calling native");

    ULONG ta_oob = ctx.mem_size - 4;
    ULONG s_ok = put_cstr(ctx, 0x002000U, "topaz.font");
    m68k_write32(&ctx, ta_oob, s_ok);
    of_b = g_openfont_calls;
    check(call_openfont(ctx, ta_oob) == 0 && g_openfont_calls == of_b,
          "bounds: truncated TextAttr at mem_size-4 rejected before native call");

    ULONG bad_str = ctx.mem_size - 5;
    std::memcpy(ctx.mem + bad_str, "topaz", 5); /* no NUL before ctx.mem_size */
    ULONG ta_unterm = 0x002520U;
    put_ta(ctx, ta_unterm, bad_str, 8, 0, 0);
    of_b = g_openfont_calls;
    check(call_openfont(ctx, ta_unterm) == 0 && g_openfont_calls == of_b,
          "bounds: unterminated ta_Name at end of mem rejected before native call");

    /* ── Group 4: CloseFont rollback on shadow_create_by_name failure (shadow_map full) ── */
    reset_env(ctx, mem);
    ULONG s_t_full = put_cstr(ctx, 0x002000U, "topaz.font");
    ULONG s_h_full = put_cstr(ctx, 0x002020U, "helvetica.font");
    ULONG ta_t_full = 0x002100U, ta_h_full = 0x002120U;
    put_ta(ctx, ta_t_full, s_t_full, 8, 0, FPF_ROMFONT);
    put_ta(ctx, ta_h_full, s_h_full, 13, 0, FPF_DISKFONT);
    ctx.num_shadows = M68KEMU_MAX_SHADOWS;
    check(call_openfont(ctx, ta_t_full) == 0, "rollback: OpenFont returns 0 when shadow_map is full");
    check(g_topaz[0].tf_Accessors == 0 && g_closefont_calls == 1,
          "rollback: OpenFont rolls back native CloseFont on shadow failure (no tf_Accessors leak)");
    check(call_opendiskfont(ctx, ta_h_full) == 0, "rollback: OpenDiskFont returns 0 when shadow_map is full");
    check(g_recycled_tf.tf_Accessors == 0 && g_closefont_calls == 2,
          "rollback: OpenDiskFont rolls back native CloseFont on shadow failure (no tf_Accessors leak)");

    /* ── Group 5: fontnames[16] & SHADOW_FONTNAME lifecycle across CloseFont, shadow_destroy, recycle, >16, zero-name ── */
    reset_env(ctx, mem);
    ULONG s_helv = put_cstr(ctx, 0x002000U, "helvetica.font");
    put_tta(ctx, 0x002100U, s_helv, 13, FS_NORMAL, FPF_DISKFONT, 0);
    ULONG mf_h = call_opendiskfont(ctx, 0x002100U);
    ULONG name_h = m68k_read32(&ctx, mf_h + 10);
    check(mf_h != 0 && name_h != 0 &&
          std::strcmp(reinterpret_cast<const char *>(ctx.mem + name_h), "helvetica.font") == 0,
          "fontnames: initial diskfont shadow caches helvetica.font");

    /* Simulate CloseFont + shadow_destroy + native expunge/address recycling for a different font ("times.font")
       at the exact same native TextFont* and native ln_Name buffer address. */
    call_closefont(ctx, mf_h);
    shadow_destroy(&ctx, shadow_find_layout("TextFont"), mf_h);
    bool detached_after_destroy = true;
    for (int k = 0; k < 16; k++)
        if (ctx.fontnames[k].font == &g_recycled_tf) detached_after_destroy = false;
    check(detached_after_destroy, "fontnames: shadow_destroy(TextFont) detaches stale fontnames[].font pointer");

    std::strcpy(g_recycled_name_buf, "times.font");
    g_recycled_tf.tf_YSize = 16;

    ULONG s_times = put_cstr(ctx, 0x002040U, "times.font");
    put_tta(ctx, 0x002120U, s_times, 16, FS_NORMAL, FPF_DISKFONT, 0);
    ULONG mf_t = call_opendiskfont(ctx, 0x002120U);
    ULONG name_t = m68k_read32(&ctx, mf_t + 10);
    check(mf_t != 0 && name_t != 0 && name_t != name_h &&
          std::strcmp(reinterpret_cast<const char *>(ctx.mem + name_t), "times.font") == 0,
          "fontnames: recycled native TextFont* & ln_Name buffer gets times.font (no stale helvetica.font alias)");
    check(std::strcmp(reinterpret_cast<const char *>(ctx.mem + name_h), "helvetica.font") == 0,
          "fontnames: previously handed-out 68k font name string remains intact (no 68k UAF)");

    ULONG ta_ask = 0x002200U;
    call_askfont(ctx, 0x002300U, ta_ask);
    ULONG ask_name = m68k_read32(&ctx, ta_ask);
    check(ask_name == name_t &&
          std::strcmp(reinterpret_cast<const char *>(ctx.mem + ask_name), "times.font") == 0,
          "fontnames: AskFont on recycled native TextFont* agrees with shadow (times.font)");

    /* Repeated CloseFont + native recycle without shadow_destroy toggling
       helvetica.font <-> times.font at the same native TextFont* must not alias and must allocate 0 new heap bytes. */
    ULONG avail_before_flips = ctx.rgn[RGN_FAST].avail;
    bool flips_ok = true;
    for (int r = 0; r < 10; r++)
    {
        const bool to_helv = (r % 2 == 0);
        call_closefont(ctx, mf_t);
        std::strcpy(g_recycled_name_buf, to_helv ? "helvetica.font" : "times.font");
        ULONG mf_r = call_opendiskfont(ctx, to_helv ? 0x002100U : 0x002120U);
        ULONG nm_r = mf_r ? m68k_read32(&ctx, mf_r + 10) : 0;
        call_askfont(ctx, 0x002300U, ta_ask);
        ULONG ask_r = m68k_read32(&ctx, ta_ask);
        const char *want = to_helv ? "helvetica.font" : "times.font";
        ULONG expected = to_helv ? name_h : name_t;
        if (!nm_r || nm_r != expected || ask_r != expected ||
            std::strcmp(reinterpret_cast<const char *>(ctx.mem + nm_r), want) != 0)
            flips_ok = false;
        mf_t = mf_r;
    }
    check(flips_ok && ctx.rgn[RGN_FAST].avail == avail_before_flips,
          "fontnames: 10 CloseFont+recycle flips without shadow_destroy reuse cached names with 0 heap growth");

    /* Recycled native TextFont* with NULL or empty ("") zero-name clears stale cache link & writes 0 */
    call_closefont(ctx, mf_t);
    shadow_destroy(&ctx, shadow_find_layout("TextFont"), mf_t);
    g_recycled_tf.tf_Message.mn_Node.ln_Name = nullptr;
    ULONG mf_z = shadow_create_by_name(&ctx, "TextFont", &g_recycled_tf);
    check(mf_z != 0 && m68k_read32(&ctx, mf_z + 10) == 0,
          "fontnames: zero-name (NULL ln_Name) TextFont shadow writes 0 at +10");
    call_askfont(ctx, 0x002300U, ta_ask);
    check(m68k_read32(&ctx, ta_ask) == 0,
          "fontnames: AskFont after NULL ln_Name recycle returns 0");

    shadow_destroy(&ctx, shadow_find_layout("TextFont"), mf_z);
    g_recycled_name_buf[0] = '\0';
    g_recycled_tf.tf_Message.mn_Node.ln_Name = g_recycled_name_buf;
    ULONG mf_e = shadow_create_by_name(&ctx, "TextFont", &g_recycled_tf);
    check(mf_e != 0 && m68k_read32(&ctx, mf_e + 10) == 0,
          "fontnames: empty-string ln_Name TextFont shadow writes 0 at +10");

    /* Same font family across 4 sizes deduplicates into 1 fontnames slot & 1 68k string */
    reset_env(ctx, mem);
    s_topaz = put_cstr(ctx, 0x002000U, "topaz.font");
    ULONG n_sizes[4] = {};
    for (int i = 0; i < 4; i++)
    {
        put_tta(ctx, 0x002100U + i * 16U, s_topaz, static_cast<UWORD>(8 + i * 2), FS_NORMAL, FPF_ROMFONT, 0);
        ULONG mf = call_openfont(ctx, 0x002100U + i * 16U);
        n_sizes[i] = mf ? m68k_read32(&ctx, mf + 10) : 0;
    }
    int used_slots = 0;
    for (int k = 0; k < 16; k++) if (ctx.fontnames[k].name) used_slots++;
    check(n_sizes[0] != 0 && n_sizes[0] == n_sizes[1] && n_sizes[0] == n_sizes[2] && n_sizes[0] == n_sizes[3] &&
          used_slots == 1,
          "fontnames: 4 sizes of topaz.font share 1 68k string and 1 fontnames slot");

    /* >16-font saturation (24 distinct families) + AskFont + zero-leak reopen + recycle + full shadow_map rollback */
    reset_env(ctx, mem);
    bool all24_ok = true;
    ULONG name22 = 0;
    for (int i = 0; i < 24; i++)
    {
        ULONG s = put_cstr(ctx, 0x004000U + i * 32U, g_many_names[i]);
        put_tta(ctx, 0x004800U + i * 16U, s, 10, FS_NORMAL, FPF_DISKFONT, 0);
        ULONG mf = call_openfont(ctx, 0x004800U + i * 16U);
        ULONG nm = mf ? m68k_read32(&ctx, mf + 10) : 0;
        if (!mf || !nm || std::strcmp(reinterpret_cast<const char *>(ctx.mem + nm), g_many_names[i]) != 0)
            all24_ok = false;
        if (i == 22) name22 = nm;
    }
    check(all24_ok, "fontnames: all 24 distinct font families (>16) retain valid 68k ln_Name");

    g_rp.Font = &g_many[22];
    call_askfont(ctx, 0x002300U, 0x002200U);
    check(m68k_read32(&ctx, 0x002200U) == name22,
          "fontnames: AskFont on font #22 (>16) reuses SHADOW_FONTNAME instead of returning 0");

    /* Reopen >16 font #22 via its own shadow ln_Name: must deduplicate with 0 extra heap or shadow entries */
    ULONG avail_before_reopen = ctx.rgn[RGN_FAST].avail;
    UWORD shadows_before_reopen = ctx.num_shadows;
    ULONG ta22_reopen = 0x002400U;
    put_ta(ctx, ta22_reopen, name22, 10, FS_NORMAL, FPF_DISKFONT);
    ULONG mf22_reopen = call_openfont(ctx, ta22_reopen);
    check(mf22_reopen != 0 && m68k_read32(&ctx, mf22_reopen + 10) == name22 &&
          ctx.rgn[RGN_FAST].avail == avail_before_reopen && ctx.num_shadows == shadows_before_reopen,
          "fontnames: reopening >16 font via shadow ln_Name reuses cached name (zero heap/shadow leak)");

    std::strcpy(g_many_names[22], "recycled22.font");
    ULONG mf22_new = shadow_create_by_name(&ctx, "TextFont", &g_many[22]);
    ULONG nm22_new = mf22_new ? m68k_read32(&ctx, mf22_new + 10) : 0;
    check(nm22_new != 0 && nm22_new != name22 &&
          std::strcmp(reinterpret_cast<const char *>(ctx.mem + nm22_new), "recycled22.font") == 0,
          "fontnames: >16 SHADOW_FONTNAME verifies content and avoids stale alias on recycled native ln_Name buffer");

    ctx.num_shadows = M68KEMU_MAX_SHADOWS;
    ULONG avail_before_full = ctx.rgn[RGN_FAST].avail;
    std::strcpy(g_many_names[23], "overflow25.font");
    g_rp.Font = &g_many[23];
    call_askfont(ctx, 0x002300U, 0x002200U);
    check(m68k_read32(&ctx, 0x002200U) == 0 && ctx.rgn[RGN_FAST].avail == avail_before_full,
          "fontnames: new >16 font name when shadow_map full rolls back 68k heap cleanly (zero leak)");

    /* ── Group 6: SHADOW_FONTNAME coexistence with SHADOW_OWNRP and non-dangling shadow native_ptr ── */
#ifdef SHADOW_FONTNAME
    check(SHADOW_FONTNAME != SHADOW_OWNRP && SHADOW_FONTNAME != SHADOW_GLYPHS,
          "coexistence: SHADOW_FONTNAME does not collide with SHADOW_OWNRP or SHADOW_GLYPHS");
#else
    check(false, "coexistence: SHADOW_FONTNAME is defined and distinct from SHADOW_OWNRP");
#endif

    reset_env(ctx, mem);
    static int g_fake_own_rp = 0;
    ULONG fake_own_rp = put_cstr(ctx, 0x002600U, "fam16.font");
    shadow_register(&ctx, fake_own_rp, &g_fake_own_rp, SHADOW_OWNRP);
    for (int i = 0; i < 16; i++)
    {
        ULONG s = put_cstr(ctx, 0x004000U + i * 32U, g_many_names[i]);
        put_tta(ctx, 0x004800U + i * 16U, s, 10, FS_NORMAL, FPF_DISKFONT, 0);
        call_openfont(ctx, 0x004800U + i * 16U);
    }
    ULONG s16 = put_cstr(ctx, 0x004000U + 16U * 32U, g_many_names[16]);
    put_tta(ctx, 0x004800U + 16U * 16U, s16, 10, FS_NORMAL, FPF_DISKFONT, 0);
    ULONG mf16 = call_openfont(ctx, 0x004800U + 16U * 16U);
    ULONG nm16 = mf16 ? m68k_read32(&ctx, mf16 + 10) : 0;
    check(mf16 != 0 && nm16 != 0 && nm16 != fake_own_rp &&
          std::strcmp(reinterpret_cast<const char *>(ctx.mem + nm16), "fam16.font") == 0,
          "coexistence: >16 font name ignores SHADOW_OWNRP entry and does not alias guest RastPort memory");

    void *resolved16 = m68k_to_host_or_shadow(&ctx, nm16);
    std::strcpy(g_many_names[16], "expunged.font");
    check(nm16 != 0 && resolved16 == static_cast<void *>(ctx.mem + nm16) &&
          resolved16 != static_cast<void *>(g_many_names[16]) &&
          std::strcmp(static_cast<const char *>(resolved16), "fam16.font") == 0,
          "coexistence: SHADOW_FONTNAME native_ptr stays pinned to containment copy (no dangling native pointer)");

    std::printf("m68kemu font bridge: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
