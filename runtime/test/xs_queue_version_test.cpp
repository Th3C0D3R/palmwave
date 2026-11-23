// Versioned console queue layout of the Xbox-ABI queue proxy (runtime/shims/d3d12_xs_device.cpp, "Versioned console
// queue layout"), over synthetic COM objects: no D3D12 runtime, no GPU, no game data.
//
// A fake PC device creates fake PC queues and fences; the real device proxy wraps them, and every call below goes
// through the proxy's own vtable, so the routing under test is the one a title reaches. Covered:
//   - version 0 (default argument): the PC queue layout, including native C++ calls through ID3D12CommandQueue
//     (Signal, GetDesc struct return, ExecuteCommandLists) and the refusal of slots 20, 32 and 35;
//   - version 0x20004 (modern): Signal 16 / Wait 17 with the fence proxy unwrapped, GetTimestampFrequency 18,
//     GetClockCalibration 19, GetDesc 20 in the PC form and the swapped console form (buffer in RCX, queue in RDX),
//     ExecuteCommandListsX 35 forwarding count and lists unchanged, its refusals (zero count, null array, null
//     entry, non-list proxies, non-zero frame word or flags) with no backend call, PresentX 32 and the other refused
//     slots (8..15 where the public sources disagree, 21, 23..25, 33, 36);
//   - version 0x20003 (legacy): PC forwarding plus PresentX 30 refused; version 0x20005: everything past 7 refused;
//   - COM identity, GetDevice, reference counts and final release of every fake object.
// This fixture exercises CPU routing only; see docs/BLOCKERS.md ("Queue PresentX bridge") for what is not covered.
#include "../shims/d3d12_xs_device.h"

#include <d3d12.h>
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace xs = rt::d3d12_xs;

namespace {

int g_failures = 0;
#define EXPECT(cond)                                                                \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::printf("xs_queue_version line %d: %s\n", __LINE__, #cond);         \
            ++g_failures;                                                           \
        }                                                                           \
    } while (0)

// ---- Fake PC queue (19 public slots) ---------------------------------------------------------------------------

struct FakeQueue {
    void** vtable;
    long refs = 1;
    unsigned calls[19] = {};
    std::uint64_t last_a1 = 0, last_a2 = 0;
    unsigned exec_count = 0;
    void* exec_lists[8] = {};
    D3D12_COMMAND_QUEUE_DESC desc{D3D12_COMMAND_LIST_TYPE_COMPUTE, 100, D3D12_COMMAND_QUEUE_FLAG_NONE, 1};
    unsigned total() const {
        unsigned n = 0;
        for (unsigned s = 3; s < 19; ++s) n += calls[s];
        return n;
    }
};

bool is_queue_iid(REFIID iid) {
    return IsEqualIID(iid, __uuidof(IUnknown)) || IsEqualIID(iid, __uuidof(ID3D12Object)) ||
           IsEqualIID(iid, __uuidof(ID3D12DeviceChild)) || IsEqualIID(iid, __uuidof(ID3D12Pageable)) ||
           IsEqualIID(iid, __uuidof(ID3D12CommandQueue));
}

HRESULT WINAPI fq_qi(FakeQueue* self, REFIID iid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!is_queue_iid(iid)) return E_NOINTERFACE;
    ++self->refs;
    *out = self;
    return S_OK;
}
ULONG WINAPI fq_add_ref(FakeQueue* self) { return static_cast<ULONG>(++self->refs); }
ULONG WINAPI fq_release(FakeQueue* self) { return static_cast<ULONG>(--self->refs); }  // storage owned by the test
template <unsigned Slot>
std::uint64_t WINAPI fq_other(FakeQueue* self, std::uint64_t a1, std::uint64_t a2) {
    ++self->calls[Slot];
    self->last_a1 = a1;
    self->last_a2 = a2;
    return static_cast<std::uint64_t>(static_cast<std::uint32_t>(E_NOTIMPL));
}
void WINAPI fq_execute(FakeQueue* self, UINT count, void* const* lists) {
    ++self->calls[10];
    self->exec_count = count;
    for (UINT i = 0; i < count && i < 8; ++i) self->exec_lists[i] = lists[i];
}
HRESULT WINAPI fq_signal(FakeQueue* self, std::uint64_t fence, std::uint64_t value) {
    ++self->calls[14];
    self->last_a1 = fence;
    self->last_a2 = value;
    return S_OK;
}
HRESULT WINAPI fq_wait(FakeQueue* self, std::uint64_t fence, std::uint64_t value) {
    ++self->calls[15];
    self->last_a1 = fence;
    self->last_a2 = value;
    return S_FALSE;  // distinct from Signal's answer, so the route is visible in the result
}
HRESULT WINAPI fq_frequency(FakeQueue* self, UINT64* out) {
    ++self->calls[16];
    if (!out) return E_POINTER;
    *out = 123456789;
    return S_OK;
}
HRESULT WINAPI fq_calibration(FakeQueue* self, UINT64* gpu, UINT64* cpu) {
    ++self->calls[17];
    if (!gpu || !cpu) return E_POINTER;
    *gpu = 111;
    *cpu = 222;
    return S_OK;
}
D3D12_COMMAND_QUEUE_DESC* WINAPI fq_get_desc(FakeQueue* self, D3D12_COMMAND_QUEUE_DESC* out) {  // the PC C++ form
    ++self->calls[18];
    *out = self->desc;
    return out;
}

void* g_queue_vtable[19] = {
    reinterpret_cast<void*>(&fq_qi),          reinterpret_cast<void*>(&fq_add_ref),
    reinterpret_cast<void*>(&fq_release),     reinterpret_cast<void*>(&fq_other<3>),
    reinterpret_cast<void*>(&fq_other<4>),    reinterpret_cast<void*>(&fq_other<5>),
    reinterpret_cast<void*>(&fq_other<6>),    reinterpret_cast<void*>(&fq_other<7>),
    reinterpret_cast<void*>(&fq_other<8>),    reinterpret_cast<void*>(&fq_other<9>),
    reinterpret_cast<void*>(&fq_execute),     reinterpret_cast<void*>(&fq_other<11>),
    reinterpret_cast<void*>(&fq_other<12>),   reinterpret_cast<void*>(&fq_other<13>),
    reinterpret_cast<void*>(&fq_signal),      reinterpret_cast<void*>(&fq_wait),
    reinterpret_cast<void*>(&fq_frequency),   reinterpret_cast<void*>(&fq_calibration),
    reinterpret_cast<void*>(&fq_get_desc),
};

// ---- Fake fence (ID3D12Fence only, no Fence1) --------------------------------------------------------------------

struct FakeFence {
    void** vtable;
    long refs = 1;
};
HRESULT WINAPI ff_qi(FakeFence* self, REFIID iid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!IsEqualIID(iid, __uuidof(IUnknown)) && !IsEqualIID(iid, __uuidof(ID3D12Object)) &&
        !IsEqualIID(iid, __uuidof(ID3D12DeviceChild)) && !IsEqualIID(iid, __uuidof(ID3D12Pageable)) &&
        !IsEqualIID(iid, __uuidof(ID3D12Fence)))
        return E_NOINTERFACE;
    ++self->refs;
    *out = self;
    return S_OK;
}
ULONG WINAPI ff_add_ref(FakeFence* self) { return static_cast<ULONG>(++self->refs); }
ULONG WINAPI ff_release(FakeFence* self) { return static_cast<ULONG>(--self->refs); }
std::uint64_t WINAPI ff_other(FakeFence*) { return static_cast<std::uint64_t>(static_cast<std::uint32_t>(E_NOTIMPL)); }
void* g_fence_vtable[11] = {
    reinterpret_cast<void*>(&ff_qi),    reinterpret_cast<void*>(&ff_add_ref), reinterpret_cast<void*>(&ff_release),
    reinterpret_cast<void*>(&ff_other), reinterpret_cast<void*>(&ff_other),  reinterpret_cast<void*>(&ff_other),
    reinterpret_cast<void*>(&ff_other), reinterpret_cast<void*>(&ff_other),  reinterpret_cast<void*>(&ff_other),
    reinterpret_cast<void*>(&ff_other), reinterpret_cast<void*>(&ff_other),
};

// ---- Fake PC device: QueryInterface, AddRef, Release, CreateCommandQueue (8), CreateFence (36) -------------------

constexpr unsigned kMaxQueues = 4;
struct FakeDevice {
    void** vtable;
    long refs = 1;
    FakeQueue queues[kMaxQueues];
    unsigned queue_count = 0;
    FakeFence fence{g_fence_vtable};
};

HRESULT WINAPI fd_qi(FakeDevice* self, REFIID iid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!IsEqualIID(iid, __uuidof(IUnknown)) && !IsEqualIID(iid, __uuidof(ID3D12Object)) &&
        !IsEqualIID(iid, __uuidof(ID3D12Device)))
        return E_NOINTERFACE;
    ++self->refs;
    *out = self;
    return S_OK;
}
ULONG WINAPI fd_add_ref(FakeDevice* self) { return static_cast<ULONG>(++self->refs); }
ULONG WINAPI fd_release(FakeDevice* self) { return static_cast<ULONG>(--self->refs); }
HRESULT WINAPI fd_create_queue(FakeDevice* self, const D3D12_COMMAND_QUEUE_DESC* desc, REFIID iid, void** out) {
    if (!out || !desc || !IsEqualIID(iid, __uuidof(ID3D12CommandQueue)) || self->queue_count == kMaxQueues)
        return E_INVALIDARG;
    FakeQueue& queue = self->queues[self->queue_count++];
    queue.vtable = g_queue_vtable;
    queue.desc = *desc;
    *out = &queue;
    return S_OK;
}
HRESULT WINAPI fd_create_fence(FakeDevice* self, UINT64, D3D12_FENCE_FLAGS, REFIID iid, void** out) {
    if (!out || !IsEqualIID(iid, __uuidof(ID3D12Fence))) return E_INVALIDARG;
    ++self->fence.refs;
    *out = &self->fence;
    return S_OK;
}
HRESULT WINAPI fd_unexpected(FakeDevice*) { return E_NOTIMPL; }

void* g_device_vtable[64];

void init_device_vtable() {
    for (void*& entry : g_device_vtable) entry = reinterpret_cast<void*>(&fd_unexpected);
    g_device_vtable[0] = reinterpret_cast<void*>(&fd_qi);
    g_device_vtable[1] = reinterpret_cast<void*>(&fd_add_ref);
    g_device_vtable[2] = reinterpret_cast<void*>(&fd_release);
    g_device_vtable[8] = reinterpret_cast<void*>(&fd_create_queue);
    g_device_vtable[36] = reinterpret_cast<void*>(&fd_create_fence);
}

// ---- Raw calls through a proxy's vtable ----------------------------------------------------------------------------

void** vtable_of(const void* object) { return *static_cast<void** const*>(object); }

std::uint64_t raw(void* object, unsigned slot, std::uint64_t a1 = 0, std::uint64_t a2 = 0, std::uint64_t a3 = 0,
                  std::uint64_t a4 = 0) {
    using Fn = std::uint64_t(WINAPI*)(void*, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);
    return reinterpret_cast<Fn>(vtable_of(object)[slot])(object, a1, a2, a3, a4);
}

HRESULT raw_hr(void* object, unsigned slot, std::uint64_t a1 = 0, std::uint64_t a2 = 0, std::uint64_t a3 = 0,
               std::uint64_t a4 = 0) {
    return static_cast<HRESULT>(static_cast<std::uint32_t>(raw(object, slot, a1, a2, a3, a4)));
}

std::uint64_t word(const void* p) { return reinterpret_cast<std::uint64_t>(p); }

struct Created {
    void* device = nullptr;  // the device proxy (ID3D12Device*)
    void* queue = nullptr;   // a queue proxy from device slot 8
    void* fence = nullptr;   // a fence proxy from device slot 36
};

Created create(FakeDevice& fake, std::uint32_t version, bool pass_version) {
    Created c;
    ++fake.refs;  // create_device_proxy takes one reference on success
    const HRESULT hr = pass_version ? xs::create_device_proxy(reinterpret_cast<ID3D12Device*>(&fake),
                                                              __uuidof(ID3D12Device), &c.device, version)
                                    : xs::create_device_proxy(reinterpret_cast<ID3D12Device*>(&fake),
                                                              __uuidof(ID3D12Device), &c.device);
    EXPECT(SUCCEEDED(hr) && c.device && xs::is_device_proxy(c.device));
    if (FAILED(hr)) {
        --fake.refs;
        return c;
    }
    D3D12_COMMAND_QUEUE_DESC desc{D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
    EXPECT(raw_hr(c.device, 8, word(&desc), word(&__uuidof(ID3D12CommandQueue)), word(&c.queue)) == S_OK);
    EXPECT(c.queue && xs::is_queue_proxy(c.queue));
    EXPECT(raw_hr(c.device, 36, 0, 0, word(&__uuidof(ID3D12Fence)), word(&c.fence)) == S_OK);
    EXPECT(c.fence && xs::is_fence_proxy(c.fence));
    EXPECT(xs::queue_proxy_xbox_version(c.queue) == (pass_version ? version : 0u));
    EXPECT(xs::queue_proxy_xbox_version(c.fence) == 0);
    return c;
}

void release(Created& c) {
    if (c.fence) EXPECT(raw(c.fence, 2) == 0);
    if (c.queue) EXPECT(raw(c.queue, 2) == 0);
    if (c.device) EXPECT(raw(c.device, 2) == 0);
    c = {};
}

// A plain object with a vtable that is none of the proxies: stands for a PC command list.
void* g_plain_vtable[4] = {};
struct PlainList {
    void** vtable = g_plain_vtable;
};

// ---- Cases ---------------------------------------------------------------------------------------------------------

void check_pc_layout(bool pass_zero) {
    FakeDevice fake{g_device_vtable};
    Created c = create(fake, 0, pass_zero);
    if (!c.queue) return;
    FakeQueue& real = fake.queues[0];
    void* real_fence = &fake.fence;

    // Native C++ calls in the PC shape.
    auto* queue = static_cast<ID3D12CommandQueue*>(c.queue);
    EXPECT(queue->Signal(static_cast<ID3D12Fence*>(c.fence), 5) == S_OK);
    EXPECT(real.calls[14] == 1 && real.last_a1 == word(real_fence) && real.last_a2 == 5);
    EXPECT(queue->Wait(static_cast<ID3D12Fence*>(c.fence), 6) == S_FALSE);
    EXPECT(real.calls[15] == 1 && real.last_a1 == word(real_fence) && real.last_a2 == 6);
    UINT64 frequency = 0;
    EXPECT(queue->GetTimestampFrequency(&frequency) == S_OK && frequency == 123456789);
    const D3D12_COMMAND_QUEUE_DESC got = queue->GetDesc();
    EXPECT(real.calls[18] == 1 && got.Type == D3D12_COMMAND_LIST_TYPE_DIRECT && got.Priority == 0);
    PlainList a, b;
    ID3D12CommandList* lists[2] = {reinterpret_cast<ID3D12CommandList*>(&a), reinterpret_cast<ID3D12CommandList*>(&b)};
    queue->ExecuteCommandLists(2, lists);
    EXPECT(real.calls[10] == 1 && real.exec_count == 2 && real.exec_lists[0] == &a && real.exec_lists[1] == &b);

    // Console-only slots are past the PC chain: refused, nothing reaches the backend.
    const unsigned before = real.total();
    EXPECT(raw_hr(c.queue, 20, word(&frequency)) == E_NOTIMPL);
    EXPECT(raw_hr(c.queue, 32, 1, 0, 0) == E_NOTIMPL);
    EXPECT(raw_hr(c.queue, 35, 2, word(lists), 0, 0) == E_NOTIMPL);
    EXPECT(raw_hr(c.queue, 30, 1, 0, 0) == E_NOTIMPL);
    EXPECT(real.total() == before);
    release(c);
    EXPECT(real.refs == 0 && fake.fence.refs == 1 && fake.refs == 1);
}

void check_modern_layout() {
    FakeDevice fake{g_device_vtable};
    Created c = create(fake, 0x00020004u, true);
    if (!c.queue) return;
    FakeQueue& real = fake.queues[0];
    real.desc = {D3D12_COMMAND_LIST_TYPE_COMPUTE, 100, D3D12_COMMAND_QUEUE_FLAG_NONE, 1};
    void* real_fence = &fake.fence;

    // COM identity and GetDevice are unchanged by the layout.
    void* same = nullptr;
    EXPECT(raw_hr(c.queue, 0, word(&__uuidof(ID3D12CommandQueue)), word(&same)) == S_OK && same == c.queue);
    EXPECT(xs::command_object_proxy_ref_count(c.queue) == 2);
    EXPECT(raw(c.queue, 2) == 1);
    void* device = nullptr;
    EXPECT(raw_hr(c.queue, 7, word(&__uuidof(ID3D12Device)), word(&device)) == S_OK && device == c.device);
    EXPECT(raw(c.device, 2) >= 1);

    // Signal 16 / Wait 17 unwrap the fence proxy; a real fence pointer passes unchanged.
    EXPECT(raw_hr(c.queue, 16, word(c.fence), 7) == S_OK);
    EXPECT(real.calls[14] == 1 && real.last_a1 == word(real_fence) && real.last_a2 == 7);
    EXPECT(raw_hr(c.queue, 17, word(c.fence), 8) == S_FALSE);
    EXPECT(real.calls[15] == 1 && real.last_a1 == word(real_fence) && real.last_a2 == 8);
    EXPECT(raw_hr(c.queue, 16, word(real_fence), 9) == S_OK && real.last_a1 == word(real_fence));

    // GetTimestampFrequency 18 and GetClockCalibration 19.
    UINT64 frequency = 0, gpu = 0, cpu = 0;
    EXPECT(raw_hr(c.queue, 18, word(&frequency)) == S_OK && frequency == 123456789 && real.calls[16] == 1);
    EXPECT(raw_hr(c.queue, 19, word(&gpu), word(&cpu)) == S_OK && gpu == 111 && cpu == 222 && real.calls[17] == 1);

    // GetDesc 20, PC form (queue in RCX, buffer in RDX) and console form (buffer in RCX, queue in RDX), with canaries.
    struct Guarded {
        std::uint64_t before;
        D3D12_COMMAND_QUEUE_DESC desc;
        std::uint64_t after;
    };
    using GetDescFn = D3D12_COMMAND_QUEUE_DESC*(WINAPI*)(void*, void*);
    const auto get_desc = reinterpret_cast<GetDescFn>(vtable_of(c.queue)[20]);
    Guarded pc_form{0xA5A5A5A5A5A5A5A5ull, {}, 0x5A5A5A5A5A5A5A5Aull};
    EXPECT(get_desc(c.queue, &pc_form.desc) == &pc_form.desc);
    EXPECT(pc_form.desc.Type == D3D12_COMMAND_LIST_TYPE_COMPUTE && pc_form.desc.Priority == 100 &&
           pc_form.desc.NodeMask == 1);
    EXPECT(pc_form.before == 0xA5A5A5A5A5A5A5A5ull && pc_form.after == 0x5A5A5A5A5A5A5A5Aull);
    Guarded console_form{0xA5A5A5A5A5A5A5A5ull, {}, 0x5A5A5A5A5A5A5A5Aull};
    std::memset(&console_form.desc, 0xCC, sizeof(console_form.desc));
    EXPECT(get_desc(&console_form.desc, c.queue) == &console_form.desc);
    EXPECT(console_form.desc.Type == D3D12_COMMAND_LIST_TYPE_COMPUTE && console_form.desc.Priority == 100 &&
           console_form.desc.NodeMask == 1);
    EXPECT(console_form.before == 0xA5A5A5A5A5A5A5A5ull && console_form.after == 0x5A5A5A5A5A5A5A5Aull);
    EXPECT(real.calls[18] == 2);
    // Neither register a queue proxy: nothing written, null returned, no backend call.
    D3D12_COMMAND_QUEUE_DESC untouched, other;
    std::memset(&untouched, 0xCC, sizeof(untouched));
    std::memset(&other, 0xCC, sizeof(other));
    EXPECT(get_desc(&untouched, &other) == nullptr && real.calls[18] == 2);
    const auto* bytes = reinterpret_cast<const unsigned char*>(&untouched);
    for (std::size_t i = 0; i < sizeof(untouched); ++i) EXPECT(bytes[i] == 0xCC);
    // A null buffer in the PC form: nothing to fill.
    EXPECT(get_desc(c.queue, nullptr) == nullptr && real.calls[18] == 2);

    // ExecuteCommandListsX 35: count and lists forwarded unchanged through PC ExecuteCommandLists; the caller's array
    // is not written. The upper halves of the 32-bit count and flags are not part of their values.
    PlainList a, b;
    void* lists[2] = {&a, &b};
    EXPECT(raw_hr(c.queue, 35, 0xDEAD000000000002ull, word(lists), 0, 0xBEEF000000000000ull) == S_OK);
    EXPECT(real.calls[10] == 1 && real.exec_count == 2 && real.exec_lists[0] == &a && real.exec_lists[1] == &b);
    EXPECT(lists[0] == &a && lists[1] == &b);

    // Its refusals: nothing reaches the backend.
    unsigned before = real.total();
    EXPECT(raw_hr(c.queue, 35, 0, word(lists), 0, 0) == E_INVALIDARG);
    EXPECT(raw_hr(c.queue, 35, 2, 0, 0, 0) == E_INVALIDARG);
    void* with_null[2] = {&a, nullptr};
    EXPECT(raw_hr(c.queue, 35, 2, word(with_null), 0, 0) == E_INVALIDARG);
    void* with_fence[2] = {&a, c.fence};
    EXPECT(raw_hr(c.queue, 35, 2, word(with_fence), 0, 0) == E_INVALIDARG);
    void* with_queue[1] = {c.queue};
    EXPECT(raw_hr(c.queue, 35, 1, word(with_queue), 0, 0) == E_INVALIDARG);
    void* with_device[1] = {c.device};
    EXPECT(raw_hr(c.queue, 35, 1, word(with_device), 0, 0) == E_INVALIDARG);
    EXPECT(raw_hr(c.queue, 35, 2, word(lists), 0x1234, 0) == E_NOTIMPL);  // frame word: meaning not established
    EXPECT(raw_hr(c.queue, 35, 2, word(lists), 0, 3) == E_NOTIMPL);       // flags: meaning not established
    EXPECT(raw_hr(c.queue, 35, 2, word(lists), 0x1234, 3) == E_NOTIMPL);
    EXPECT(real.total() == before && real.calls[10] == 1);
    EXPECT(lists[0] == &a && lists[1] == &b && with_fence[1] == c.fence);

    // PresentX 32 is recognised and refused (layout not established); the other recognised methods too; slots 8..15,
    // where the public sources disagree, and unrouted slots are refused. None reaches the backend.
    before = real.total();
    EXPECT(raw_hr(c.queue, 32, 1, word(lists), 0) == E_NOTIMPL);
    EXPECT(raw_hr(c.queue, 32, 0, 0, 0) == E_NOTIMPL);
    for (unsigned slot : {8u, 9u, 10u, 11u, 12u, 13u, 14u, 15u, 21u, 22u, 23u, 24u, 25u, 26u, 30u, 31u, 33u, 34u,
                          36u, 40u, 127u})
        EXPECT(raw_hr(c.queue, slot, word(c.fence), 1, 0, 0) == E_NOTIMPL);
    EXPECT(real.total() == before);
    EXPECT(xs::queue_slot_hits(35) >= 10 && xs::queue_slot_hits(20) >= 4);

    release(c);
    EXPECT(real.refs == 0 && fake.fence.refs == 1 && fake.refs == 1);
}

void check_legacy_and_unknown() {
    {
        FakeDevice fake{g_device_vtable};
        Created c = create(fake, 0x00020003u, true);
        if (c.queue) {
            FakeQueue& real = fake.queues[0];
            EXPECT(raw_hr(c.queue, 14, word(c.fence), 3) == S_OK);
            EXPECT(real.calls[14] == 1 && real.last_a1 == word(&fake.fence) && real.last_a2 == 3);
            UINT64 frequency = 0;
            EXPECT(raw_hr(c.queue, 16, word(&frequency)) == S_OK && frequency == 123456789);
            const unsigned before = real.total();
            EXPECT(raw_hr(c.queue, 30, 1, 0, 0) == E_NOTIMPL);  // PresentX (legacy): layout not established
            EXPECT(raw_hr(c.queue, 32, 1, 0, 0) == E_NOTIMPL);  // past the PC chain
            EXPECT(raw_hr(c.queue, 35, 1, 0, 0, 0) == E_NOTIMPL);
            EXPECT(real.total() == before);
            release(c);
            EXPECT(real.refs == 0 && fake.refs == 1);
        }
    }
    {
        FakeDevice fake{g_device_vtable};
        Created c = create(fake, 0x00020005u, true);
        if (c.queue) {
            FakeQueue& real = fake.queues[0];
            void* same = nullptr;
            EXPECT(raw_hr(c.queue, 0, word(&__uuidof(IUnknown)), word(&same)) == S_OK && same == c.queue);
            EXPECT(raw(c.queue, 2) == 1);
            UINT64 frequency = 0;
            for (unsigned slot : {10u, 14u, 15u, 16u, 18u, 20u, 32u, 35u})
                EXPECT(raw_hr(c.queue, slot, word(&frequency), 0, 0, 0) == E_NOTIMPL);
            EXPECT(real.total() == 0 && frequency == 0);
            release(c);
            EXPECT(real.refs == 0 && fake.refs == 1);
        }
    }
}

}  // namespace

int main() {
    init_device_vtable();
    (void)xs::queue_slot_hits(0);  // the first read turns the per-slot hit counters on
    check_pc_layout(false);  // the default argument
    check_pc_layout(true);   // an explicit 0
    check_modern_layout();
    check_legacy_and_unknown();
    if (g_failures) {
        std::printf("xs_queue_version_test: FAILED (%d)\n", g_failures);
        return 1;
    }
    std::printf("xs_queue_version_test: ok\n");
    return 0;
}
