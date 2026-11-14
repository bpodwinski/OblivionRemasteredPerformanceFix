// PerceptionLosPrefetch ([Fixes] PerceptionLosPrefetch, default 0 = off; 2026-11-14, fork bpodwinski; not measured yet).
//
// The problem: every NPC runs a perception round five times a second on the game thread (a timer method of the paired pawn;
// this build exe+0x485FA60). The round lists the world's pawns, and for every pawn that passes its filters and is within
// its perception range it calls a line-of-sight helper (exe+0x485A170): up to three LineTraceSingleByChannel (channel 17)
// from the NPC's eyes to the other pawn (a component, then two skeleton sockets), ignoring both pawns' attached and child
// actors. In a crowd of 40 NPCs these rounds are the largest single item of the game thread (tick split, 2026-11-14:
// ~0.5 ms per round, ~30 % of the timed game-thread work).
//
// The change: at the start of a round, right after the game built its pawn list, the line-of-sight helper is run on
// helper threads (and this thread) for every pawn the round's loop would reach the helper with, using the same checks in
// the same way (not null, of the round's pawn class, not the NPC itself, a paired old-engine object, its root within the
// range computed from the same settings floats in the same float order), the game thread waiting until all are done.
// Then the game's loop runs unchanged, in its own order; at its helper call site (redirected to us) a call for this round's
// NPC, a precomputed pawn and the stock arguments (3 tries, flag 1, result byte 3) gets the precomputed return value and
// result byte; anything else runs the game's helper.
//
// Why the result is the same: the helper only reads (its writes go to its own stack and to the caller's result byte;
// checked in its code: no other store outside its frame). Between the precomputation and the stock call, the round's loop
// does only three things per pawn: a virtual filter on the NPC (vtable +0xA90), an insert into the NPC's own perception map
// (+0xC80), and a command to the old engine's queue (exe+0x4865DB0: a 0x30-byte command "X sees Y at level N"); none of
// them moves an actor, changes an attachment or writes the physics scene the traces read. The game thread is blocked in
// the round while the helpers trace, so no game-thread write can interleave (no garbage collection either: it runs on the
// game thread). Other threads (animation workers moving bones, the physics thread) run during the stock round too, so a
// trace answered microseconds earlier can only differ by a race the game does not order (RigTraceReuse's argument).
// UE runs scene queries off the game thread itself (async traces under the scene read lock).
// The precomputation also traces pawns the virtual filter would reject; that costs helper time only (read-only helper).
//
// [Fixes] PerceptionLosPrefetch: 0 = not installed (start-up), 1 = on, 2 = verify (the game's helper runs anyway and its
// result is compared with the precomputed one; the game's result is used); installed only if not 0 at start-up (version.dll
// route), then live. Off while ParallelHelpersAllowed is false. Hidden: PerceptionLosPrefetchMin (4: precomputed pawns
// needed for a batch, live), PerceptionLosPrefetchThreads (4: helper threads, 0..8, start-up).
//
// Where (all found by pattern and checked; any mismatch = not installed, with a log line):
// - R: the round after its pawn list (exe+0x485FAF4): the list at [rsp+60h] {data, count}, the pawn class getter call,
//   the settings getter call (exe+0x4711300, no arguments) whose call we redirect to a stub that calls it and then us,
//   the range constant (movss xmm7).
// - L: the loop's checks (R+0x5E): class chain at +0x10 / +0x30 / +0x38, self compare, old-engine object +0x7B8 / +0x120,
//   the range floats +0x4E8 / +0x4EC and their float order, the virtual filter +0xA90, the other's root +0x1A0 and its
//   location +0x280 / +0x290.
// - C: the helper's call site (R+0x1CC): r9d 3, [rsp+20h] 1, r8 = the result byte at [rsp+30h], rdx other, rcx self.
// - H: the helper itself (its prologue up to the result byte's first store), equal to C's call target.
// - the NPC's own root +0x1A0 at R+0x145.
#include "common.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace opf {
namespace {

constexpr char kRound[] =
    "48 89 87 48 0C 00 00 E8 ?? ?? ?? ?? 48 8B CF 48 8B D8 E8 ?? ?? ?? ?? 4C 8D 44 24 60 48 8B D3 48 8B C8 E8 ?? ?? ?? ?? "
    "E8 ?? ?? ?? ?? 4C 8B 74 24 60 48 63 4C 24 68 4D 8D 2C CE 4D 3B F5 0F 84 ?? ?? ?? ?? 0F 29 BC 24 20 01 00 00 48 8B F0 "
    "F3 0F 10 3D ?? ?? ?? ??";
constexpr size_t kRClassCall = 0x07, kRSettingsCall = 0x27, kRConst = 0x4E, kRLoop = 0x5E, kRSelfRoot = 0x145, kRCall = 0x1CC;
constexpr char kLoop[] =
    "49 8B 1E 48 85 DB 0F 84 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B 53 10 4C 8D 40 30 48 63 40 38 3B 42 38 0F 8F ?? ?? ?? ?? 48 8B C8 "
    "48 8B 42 30 4C 39 04 C8 0F 85 ?? ?? ?? ?? 48 3B DF 0F 84 ?? ?? ?? ?? 48 8B 83 B8 07 00 00 48 83 B8 20 01 00 00 00 0F 84 "
    "?? ?? ?? ?? F3 0F 10 8E E8 04 00 00 41 B7 03 F3 0F 10 86 EC 04 00 00 0F 28 F1 48 8B 07 48 8B D3 F3 0F 59 F7 48 8B CF 45 "
    "32 E4 44 88 7C 24 30 F3 0F 59 C8 F3 0F 59 F0 F3 0F 59 F1 FF 90 90 0A 00 00 84 C0 0F 84 ?? ?? ?? ?? 48 8B 83 A0 01 00 00 "
    "F2 0F 10 15 ?? ?? ?? ?? 0F 10 1D ?? ?? ?? ?? 48 85 C0 74 1D 0F 10 80 80 02 00 00";
constexpr size_t kLClassCall = 0x0C;
constexpr char kSelfRoot[] = "48 8B 87 A0 01 00 00";
constexpr char kCall[] =
    "66 0F 2F E0 73 25 41 B9 03 00 00 00 C6 44 24 20 01 4C 8D 44 24 30 48 8B D3 48 8B CF E8 ?? ?? ?? ?? 44 0F B6 7C 24 30 "
    "44 0F B6 E0";
constexpr size_t kCCall = 0x1C;
constexpr char kHelper[] =
    "4C 8B DC 55 53 56 57 41 57 49 8D AB A8 FE FF FF 48 81 EC 30 02 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 85 10 01 00 00 "
    "41 8B F9 49 8B F0 48 8B DA 4C 8B F9 48 85 D2 0F 84 ?? ?? ?? ?? 48 83 BA B8 07 00 00 00 0F 84 ?? ?? ?? ?? 4D 89 63 D0 48 "
    "8D 4D B0 4D 89 73 C8 41 C6 00 03";

// layout (pinned by the patterns above)
constexpr size_t kClassPtr = 0x10, kChain = 0x30, kDepth = 0x38, kPaired = 0x7B8, kPairedObj = 0x120, kRoot = 0x1A0, kLoc = 0x280,
                 kRangeA = 0x4E8, kRangeB = 0x4EC;
constexpr int kTries = 3;
constexpr uint8_t kFlag = 1, kOutInit = 3;
constexpr int kMaxJobs = 256;

template <class T> T Rd(const void* p, size_t off)
{
    T v;
    memcpy(&v, static_cast<const uint8_t*>(p) + off, sizeof v);
    return v;
}

using HelperFn = uint8_t (*)(uint8_t* self, uint8_t* other, uint8_t* out, int tries, uint8_t flag);
using ClassFn = const uint8_t* (*)();
HelperFn g_helper = nullptr;
ClassFn g_class = nullptr;
float g_rangeConst = 1.0f;
bool g_installed = false;

std::atomic<int> g_mode{0};  // 0 off, 1 serve, 2 verify
std::atomic<int> g_min{4};
int g_threads = 4;

struct Job {
    uint8_t* other;
    std::atomic<int> state;  // 0 queued, 1 done
    uint8_t ret, out;
    bool used;
};
Job g_jobs[kMaxJobs];
int g_jobCount = 0;
uint8_t* g_roundSelf = nullptr;
DWORD g_roundThread = 0;
int g_cursor = 0;
// gen << 32 | next << 16 | count: the helpers claim jobs by position
std::atomic<uint64_t> g_word{0};
uint32_t g_gen = 0;
HANDLE g_sem = nullptr;
int g_helpers = 0;
bool g_helpersTried = false;

struct Stats {
    std::atomic<uint64_t> rounds{0}, batches{0}, jobs{0}, served{0}, verified{0}, diff{0}, fallback{0}, unused{0}, wallUs{0},
        maxWallUs{0};
};
Stats g_s;

int Claim()
{
    uint64_t w = g_word.load(std::memory_order_acquire);
    for (;;) {
        const uint32_t next = static_cast<uint32_t>(w >> 16) & 0xFFFF, count = static_cast<uint32_t>(w) & 0xFFFF;
        if (next >= count) return -1;
        const uint64_t nw = (w & ~0xFFFF0000ull) | (static_cast<uint64_t>(next + 1) << 16);
        if (g_word.compare_exchange_weak(w, nw, std::memory_order_acq_rel, std::memory_order_acquire)) return static_cast<int>(next);
    }
}

void RunJob(Job& j)
{
    uint8_t out = kOutInit;
    j.ret = g_helper(g_roundSelf, j.other, &out, kTries, kFlag);
    j.out = out;
    j.state.store(1, std::memory_order_release);
    WakeWordAll(&j.state);
}

DWORD WINAPI Helper(void*)
{
    for (;;) {
        WaitForSingleObject(g_sem, INFINITE);
        int i;
        while ((i = Claim()) >= 0) RunJob(g_jobs[i]);
    }
}

void StartHelpers()
{
    g_helpersTried = true;
    g_sem = CreateSemaphoreW(nullptr, 0, 1 << 20, nullptr);
    if (!g_sem) return;
    for (int i = 0; i < g_threads; ++i) {
        HANDLE t = CreateThread(nullptr, 1 << 20, Helper, nullptr, 0, nullptr);
        if (!t) break;
        DescribeThread(t, L"OPF PerceptionLosPrefetch");
        CloseHandle(t);
        ++g_helpers;
    }
    Log("[perclos] PerceptionLosPrefetch: %d helper threads started", g_helpers);
}

// The class chain check of the round's loop (L: [obj+0x10] class, its chain at +0x30, depth at +0x38).
bool IsA(const uint8_t* obj, const uint8_t* cls)
{
    const uint8_t* const oc = Rd<const uint8_t*>(obj, kClassPtr);
    if (!oc) return false;
    const int depth = Rd<int>(cls, kDepth);
    if (depth > Rd<int>(oc, kDepth)) return false;
    const uint8_t* const chain = Rd<const uint8_t*>(oc, kChain);
    return chain && Rd<const uint8_t*>(chain, static_cast<size_t>(depth) * 8) == cls + kChain;
}

double QpcUs()
{
    static int64_t freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f.QuadPart; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) * 1e6 / static_cast<double>(freq);
}

// Runs the queued jobs on the helpers and this thread; returns when all are done.
void RunBatch()
{
    if (!g_helpersTried) StartHelpers();
    ++g_gen;
    g_word.store((static_cast<uint64_t>(g_gen) << 32) | static_cast<uint64_t>(g_jobCount), std::memory_order_release);
    if (g_sem && g_helpers > 0 && g_jobCount > 1) ReleaseSemaphore(g_sem, std::min(g_helpers, g_jobCount - 1), nullptr);
    int i;
    while ((i = Claim()) >= 0) RunJob(g_jobs[i]);
    for (int k = 0; k < g_jobCount; ++k) {
        Job& j = g_jobs[k];
        int spin = 0;
        uint64_t deadline = 0;
        while (j.state.load(std::memory_order_acquire) == 0) {
            if (SpinPhase(spin, deadline)) continue;
            const int seen = 0;
            BlockOnWord(&j.state, &seen, sizeof seen);
        }
    }
    g_word.store(static_cast<uint64_t>(g_gen) << 32, std::memory_order_release);
}

void AddJob(uint8_t* other)
{
    Job& j = g_jobs[g_jobCount++];
    j.other = other;
    j.state.store(0, std::memory_order_relaxed);
    j.ret = 0;
    j.out = kOutInit;
    j.used = false;
}

void Inc(std::atomic<uint64_t>& c, uint64_t n = 1) { c.fetch_add(n, std::memory_order_relaxed); }

// Called by the stub at the round's settings getter (game thread), after the getter: the NPC, its pawn list, the settings.
void Prefetch(uint8_t* self, const uint8_t* list, const uint8_t* settings)
{
    // the previous round's state goes first: every round passes here before its loop
    for (int k = 0; k < g_jobCount; ++k)
        if (!g_jobs[k].used) Inc(g_s.unused);
    g_jobCount = 0;
    g_roundSelf = nullptr;
    g_cursor = 0;
    Inc(g_s.rounds);
    if (g_mode.load(std::memory_order_relaxed) == 0 || !self || !list || !settings) return;
    uint8_t* const* const data = Rd<uint8_t* const*>(list, 0);
    const int n = Rd<int>(list, 8);
    if (!data || n <= 0) return;
    const uint8_t* const sroot = Rd<const uint8_t*>(self, kRoot);
    if (!sroot) return;
    const uint8_t* const cls = g_class();
    if (!cls) return;
    const double sx = Rd<double>(sroot, kLoc), sy = Rd<double>(sroot, kLoc + 8), sz = Rd<double>(sroot, kLoc + 16);
    // the loop's range, in its float order: x6 = a; x1 = a; x6 *= c; x1 *= b; x6 *= b; x6 *= x1; compared as a double
    const float a = Rd<float>(settings, kRangeA), b = Rd<float>(settings, kRangeB);
    float x6 = a, x1 = a;
    x6 *= g_rangeConst;
    x1 *= b;
    x6 *= b;
    x6 *= x1;
    const double range = static_cast<double>(x6);
    for (int i = 0; i < n && g_jobCount < kMaxJobs; ++i) {
        uint8_t* const other = data[i];
        if (!other || !IsA(other, cls) || other == self) continue;
        const uint8_t* const paired = Rd<const uint8_t*>(other, kPaired);
        if (!paired || !Rd<const void*>(paired, kPairedObj)) continue;
        const uint8_t* const oroot = Rd<const uint8_t*>(other, kRoot);
        if (!oroot) continue;  // the loop uses a default location then: not precomputed (the game's helper runs)
        const double dx = Rd<double>(oroot, kLoc) - sx, dy = Rd<double>(oroot, kLoc + 8) - sy, dz = Rd<double>(oroot, kLoc + 16) - sz;
        double d = dx * dx;
        d += dy * dy;
        d += dz * dz;
        if (!(d < range)) continue;
        AddJob(other);
    }
    if (g_jobCount < std::max(1, g_min.load(std::memory_order_relaxed))) {
        g_jobCount = 0;
        return;
    }
    const double t0 = QpcUs();
    g_roundSelf = self;
    g_roundThread = GetCurrentThreadId();
    RunBatch();
    const uint64_t us = static_cast<uint64_t>(QpcUs() - t0);
    Inc(g_s.batches);
    Inc(g_s.jobs, static_cast<uint64_t>(g_jobCount));
    Inc(g_s.wallUs, us);
    if (us > g_s.maxWallUs.load(std::memory_order_relaxed)) g_s.maxWallUs.store(us, std::memory_order_relaxed);
}

// The loop's helper call site (redirected here): serve a precomputed answer, else the game's helper.
uint8_t HelperHook(uint8_t* self, uint8_t* other, uint8_t* out, int tries, uint8_t flag)
{
    const int mode = g_mode.load(std::memory_order_relaxed);
    if (mode != 0 && g_jobCount > 0 && self == g_roundSelf && tries == kTries && flag == kFlag && out && *out == kOutInit &&
        GetCurrentThreadId() == g_roundThread) {
        Job* hit = nullptr;
        for (int k = 0; k < g_jobCount && !hit; ++k) {
            Job& j = g_jobs[(g_cursor + k) % g_jobCount];
            if (!j.used && j.other == other) hit = &j;
        }
        if (hit) {
            hit->used = true;
            g_cursor = static_cast<int>((hit - g_jobs) + 1) % g_jobCount;
            if (mode == 2) {
                const uint8_t r = g_helper(self, other, out, tries, flag);
                Inc(g_s.verified);
                if (r != hit->ret || *out != hit->out) Inc(g_s.diff);
                return r;
            }
            *out = hit->out;
            Inc(g_s.served);
            return hit->ret;
        }
    }
    Inc(g_s.fallback);
    return g_helper(self, other, out, tries, flag);
}

struct Sites {
    uint8_t *round, *settingsCall, *callSite, *helper, *settings, *classFn;
    float rangeConst;
};

const char* Discover(const GameModule& game, Sites& s)
{
    const auto r = FindAll(game.text, ParsePattern(kRound));
    if (r.size() != 1) return "the perception round's list and settings calls not found exactly once";
    s.round = r[0];
    if (!MatchesAt(s.round + kRLoop, ParsePattern(kLoop))) return "the round's loop checks differ";
    if (!MatchesAt(s.round + kRSelfRoot, ParsePattern(kSelfRoot))) return "the NPC's own root load differs";
    if (!MatchesAt(s.round + kRCall, ParsePattern(kCall))) return "the line-of-sight call site differs";
    s.classFn = RipTarget(s.round + kRClassCall, 1, 5);
    if (RipTarget(s.round + kRLoop + kLClassCall, 1, 5) != s.classFn) return "the loop's class getter is another function";
    s.settingsCall = s.round + kRSettingsCall;
    s.settings = RipTarget(s.settingsCall, 1, 5);
    const uint8_t* const cst = RipTarget(s.round + kRConst, 4, 8);
    if (!game.rdata.Contains(cst) && !game.data.Contains(cst)) return "the range constant is not in the exe's data";
    memcpy(&s.rangeConst, cst, 4);
    s.callSite = s.round + kRCall + kCCall;
    s.helper = RipTarget(s.callSite, 1, 5);
    if (!game.text.Contains(s.helper) || !MatchesAt(s.helper, ParsePattern(kHelper))) return "the line-of-sight helper differs";
    if (!game.text.Contains(s.settings) || !game.text.Contains(s.classFn)) return "a getter is outside the exe's code";
    return nullptr;
}

bool PatchCall(uint8_t* site, const uint8_t* to)
{
    const int64_t rel = to - (site + 5);
    if (rel != static_cast<int32_t>(rel)) return false;
    uint8_t bytes[5] = {0xE8};
    const int32_t rel32 = static_cast<int32_t>(rel);
    memcpy(bytes + 1, &rel32, 4);
    return WriteCode(site, bytes);
}

uint64_t g_statFrame = 0;
const char* ModeText(int mode)
{
    return mode == 1 ? "a perception round's line-of-sight checks are precomputed on helper threads and served in the game's order"
         : mode == 2 ? "verify: the game's line-of-sight helper also runs and is compared with the precomputed answer"
                     : "every line-of-sight check runs on the game thread, as in the game";
}

}  // namespace

// Self-test: the RVAs of the round match, the call site and the helper (0 = not found); no writes.
const char* PerceptionLosTestDiscover(const GameModule& game, uint32_t* round, uint32_t* callSite, uint32_t* helper)
{
    Sites s{};
    const char* why = Discover(game, s);
    if (!why) {
        if (round) *round = static_cast<uint32_t>(s.round - game.base);
        if (callSite) *callSite = static_cast<uint32_t>(s.callSite - game.base);
        if (helper) *helper = static_cast<uint32_t>(s.helper - game.base);
    }
    return why;
}

bool InstallPerceptionLosPrefetch(bool beforeGameCode, int threads)
{
    if (g_installed) return true;
    if (!beforeGameCode) {
        Log("[perclos] PerceptionLosPrefetch: only installed when the game starts (version.dll route; not on the OBSE64 route): not installed");
        return false;
    }
    const GameModule& game = Game();
    Sites s{};
    if (const char* why = Discover(game, s)) {
        Log("[perclos] %s: PerceptionLosPrefetch not installed", why);
        return false;
    }
    uint8_t* const region = AllocNearGame(128);
    if (!region) { Log("[perclos] no memory near the game's code: PerceptionLosPrefetch not installed"); return false; }
    // region +0: the round stub (called instead of the settings getter; the caller's frame is at our entry rsp + 8)
    //   sub rsp,28h; mov rax,getter; call rax; mov [rsp+20h],rax; mov rcx,rdi; lea rdx,[rsp+90h]; mov r8,rax;
    //   mov rax,Prefetch; call rax; mov rax,[rsp+20h]; add rsp,28h; ret
    // region +64: jmp [rip+0]; the helper hook's address
    uint8_t* p = region;
    auto put = [&](std::initializer_list<uint8_t> b) { for (uint8_t x : b) *p++ = x; };
    auto put64 = [&](const void* v) { memcpy(p, &v, 8); p += 8; };
    put({0x48, 0x83, 0xEC, 0x28});
    put({0x48, 0xB8}); put64(s.settings);
    put({0xFF, 0xD0});
    put({0x48, 0x89, 0x44, 0x24, 0x20});
    put({0x48, 0x8B, 0xCF});
    put({0x48, 0x8D, 0x94, 0x24, 0x90, 0x00, 0x00, 0x00});
    put({0x4C, 0x8B, 0xC0});
    put({0x48, 0xB8}); put64(reinterpret_cast<const void*>(&Prefetch));
    put({0xFF, 0xD0});
    put({0x48, 0x8B, 0x44, 0x24, 0x20});
    put({0x48, 0x83, 0xC4, 0x28});
    put({0xC3});
    uint8_t* const jmp = region + 64;
    p = jmp;
    put({0xFF, 0x25, 0x00, 0x00, 0x00, 0x00});
    put64(reinterpret_cast<const void*>(&HelperHook));
    DWORD old;
    if (!VirtualProtect(region, 128, PAGE_EXECUTE_READ, &old)) {
        VirtualFree(region, 0, MEM_RELEASE);
        Log("[perclos] could not make the stubs executable: PerceptionLosPrefetch not installed");
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), region, 128);
    g_helper = reinterpret_cast<HelperFn>(s.helper);
    g_class = reinterpret_cast<ClassFn>(s.classFn);
    g_rangeConst = s.rangeConst;
    g_threads = std::clamp(threads, 0, 8);
    // the call site first: alone it only forwards to the game's helper (no round state yet)
    if (!PatchCall(s.callSite, jmp)) { Log("[perclos] could not patch the line-of-sight call: PerceptionLosPrefetch not installed"); return false; }
    if (!PatchCall(s.settingsCall, region)) {
        Log("[perclos] could not patch the round's settings call (the call site forwards only): PerceptionLosPrefetch not installed");
        return false;
    }
    g_installed = true;
    Log("[perclos] perception round exe+0x%X (settings call exe+0x%X), line-of-sight call exe+0x%X -> helper exe+0x%X hooked; "
        "range constant %g; %d helper threads (on first use); live",
        static_cast<unsigned>(s.round - game.base), static_cast<unsigned>(s.settingsCall - game.base),
        static_cast<unsigned>(s.callSite - game.base), static_cast<unsigned>(s.helper - game.base), static_cast<double>(s.rangeConst),
        g_threads);
    g_statFrame = NpcPacingFrame();
    return true;
}

// Live (monitor thread): 0 off, 1 serve, 2 verify; no-op until installed.
void SetPerceptionLosPrefetch(int mode, int minJobs, bool helpersAllowed)
{
    if (!g_installed) return;
    mode = mode == 1 ? 1 : mode == 2 ? 2 : 0;
    if (!helpersAllowed) mode = 0;
    g_min = std::clamp(minJobs, 1, kMaxJobs);
    if (g_mode.load() == mode) return;
    g_mode = mode;
    Log("[perclos] PerceptionLosPrefetch=%d: %s%s", mode, ModeText(mode), helpersAllowed ? "" : " (ParallelHelpersAllowed: no)");
}

void LogPerceptionLosPrefetchStats()
{
    if (!g_installed) return;
    const uint64_t frame = NpcPacingFrame();
    const double frames = static_cast<double>(frame - g_statFrame);
    g_statFrame = frame;
    const uint64_t rounds = g_s.rounds.exchange(0), batches = g_s.batches.exchange(0), jobs = g_s.jobs.exchange(0),
                   served = g_s.served.exchange(0), verified = g_s.verified.exchange(0), diff = g_s.diff.exchange(0),
                   fallback = g_s.fallback.exchange(0), unused = g_s.unused.exchange(0), wallUs = g_s.wallUs.exchange(0),
                   maxWallUs = g_s.maxWallUs.exchange(0);
    if (!rounds) return;
    Log("[perclos] since last report (%.0f frames): %llu perception rounds, %llu batched (%llu checks precomputed, %.1f per batch, "
        "game thread blocked %.2f ms in total, max %.3f ms), %llu served, %llu verified (%llu different), %llu ran as in the game, "
        "%llu precomputed but not asked (PerceptionLosPrefetch=%d)",
        frames, (unsigned long long)rounds, (unsigned long long)batches, (unsigned long long)jobs,
        batches ? static_cast<double>(jobs) / static_cast<double>(batches) : 0.0, static_cast<double>(wallUs) / 1000.0,
        static_cast<double>(maxWallUs) / 1000.0, (unsigned long long)served, (unsigned long long)verified, (unsigned long long)diff,
        (unsigned long long)fallback, (unsigned long long)unused, g_mode.load());
}

// ---- self-test: the batch and the serve with a fake helper and fake pawns ----
void PerceptionLosTestSetup(void* fakeHelper, void* fakeClass, float rangeConst, int mode, int minJobs, int threads)
{
    g_helper = reinterpret_cast<HelperFn>(fakeHelper);
    g_class = reinterpret_cast<ClassFn>(fakeClass);
    g_rangeConst = rangeConst;
    g_mode = mode;
    g_min = minJobs;
    g_threads = threads;
}
void PerceptionLosTestPrefetch(uint8_t* self, const uint8_t* list, const uint8_t* settings) { Prefetch(self, list, settings); }
uint8_t PerceptionLosTestCall(uint8_t* self, uint8_t* other, uint8_t* out, int tries, uint8_t flag)
{
    return HelperHook(self, other, out, tries, flag);
}
void PerceptionLosTestCounts(uint64_t* served, uint64_t* verified, uint64_t* diff, uint64_t* fallback, uint64_t* jobs)
{
    if (served) *served = g_s.served.load();
    if (verified) *verified = g_s.verified.load();
    if (diff) *diff = g_s.diff.load();
    if (fallback) *fallback = g_s.fallback.load();
    if (jobs) *jobs = g_s.jobs.load();
}
}  // namespace opf
