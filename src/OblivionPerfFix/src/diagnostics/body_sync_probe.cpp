// Diagnostic BodySyncProbe ([Fixes] BodySyncProbe=0 default; 1 = counts, 2 = counts and call paths; start-up only, on the
// version.dll route): how often the game thread re-files a physics body in the collision search structure per frame, and
// from where. No behaviour change: two entry detours count and pass every call on unchanged.
//
// Why: in a battle about half of a near character's movement tick (VPairedPawnMovementComponent) is the end of the move,
// where the new capsule position is passed down to every attached component, and each physics body is moved and its
// bounds re-filed in the scene-query tree. If a body is re-filed twice in one frame (once when the capsule moves, once
// after the animation), or with bounds equal to its last ones, that work may be skippable; this probe counts both.
//
// Sites (build 19115871; each by pattern, one match):
//   the scene-query element update with new bounds (exe+0x1935EF0): rcx = the structure (+0x120 = its tree), rdx = the
//     bounds (6 doubles, min xyz then max xyz), r8 = the element's payload (8 bytes, passed on by address);
//     mov rax,rsp; sub rsp,58h; movups xmm0,[rdx]; add rcx,120h; mov [rax+8],r8; ... call
//   the physics write-command runner (exe+0x3530480): rcx = the actor handle's address, rdx = the command (a callable
//     reference: [rdx] invoker, [rdx+8] object); takes the scene's write lock around the call;
//     push rbx; sub rsp,40h; mov rbx,rdx; mov r8b,1; mov rdx,rcx; lea rcx,[rsp+20h]; call (lock); mov rcx,[rbx+8]; call [rbx]
// Only game-thread calls are counted per element (LedgerIsGameThread, so HitchLedger=1 is needed); other threads' calls
// are only counted. The frame is NpcCreationPacing's frame counter.
#include "common.h"

#include <atomic>
#include <cstring>

namespace opf {
namespace {

constexpr char kTreeUpdate[] =
    "48 8B C4 48 83 EC 58 0F 10 02 48 81 C1 20 01 00 00 4C 89 40 08 F2 0F 10 4A 10 4C 8D 40 08 0F 11 40 C8 0F 10 42 18 "
    "F2 0F 11 48 D8 F2 0F 10 4A 28 48 8D 50 C8 0F 11 40 E0 F2 0F 11 48 F0 E8";
constexpr size_t kTreePrologue = 7;  // mov rax,rsp; sub rsp,58h
constexpr char kWriteCmd[] =
    "40 53 48 83 EC 40 48 8B DA 41 B0 01 48 8B D1 48 8D 4C 24 20 E8 ?? ?? ?? ?? 48 8B 4B 08 FF 13 48 8D 4C 24 20 E8";
constexpr size_t kWritePrologue = 6;  // push rbx (REX); sub rsp,40h

using TreeFn = uint64_t (*)(void* sq, const double* bounds, uint64_t payload);
using WriteFn = uint64_t (*)(void* handle, void* const* cmd);

TreeFn g_treeOrig = nullptr;
WriteFn g_writeOrig = nullptr;
int g_mode = 0;
const uint8_t* g_base = nullptr;

// ---- per element (game thread only) ----
constexpr size_t kKeys = 1 << 16;
struct KeyEntry {
    uint64_t key;
    uint64_t frame;
    double bounds[6];
    uint64_t seqAtUpdate;  // the game thread's query count at this element's last update
    const void* ctx;       // the movement component ticking then (FarPawnMoveTicking)
    uint16_t inFrame;
    int16_t lastSig;
    bool hasBounds;
};
KeyEntry g_keys[2][kKeys];  // [0] tree updates, [1] write commands

// ---- call paths (mode 2) ----
constexpr int kDepth = 14;
constexpr int kSigs = 256;
struct Sig {
    uint64_t hash;
    uint32_t rva[kDepth];
    int n;
    uint64_t calls, repeats, overwritten, sameAsLast, sameInFrame;
};
Sig g_sigs[2][kSigs];
int g_sigCount[2] = {0, 0};

struct Counts {
    uint64_t calls, otherThread, newKeys, repeats, sameAsLast, sameInFrame, overflow, sigOverflow;
    uint64_t noQuery, ownOnly, foreign, foreignOtherPawn, sameTick, logFull;  // repeats: queries between the two updates
};

// ---- scene queries on the structure's class (game thread) ----
constexpr int kQSlots = 7;
const int kQSlotOff[kQSlots] = {-0xC8, -0xC0, -0xB8, -0xB0, -0x98, -0x10, -0x08};  // from the tree update's vtable slot
uint32_t g_qRva[kQSlots];
uint64_t g_qCalls[kQSlots], g_qInTick[kQSlots];
std::atomic<uint64_t> g_qOther[kQSlots];
uint64_t g_qSeq = 0, g_qBase = 0, g_qFrame = ~0ull;
constexpr uint32_t kQLog = 1 << 16;
const void* g_qCtx[kQLog];  // this frame's queries: the movement component ticking at each
Counts g_c[2];
std::atomic<uint64_t> g_other[2];
uint64_t g_frame0 = 0;
SRWLOCK g_lock = SRWLOCK_INIT;  // game thread writes, the monitor thread logs and clears

KeyEntry* Find(int k, uint64_t key, bool* fresh)
{
    size_t h = static_cast<size_t>((key * 0x9E3779B97F4A7C15ull) >> 48) & (kKeys - 1);
    for (int i = 0; i < 64; ++i, h = (h + 1) & (kKeys - 1)) {
        KeyEntry& e = g_keys[k][h];
        if (e.key == key) { *fresh = false; return &e; }
        if (e.key == 0) {
            e.key = key;
            e.frame = ~0ull;
            e.lastSig = -1;
            e.hasBounds = false;
            e.ctx = nullptr;
            e.seqAtUpdate = 0;
            *fresh = true;
            return &e;
        }
    }
    return nullptr;
}

int SigOf(int k, uint32_t first)
{
    void* frames[40];
    const USHORT got = RtlCaptureStackBackTrace(1, 40, frames, nullptr);
    const GameModule& game = Game();
    uint32_t rva[kDepth];
    int n = 0;
    if (first) rva[n++] = first;
    uint64_t hash = 1469598103934665603ull ^ first;
    for (USHORT i = 0; i < got && n < kDepth; ++i) {
        const uint8_t* at = static_cast<const uint8_t*>(frames[i]);
        if (!game.text.Contains(at)) continue;
        const uint8_t* fn = PrimaryFunction(game, at - 1);
        const uint32_t r = static_cast<uint32_t>((fn ? fn : at) - game.base);
        if (n && rva[n - 1] == r) continue;
        rva[n++] = r;
        hash = (hash ^ r) * 1099511628211ull;
    }
    for (int s = 0; s < g_sigCount[k]; ++s)
        if (g_sigs[k][s].hash == hash) return s;
    if (g_sigCount[k] >= kSigs) { ++g_c[k].sigOverflow; return -1; }
    Sig& s = g_sigs[k][g_sigCount[k]];
    memset(&s, 0, sizeof s);
    s.hash = hash;
    s.n = n;
    memcpy(s.rva, rva, sizeof(uint32_t) * n);
    return g_sigCount[k]++;
}

void Note(int k, uint64_t key, const double* bounds, uint32_t first)
{
    if (!LedgerIsGameThread()) {
        g_other[k].fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const uint64_t frame = NpcPacingFrame();
    AcquireSRWLockExclusive(&g_lock);
    Counts& c = g_c[k];
    ++c.calls;
    const int sig = g_mode >= 2 ? SigOf(k, first) : -1;
    if (sig >= 0) ++g_sigs[k][sig].calls;
    bool fresh = false;
    KeyEntry* e = Find(k, key, &fresh);
    if (!e) {
        ++c.overflow;
    } else {
        if (fresh) ++c.newKeys;
        if (e->frame != frame) { e->frame = frame; e->inFrame = 0; }
        const bool same = bounds && e->hasBounds && memcmp(e->bounds, bounds, sizeof e->bounds) == 0;
        const void* const ctx = FarPawnMoveTicking();
        if (e->inFrame > 0) {
            ++c.repeats;
            if (k == 0) {
                if (ctx && ctx == e->ctx) ++c.sameTick;
                if (g_qSeq == e->seqAtUpdate) ++c.noQuery;
                else if (e->seqAtUpdate < g_qBase || g_qSeq - g_qBase > kQLog) ++c.logFull;
                else {
                    bool foreign = false, otherPawn = false;
                    for (uint64_t q = e->seqAtUpdate; q < g_qSeq; ++q) {
                        const void* const qc = g_qCtx[q - g_qBase];
                        if (qc != e->ctx || !qc) foreign = true;
                        if (qc && qc != e->ctx) otherPawn = true;
                    }
                    ++(foreign ? c.foreign : c.ownOnly);
                    if (otherPawn) ++c.foreignOtherPawn;
                }
            }
            if (same) ++c.sameInFrame;
            if (sig >= 0) { ++g_sigs[k][sig].repeats; if (same) ++g_sigs[k][sig].sameInFrame; }
            if (e->lastSig >= 0) ++g_sigs[k][e->lastSig].overwritten;
        }
        if (same) { ++c.sameAsLast; if (sig >= 0) ++g_sigs[k][sig].sameAsLast; }
        if (bounds) { memcpy(e->bounds, bounds, sizeof e->bounds); e->hasBounds = true; }
        ++e->inFrame;
        e->lastSig = static_cast<int16_t>(sig);
        e->seqAtUpdate = g_qSeq;
        e->ctx = ctx;
    }
    ReleaseSRWLockExclusive(&g_lock);
}

// Called from the query thunks (all argument registers saved around the call).
void QueryNote(int slot)
{
    if (!LedgerIsGameThread()) {
        g_qOther[slot].fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const uint64_t frame = NpcPacingFrame();
    const void* const ctx = FarPawnMoveTicking();
    AcquireSRWLockExclusive(&g_lock);
    if (frame != g_qFrame) { g_qFrame = frame; g_qBase = g_qSeq; }
    if (g_qSeq - g_qBase < kQLog) g_qCtx[g_qSeq - g_qBase] = ctx;
    ++g_qSeq;
    ++g_qCalls[slot];
    if (ctx) ++g_qInTick[slot];
    ReleaseSRWLockExclusive(&g_lock);
}

// push rcx/rdx/r8/r9/rax (rsp 16-aligned after them); sub rsp,60h; save xmm0-3; ecx = slot; call QueryNote; restore;
// jmp [orig]: the query runs with its own registers and stack arguments.
uint8_t* QueryThunk(uint8_t* p, int slot, void* orig)
{
    static const uint8_t head[] = {0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x50, 0x48, 0x83, 0xEC, 0x60,
                                   0xF3, 0x0F, 0x7F, 0x44, 0x24, 0x20, 0xF3, 0x0F, 0x7F, 0x4C, 0x24, 0x30,
                                   0xF3, 0x0F, 0x7F, 0x54, 0x24, 0x40, 0xF3, 0x0F, 0x7F, 0x5C, 0x24, 0x50};
    static const uint8_t tail[] = {0xFF, 0xD0, 0xF3, 0x0F, 0x6F, 0x44, 0x24, 0x20, 0xF3, 0x0F, 0x6F, 0x4C, 0x24, 0x30,
                                   0xF3, 0x0F, 0x6F, 0x54, 0x24, 0x40, 0xF3, 0x0F, 0x6F, 0x5C, 0x24, 0x50,
                                   0x48, 0x83, 0xC4, 0x60, 0x58, 0x41, 0x59, 0x41, 0x58, 0x5A, 0x59,
                                   0xFF, 0x25, 0, 0, 0, 0};
    memcpy(p, head, sizeof head); p += sizeof head;
    *p++ = 0xB9; memcpy(p, &slot, 4); p += 4;  // mov ecx, slot
    *p++ = 0x48; *p++ = 0xB8;                  // mov rax, QueryNote
    void* const fn = reinterpret_cast<void*>(&QueryNote);
    memcpy(p, &fn, 8); p += 8;
    memcpy(p, tail, sizeof tail); p += sizeof tail;
    memcpy(p, &orig, 8); p += 8;
    return p;
}

uint64_t TreeHook(void* sq, const double* bounds, uint64_t payload)
{
    Note(0, payload, bounds, 0);
    return g_treeOrig(sq, bounds, payload);
}

uint64_t WriteHook(void* handle, void* const* cmd)
{
    uint32_t inv = 0;
    if (cmd && Game().text.Contains(cmd[0])) inv = static_cast<uint32_t>(static_cast<const uint8_t*>(cmd[0]) - Game().base);
    Note(1, reinterpret_cast<uint64_t>(handle), nullptr, inv);
    return g_writeOrig(handle, cmd);
}

uint8_t* Detour(uint8_t* fn, size_t n, void* hook, uint8_t* slot)
{
    const uint8_t jmpAbs[6] = {0xFF, 0x25, 0, 0, 0, 0};
    uint8_t* const resume = fn + n;
    memcpy(slot, fn, n);
    memcpy(slot + n, jmpAbs, 6);
    memcpy(slot + n + 6, &resume, 8);
    memcpy(slot + 32, jmpAbs, 6);
    memcpy(slot + 32 + 6, &hook, 8);
    return slot;
}

bool Jump(uint8_t* fn, size_t n, uint8_t* target)
{
    const int64_t rel = target - (fn + 5);
    if (rel != static_cast<int32_t>(rel) || n < 5 || n > 8) return false;
    uint8_t jmp[8] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90};
    const int32_t rel32 = static_cast<int32_t>(rel);
    memcpy(jmp + 1, &rel32, 4);
    return WriteCode(fn, std::span<const uint8_t>(jmp, n));
}

uint8_t* One(const GameModule& game, const char* pattern, const char* what)
{
    const auto hits = FindAll(game.text, ParsePattern(pattern));
    if (hits.size() == 1) return hits[0];
    Log("[bodysync] %s %s", what, hits.empty() ? "not found" : "ambiguous");
    return nullptr;
}

}  // namespace

bool InstallBodySyncProbe(bool beforeGameCode, int mode)
{
    if (!beforeGameCode) {
        Log("[bodysync] BodySyncProbe: start-up only (version.dll route), not installed");
        return false;
    }
    const GameModule& game = Game();
    g_base = game.base;
    g_mode = mode;
    uint8_t* const tree = One(game, kTreeUpdate, "the scene-query element update");
    uint8_t* const write = One(game, kWriteCmd, "the physics write-command runner");
    if (!tree || !write) {
        Log("[bodysync] BodySyncProbe not installed");
        return false;
    }
    uint8_t* const region = AllocNearGame(128);
    if (!region) {
        Log("[bodysync] no memory near the game's code: not installed");
        return false;
    }
    g_treeOrig = reinterpret_cast<TreeFn>(Detour(tree, kTreePrologue, reinterpret_cast<void*>(&TreeHook), region));
    g_writeOrig = reinterpret_cast<WriteFn>(Detour(write, kWritePrologue, reinterpret_cast<void*>(&WriteHook), region + 64));
    DWORD old;
    VirtualProtect(region, 128, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), region, 128);
    if (!Jump(tree, kTreePrologue, region + 32) || !Jump(write, kWritePrologue, region + 96)) {
        Log("[bodysync] could not place the detours: not installed");
        return false;
    }
    // the queries: the vtable holding the tree update (one slot in .rdata); each query slot -> a counting thunk
    void** slot0 = nullptr;
    int slots = 0;
    for (uint8_t* q = game.rdata.begin; q + 8 <= game.rdata.end; q += 8)
        if (*reinterpret_cast<uint8_t**>(q) == tree) { slot0 = reinterpret_cast<void**>(q); ++slots; }
    if (slots == 1) {
        uint8_t* const code = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        uint8_t* p = code;
        void* thunk[kQSlots] = {};
        for (int i = 0; code && i < kQSlots; ++i) {
            void* const target = slot0[kQSlotOff[i] / 8];
            if (!game.text.Contains(target)) { Log("[bodysync] query slot %d: not code", i); continue; }
            g_qRva[i] = static_cast<uint32_t>(static_cast<uint8_t*>(target) - game.base);
            thunk[i] = p;
            p = QueryThunk(p, i, target);
        }
        if (code) {
            DWORD o2;
            VirtualProtect(code, 4096, PAGE_EXECUTE_READ, &o2);
            FlushInstructionCache(GetCurrentProcess(), code, 4096);
            int n = 0;
            for (int i = 0; i < kQSlots; ++i)
                if (thunk[i] && WritePointer(&slot0[kQSlotOff[i] / 8], thunk[i])) ++n;
            Log("[bodysync] %d query slots counted (vtable slot of the update at exe+0x%X)", n,
                static_cast<unsigned>(reinterpret_cast<uint8_t*>(slot0) - game.base));
        }
    } else Log("[bodysync] the tree update's vtable slot: %d found, queries not counted", slots);
    Log("[bodysync] BodySyncProbe installed (mode %d): scene-query element update exe+0x%X, write-command runner exe+0x%X",
        mode, static_cast<unsigned>(tree - game.base), static_cast<unsigned>(write - game.base));
    return true;
}

void LogBodySyncProbeStats()
{
    if (!g_treeOrig) return;
    AcquireSRWLockExclusive(&g_lock);
    const uint64_t frame = NpcPacingFrame();
    const double frames = frame > g_frame0 ? static_cast<double>(frame - g_frame0) : 1.0;
    static const char* const kName[2] = {"tree update", "write cmd"};
    for (int k = 0; k < 2; ++k) {
        Counts& c = g_c[k];
        c.otherThread = g_other[k].exchange(0, std::memory_order_relaxed);
        if (!c.calls && !c.otherThread) continue;
        Log("[bodysync] %s: %.1f game-thread calls/frame over %.0f frames (%.1f other-thread), new elements %llu, repeats in a "
            "frame %.1f/frame (%.1f%%), same bounds as the element's last %.1f/frame (%.1f%%; within a frame %.1f), table "
            "full %llu, paths over %llu",
            kName[k], c.calls / frames, frames, c.otherThread / frames, static_cast<unsigned long long>(c.newKeys),
            c.repeats / frames, c.calls ? 100.0 * c.repeats / c.calls : 0.0, c.sameAsLast / frames,
            c.calls ? 100.0 * c.sameAsLast / c.calls : 0.0, c.sameInFrame / frames, static_cast<unsigned long long>(c.overflow),
            static_cast<unsigned long long>(c.sigOverflow));
        // the 12 busiest paths
        int order[kSigs];
        const int n = g_sigCount[k];
        for (int i = 0; i < n; ++i) order[i] = i;
        for (int i = 0; i < n; ++i)
            for (int j = i + 1; j < n; ++j)
                if (g_sigs[k][order[j]].calls > g_sigs[k][order[i]].calls) std::swap(order[i], order[j]);
        for (int i = 0; i < n && i < 12; ++i) {
            const Sig& s = g_sigs[k][order[i]];
            if (!s.calls) break;
            char path[kDepth * 12 + 1];
            int len = 0;
            for (int j = 0; j < s.n && len < static_cast<int>(sizeof path) - 12; ++j)
                len += snprintf(path + len, sizeof path - len, "%s%X", j ? "<" : "", s.rva[j]);
            Log("[bodysync]   %s path %d: %.1f/frame, repeat %.1f, later overwritten %.1f, same as last %.1f (in frame %.1f): %s",
                kName[k], i, s.calls / frames, s.repeats / frames, s.overwritten / frames, s.sameAsLast / frames,
                s.sameInFrame / frames, path);
        }
        if (k == 0 && c.repeats)
            Log("[bodysync] tree update repeats: no query between %.1f%%, only queries in the same movement tick %.1f%%, other "
                "queries between %.1f%% (another character's movement among them %.1f%%), log full %.1f%%; both updates in the same "
                "movement tick %.1f%%",
                100.0 * c.noQuery / c.repeats, 100.0 * c.ownOnly / c.repeats, 100.0 * c.foreign / c.repeats,
                100.0 * c.foreignOtherPawn / c.repeats,
                100.0 * c.logFull / c.repeats, 100.0 * c.sameTick / c.repeats);
        memset(&g_c[k], 0, sizeof g_c[k]);
        for (int i = 0; i < n; ++i) {
            Sig& s = g_sigs[k][i];
            s.calls = s.repeats = s.overwritten = s.sameAsLast = s.sameInFrame = 0;
        }
    }
    for (int i = 0; i < kQSlots; ++i) {
        const uint64_t other = g_qOther[i].exchange(0, std::memory_order_relaxed);
        if (g_qCalls[i] || other)
            Log("[bodysync] query slot %d exe+0x%X: %.1f game-thread calls/frame (%.1f in a movement tick), %.1f other-thread",
                i, g_qRva[i], g_qCalls[i] / frames, g_qInTick[i] / frames, other / frames);
        g_qCalls[i] = g_qInTick[i] = 0;
    }
    g_frame0 = frame;
    ReleaseSRWLockExclusive(&g_lock);
}

}  // namespace opf
