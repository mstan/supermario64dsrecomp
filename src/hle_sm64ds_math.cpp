#include "runtime_arm.h"

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

constexpr uint32_t kStart = 0x02052858u;
constexpr uint32_t kEnd = 0x02052914u;
constexpr uint32_t kInstructionCount = 47u;
constexpr char kRoutineId[] = "sm64ds.mul_vec3_mat4x3";
constexpr char kBank[] = "sm64ds_arm9";
constexpr char kContentSha1[] = "4d9db01dcbcbd3b05ce22dfd9ab1eb06ecaa6616";

struct Prediction {
    ArmCpuState cpu{};
    uint32_t intermediate_output[3]{};
    uint32_t output[3]{};
    uint32_t stack[6]{};
    uint32_t output_address = 0;
    uint32_t scratch_address = 0;
    uint32_t cycles = 0;
};

bool reject_once(uint32_t bit, const char* reason) {
    static uint32_t reported = 0u;
    if ((reported & bit) == 0u && std::getenv("NDS_HLE_HANDLER_DIAG")) {
        reported |= bit;
        std::fprintf(stderr,
                     "[hle-sm64ds] reject=%s pc=%08X cpsr=%08X "
                     "r0=%08X r1=%08X r2=%08X sp=%08X lr=%08X depth=%u\n",
                     reason, g_cpu.R[15], g_cpu.cpsr, g_cpu.R[0], g_cpu.R[1],
                     g_cpu.R[2], g_cpu.R[13], g_cpu.R[14],
                     runtime_call_stack_depth());
    }
    return false;
}

bool in_main_ram(uint32_t address, uint32_t size) {
    return address >= 0x02000000u &&
           static_cast<uint64_t>(address) + size <= 0x02400000ull;
}

bool overlaps(uint32_t a, uint32_t a_size, uint32_t b, uint32_t b_size) {
    const uint64_t a_end = static_cast<uint64_t>(a) + a_size;
    const uint64_t b_end = static_cast<uint64_t>(b) + b_size;
    return static_cast<uint64_t>(a) < b_end &&
           static_cast<uint64_t>(b) < a_end;
}

bool descriptor_matches(const NdsHleProfileDescriptor* descriptor) {
    return descriptor && descriptor->id && descriptor->bank &&
           descriptor->content_sha1 && descriptor->address == kStart &&
           descriptor->end_address == kEnd && descriptor->thumb == 0u &&
           descriptor->instruction_count == kInstructionCount &&
           std::strcmp(descriptor->id, kRoutineId) == 0 &&
           std::strcmp(descriptor->bank, kBank) == 0 &&
           std::strcmp(descriptor->content_sha1, kContentSha1) == 0;
}

bool coarse_math_enabled() {
    const char* value = std::getenv("NDS_HLE_MATH_COARSE");
    return value && value[0] == '1' && value[1] == '\0';
}

bool supported_call(const NdsHleProfileDescriptor* descriptor) {
    if (!descriptor_matches(descriptor))
        return reject_once(1u << 0u, "descriptor");
    if (g_nds_active != NDS_ARM9 || g_cpu.R[15] != kStart ||
        (g_cpu.cpsr & CPSR_T_BIT) != 0u)
        return reject_once(1u << 1u, "cpu-context");
    const uint32_t input = g_cpu.R[0];
    const uint32_t matrix = g_cpu.R[1];
    const uint32_t output = g_cpu.R[2];
    const uint32_t sp = g_cpu.R[13];
    if ((input | matrix | output | sp) & 3u)
        return reject_once(1u << 3u, "alignment");
    if (!in_main_ram(input, 12u) || !in_main_ram(matrix, 48u) ||
        !in_main_ram(output, 12u) || sp < 0x02000018u ||
        !in_main_ram(sp - 24u, 24u)) {
        return reject_once(1u << 4u, "address-range");
    }

    const uint32_t scratch = sp - 24u;
    if (overlaps(output, 12u, matrix, 48u) ||
        overlaps(scratch, 24u, input, 12u) ||
        overlaps(scratch, 24u, matrix, 48u) ||
        overlaps(scratch, 24u, output, 12u) ||
        overlaps(scratch, 24u, kStart, kEnd - kStart) ||
        overlaps(output, 12u, kStart, kEnd - kStart)) {
        return reject_once(1u << 5u, "aliasing");
    }

    const uint32_t depth = runtime_call_stack_depth();
    const uint32_t* stack = runtime_call_stack_data();
    const uint32_t return_key =
        (g_cpu.R[14] & ~1u) | ((g_cpu.R[14] & 1u) ? 1u : 0u);
    if (depth == 0u || !stack || stack[depth - 1u] != return_key)
        return reject_once(1u << 6u, "call-stack");
    return true;
}

uint32_t fixed_dot(uint32_t a0, uint32_t b0, uint32_t a1, uint32_t b1,
                   uint32_t a2, uint32_t b2) {
    uint64_t sum = static_cast<uint64_t>(
        static_cast<int64_t>(static_cast<int32_t>(a0)) *
        static_cast<int64_t>(static_cast<int32_t>(b0)));
    sum += static_cast<uint64_t>(
        static_cast<int64_t>(static_cast<int32_t>(a1)) *
        static_cast<int64_t>(static_cast<int32_t>(b1)));
    sum += static_cast<uint64_t>(
        static_cast<int64_t>(static_cast<int32_t>(a2)) *
        static_cast<int64_t>(static_cast<int32_t>(b2)));
    return static_cast<uint32_t>(sum >> 12u);
}

void charge_plain(Prediction& prediction, uint32_t pc) {
    prediction.cycles +=
        arm9_cycle_combine(runtime_code_cycles(pc), 0u, 0u, 0u);
}

void charge_multiply(Prediction& prediction, uint32_t pc) {
    prediction.cycles +=
        arm9_cycle_combine(runtime_code_cycles(pc), 0u, 1u, 0u);
}

void charge_memory(Prediction& prediction, uint32_t pc, uint32_t address) {
    const uint32_t data = runtime_mem_cycles(address, 4u, 0u);
    prediction.cycles +=
        arm9_cycle_combine(runtime_code_cycles(pc), data, 0u, 1u);
}

void charge_block(Prediction& prediction, uint32_t pc, uint32_t address) {
    uint32_t data = runtime_mem_cycles(address, 4u, 0u);
    for (uint32_t offset = 4u; offset < 24u; offset += 4u)
        data += runtime_mem_cycles(address + offset, 4u, 1u);
    prediction.cycles +=
        arm9_cycle_combine(runtime_code_cycles(pc), data, 0u, 1u);
}

bool predict(const NdsHleProfileDescriptor* descriptor, Prediction& prediction) {
    if (!supported_call(descriptor)) return false;

    prediction.cpu = g_cpu;
    const uint32_t input = g_cpu.R[0];
    const uint32_t matrix = g_cpu.R[1];
    const uint32_t output = g_cpu.R[2];
    const uint32_t scratch = g_cpu.R[13] - 24u;
    prediction.output_address = output;
    prediction.scratch_address = scratch;
    prediction.stack[0] = g_cpu.R[4];
    prediction.stack[1] = g_cpu.R[5];
    prediction.stack[2] = g_cpu.R[6];
    prediction.stack[3] = g_cpu.R[7];
    prediction.stack[4] = g_cpu.R[8];
    prediction.stack[5] = g_cpu.R[14];

    charge_block(prediction, 0x02052858u, scratch);

    charge_memory(prediction, 0x0205285Cu, input + 4u);
    const uint32_t y = bus_read_u32(input + 4u);
    charge_memory(prediction, 0x02052860u, matrix + 12u);
    const uint32_t m3 = bus_read_u32(matrix + 12u);
    charge_memory(prediction, 0x02052864u, input);
    const uint32_t x = bus_read_u32(input);
    charge_multiply(prediction, 0x02052868u);
    charge_memory(prediction, 0x0205286Cu, matrix);
    const uint32_t m0 = bus_read_u32(matrix);
    charge_memory(prediction, 0x02052870u, input + 8u);
    const uint32_t z = bus_read_u32(input + 8u);
    charge_multiply(prediction, 0x02052874u);
    charge_memory(prediction, 0x02052878u, matrix + 24u);
    const uint32_t m6 = bus_read_u32(matrix + 24u);
    charge_plain(prediction, 0x0205287Cu);
    charge_multiply(prediction, 0x02052880u);
    charge_plain(prediction, 0x02052884u);
    charge_plain(prediction, 0x02052888u);
    const uint32_t pre_x = fixed_dot(y, m3, x, m0, z, m6);
    prediction.intermediate_output[0] = pre_x;
    charge_memory(prediction, 0x0205288Cu, output);
    charge_memory(prediction, 0x02052890u, output);
    const uint32_t reload_x = pre_x;
    charge_memory(prediction, 0x02052894u, matrix + 36u);
    const uint32_t m9 = bus_read_u32(matrix + 36u);
    charge_plain(prediction, 0x02052898u);
    charge_plain(prediction, 0x0205289Cu);
    prediction.output[0] = reload_x + m9;
    charge_memory(prediction, 0x020528A0u, output);

    charge_memory(prediction, 0x020528A4u, matrix + 16u);
    const uint32_t m4 = bus_read_u32(matrix + 16u);
    charge_memory(prediction, 0x020528A8u, matrix + 4u);
    const uint32_t m1 = bus_read_u32(matrix + 4u);
    charge_multiply(prediction, 0x020528ACu);
    charge_multiply(prediction, 0x020528B0u);
    charge_memory(prediction, 0x020528B4u, matrix + 28u);
    const uint32_t m7 = bus_read_u32(matrix + 28u);
    charge_multiply(prediction, 0x020528B8u);
    charge_plain(prediction, 0x020528BCu);
    charge_plain(prediction, 0x020528C0u);
    const uint32_t pre_y = fixed_dot(y, m4, x, m1, z, m7);
    prediction.intermediate_output[1] = pre_y;
    charge_memory(prediction, 0x020528C4u, output + 4u);
    charge_memory(prediction, 0x020528C8u, output + 4u);
    const uint32_t reload_y = pre_y;
    charge_memory(prediction, 0x020528CCu, matrix + 40u);
    const uint32_t m10 = bus_read_u32(matrix + 40u);
    charge_plain(prediction, 0x020528D0u);
    prediction.output[1] = reload_y + m10;
    charge_memory(prediction, 0x020528D4u, output + 4u);

    charge_memory(prediction, 0x020528D8u, matrix + 20u);
    const uint32_t m5 = bus_read_u32(matrix + 20u);
    charge_memory(prediction, 0x020528DCu, matrix + 8u);
    const uint32_t m2 = bus_read_u32(matrix + 8u);
    charge_multiply(prediction, 0x020528E0u);
    charge_multiply(prediction, 0x020528E4u);
    charge_memory(prediction, 0x020528E8u, matrix + 32u);
    const uint32_t m8 = bus_read_u32(matrix + 32u);
    charge_multiply(prediction, 0x020528ECu);
    charge_plain(prediction, 0x020528F0u);
    charge_plain(prediction, 0x020528F4u);
    const uint32_t pre_z = fixed_dot(y, m5, x, m2, z, m8);
    prediction.intermediate_output[2] = pre_z;
    charge_memory(prediction, 0x020528F8u, output + 8u);
    charge_memory(prediction, 0x020528FCu, output + 8u);
    const uint32_t reload_z = pre_z;
    charge_memory(prediction, 0x02052900u, matrix + 44u);
    const uint32_t m11 = bus_read_u32(matrix + 44u);
    charge_plain(prediction, 0x02052904u);
    prediction.output[2] = reload_z + m11;
    charge_memory(prediction, 0x02052908u, output + 8u);

    charge_block(prediction, 0x0205290Cu, scratch);
    prediction.cycles += arm9_refill_cycles(g_cpu.R[14]);

    prediction.cpu.R[0] = prediction.output[2];
    prediction.cpu.R[2] = pre_z;
    prediction.cpu.R[3] = m8;
    prediction.cpu.R[4] = prediction.stack[0];
    prediction.cpu.R[5] = prediction.stack[1];
    prediction.cpu.R[6] = prediction.stack[2];
    prediction.cpu.R[7] = prediction.stack[3];
    prediction.cpu.R[8] = prediction.stack[4];
    prediction.cpu.R[12] = y;
    prediction.cpu.R[14] = prediction.stack[5];
    prediction.cpu.R[15] = prediction.cpu.R[14] & ~1u;
    if (prediction.cpu.R[14] & 1u)
        prediction.cpu.cpsr |= CPSR_T_BIT;
    else
        prediction.cpu.cpsr &= ~CPSR_T_BIT;
    return true;
}

NdsHleRunResult run(const NdsHleProfileDescriptor* descriptor) {
    Prediction prediction;
    if (!predict(descriptor, prediction)) return NDS_HLE_RUN_UNSUPPORTED;
    if (!coarse_math_enabled() &&
        !runtime_hle_atomic_allowed(prediction.cycles)) {
        reject_once(1u << 2u, "atomic-policy");
        return NDS_HLE_RUN_UNSUPPORTED;
    }

    for (uint32_t i = 0; i < 6u; ++i)
        bus_write_u32(prediction.scratch_address + i * 4u,
                      prediction.stack[i]);
    for (uint32_t i = 0; i < 3u; ++i) {
        bus_write_u32(prediction.output_address + i * 4u,
                      prediction.intermediate_output[i]);
        bus_write_u32(prediction.output_address + i * 4u,
                      prediction.output[i]);
    }

    g_cpu = prediction.cpu;
    g_insn_count[NDS_ARM9] += kInstructionCount;
    runtime_tick(prediction.cycles);
    if (!runtime_call_should_return(g_cpu.R[15]))
        runtime_dispatch_with_exchange(g_cpu.R[14]);
    return NDS_HLE_RUN_HANDLED;
}

NdsHleVerifyResult verify(const NdsHleProfileDescriptor* descriptor,
                         NdsHleLleFn lle) {
    Prediction prediction;
    const uint64_t instructions_before = g_insn_count[NDS_ARM9];
    const unsigned long long cycles_before = g_runtime_cycles;
    const uint32_t depth_before = runtime_call_stack_depth();
    if (!predict(descriptor, prediction) ||
        !runtime_hle_atomic_allowed(prediction.cycles)) {
        lle();
        return NDS_HLE_VERIFY_UNSUPPORTED;
    }

    lle();
    bool match = std::memcmp(&g_cpu, &prediction.cpu, sizeof g_cpu) == 0 &&
                 g_insn_count[NDS_ARM9] ==
                     instructions_before + kInstructionCount &&
                 g_runtime_cycles == cycles_before + prediction.cycles &&
                 runtime_call_stack_depth() + 1u == depth_before;
    for (uint32_t i = 0; i < 3u; ++i) {
        match = match &&
            bus_read_u32(prediction.output_address + i * 4u) ==
                prediction.output[i];
    }
    for (uint32_t i = 0; i < 6u; ++i) {
        match = match &&
            bus_read_u32(prediction.scratch_address + i * 4u) ==
                prediction.stack[i];
    }
    return match ? NDS_HLE_VERIFY_MATCH : NDS_HLE_VERIFY_MISMATCH;
}

}  // namespace

extern "C" const NdsHleHandler sm64ds_mul_vec3_mat4x3 = {
    "sm64ds_mul_vec3_mat4x3", run, verify, 0u, nullptr};

namespace mat4x3_mul {

constexpr uint32_t kStart = 0x02052914u;
constexpr uint32_t kEnd = 0x02052B10u;
constexpr uint32_t kInstructionCount = 127u;
constexpr char kRoutineId[] = "sm64ds.mul_mat4x3_mat4x3";
constexpr char kBank[] = "sm64ds_arm9";
constexpr char kContentSha1[] = "8383366806beba875bc99f41f1f81cd42eb1f81c";

struct Stats {
    uint64_t calls = 0;
    uint64_t descriptor_rejects = 0;
    uint64_t context_rejects = 0;
    uint64_t alignment_rejects = 0;
    uint64_t range_rejects = 0;
    uint64_t alias_rejects = 0;
    uint64_t call_stack_rejects = 0;
    uint64_t atomic_rejects = 0;
    uint64_t alias_calls = 0;
    uint64_t predictions = 0;
    uint64_t coarse_hits = 0;
};

Stats g_stats;

bool descriptor_matches(const NdsHleProfileDescriptor* descriptor) {
    return descriptor && descriptor->id && descriptor->bank &&
           descriptor->content_sha1 && descriptor->address == kStart &&
           descriptor->end_address == kEnd && descriptor->thumb == 0u &&
           descriptor->instruction_count == kInstructionCount &&
           std::strcmp(descriptor->id, kRoutineId) == 0 &&
           std::strcmp(descriptor->bank, kBank) == 0 &&
           std::strcmp(descriptor->content_sha1, kContentSha1) == 0;
}

bool coarse_math_enabled() {
    const char* value = std::getenv("NDS_HLE_MATH_COARSE");
    return value && value[0] == '1' && value[1] == '\0';
}

bool main_ram_range(uint32_t address, uint32_t size) {
    return address >= 0x02000000u &&
           static_cast<uint64_t>(address) + size <= 0x02400000ull;
}

bool ranges_overlap(uint32_t a, uint32_t a_size, uint32_t b,
                    uint32_t b_size) {
    const uint64_t a_end = static_cast<uint64_t>(a) + a_size;
    const uint64_t b_end = static_cast<uint64_t>(b) + b_size;
    return static_cast<uint64_t>(a) < b_end &&
           static_cast<uint64_t>(b) < a_end;
}

uint32_t dot_shift(uint32_t a0, uint32_t b0, uint32_t a1, uint32_t b1,
                   uint32_t a2, uint32_t b2) {
    uint64_t sum = static_cast<uint64_t>(
        static_cast<int64_t>(static_cast<int32_t>(a0)) *
        static_cast<int64_t>(static_cast<int32_t>(b0)));
    sum += static_cast<uint64_t>(
        static_cast<int64_t>(static_cast<int32_t>(a1)) *
        static_cast<int64_t>(static_cast<int32_t>(b1)));
    sum += static_cast<uint64_t>(
        static_cast<int64_t>(static_cast<int32_t>(a2)) *
        static_cast<int64_t>(static_cast<int32_t>(b2)));
    return static_cast<uint32_t>(sum >> 12u);
}

uint32_t dot_high32(uint32_t a0, uint32_t b0, uint32_t a1, uint32_t b1,
                    uint32_t a2, uint32_t b2) {
    uint64_t sum = static_cast<uint64_t>(
        static_cast<int64_t>(static_cast<int32_t>(a0)) *
        static_cast<int64_t>(static_cast<int32_t>(b0)));
    sum += static_cast<uint64_t>(
        static_cast<int64_t>(static_cast<int32_t>(a1)) *
        static_cast<int64_t>(static_cast<int32_t>(b1)));
    sum += static_cast<uint64_t>(
        static_cast<int64_t>(static_cast<int32_t>(a2)) *
        static_cast<int64_t>(static_cast<int32_t>(b2)));
    return static_cast<uint32_t>(sum >> 32u);
}

uint32_t rough_cycles() {
    return 360u;
}

struct Prediction {
    ArmCpuState cpu{};
    uint32_t stack_address = 0;
    uint32_t saved_stack[8]{};
    uint32_t output_address = 0;
    uint32_t output[12]{};
    uint32_t cycles = 0;
};

bool supported_call(const NdsHleProfileDescriptor* descriptor) {
    ++g_stats.calls;
    if (!descriptor_matches(descriptor)) {
        ++g_stats.descriptor_rejects;
        return false;
    }
    if (g_nds_active != NDS_ARM9 || g_cpu.R[15] != kStart ||
        (g_cpu.cpsr & CPSR_T_BIT) != 0u) {
        ++g_stats.context_rejects;
        return false;
    }

    const uint32_t a = g_cpu.R[0];
    const uint32_t b = g_cpu.R[1];
    const uint32_t out = g_cpu.R[2];
    const uint32_t sp = g_cpu.R[13];
    if ((a | b | out | sp) & 3u) {
        ++g_stats.alignment_rejects;
        return false;
    }
    if (!main_ram_range(a, 48u) || !main_ram_range(b, 48u) ||
        !main_ram_range(out, 48u) || sp < 0x02000020u ||
        !main_ram_range(sp - 32u, 32u)) {
        ++g_stats.range_rejects;
        return false;
    }
    if (ranges_overlap(sp - 32u, 32u, a, 48u) ||
        ranges_overlap(sp - 32u, 32u, b, 48u) ||
        ranges_overlap(sp - 32u, 32u, out, 48u)) {
        ++g_stats.alias_rejects;
        return false;
    }
    if (ranges_overlap(out, 48u, a, 48u) ||
        ranges_overlap(out, 48u, b, 48u))
        ++g_stats.alias_calls;

    const uint32_t depth = runtime_call_stack_depth();
    const uint32_t* stack = runtime_call_stack_data();
    const uint32_t return_key =
        (g_cpu.R[14] & ~1u) | ((g_cpu.R[14] & 1u) ? 1u : 0u);
    if (depth == 0u || !stack || stack[depth - 1u] != return_key) {
        ++g_stats.call_stack_rejects;
        return false;
    }
    return true;
}

bool predict(const NdsHleProfileDescriptor* descriptor, Prediction& p) {
    if (!supported_call(descriptor)) return false;

    p.cpu = g_cpu;
    p.stack_address = g_cpu.R[13] - 32u;
    p.output_address = g_cpu.R[2];
    p.cycles = rough_cycles();
    const uint32_t a_addr = g_cpu.R[0];
    const uint32_t b_addr = g_cpu.R[1];
    uint32_t a[12]{};
    uint32_t b[12]{};
    for (uint32_t i = 0; i < 12u; ++i) {
        a[i] = bus_read_u32(a_addr + i * 4u);
        b[i] = bus_read_u32(b_addr + i * 4u);
    }
    p.saved_stack[0] = g_cpu.R[4];
    p.saved_stack[1] = g_cpu.R[5];
    p.saved_stack[2] = g_cpu.R[6];
    p.saved_stack[3] = g_cpu.R[7];
    p.saved_stack[4] = g_cpu.R[8];
    p.saved_stack[5] = g_cpu.R[9];
    p.saved_stack[6] = g_cpu.R[10];
    p.saved_stack[7] = g_cpu.R[14];

    for (uint32_t row = 0; row < 3u; ++row) {
        for (uint32_t col = 0; col < 3u; ++col) {
            p.output[row * 3u + col] = dot_shift(
                a[row * 3u], b[col],
                a[row * 3u + 1u], b[3u + col],
                a[row * 3u + 2u], b[6u + col]);
        }
    }
    for (uint32_t col = 0; col < 3u; ++col) {
        p.output[9u + col] = dot_shift(
            a[9u], b[col], a[10u], b[3u + col],
            a[11u], b[6u + col]) + b[9u + col];
    }

    if (p.output_address == b_addr) {
        p.cpu.R[0] = p.output[8];
        p.cpu.R[1] = p.output[9];
        p.cpu.R[2] = p.output[10];
        p.cpu.R[3] = p.output[11];
        p.cpu.R[12] = p.output_address + 32u;
    } else {
        p.cpu.R[0] = p.output[9];
        p.cpu.R[1] = dot_high32(a[10u], b[3u], a[9u], b[0u],
                                a[11u], b[6u]);
        p.cpu.R[2] = p.output_address;
        p.cpu.R[3] = a[9u];
        p.cpu.R[12] = p.output_address;
    }
    for (uint32_t i = 0; i < 7u; ++i)
        p.cpu.R[4u + i] = p.saved_stack[i];
    p.cpu.R[13] = g_cpu.R[13];
    p.cpu.R[14] = p.saved_stack[7];
    p.cpu.R[15] = p.cpu.R[14] & ~1u;
    if (p.cpu.R[14] & 1u)
        p.cpu.cpsr |= CPSR_T_BIT;
    else
        p.cpu.cpsr &= ~CPSR_T_BIT;
    ++g_stats.predictions;
    return true;
}

NdsHleRunResult run(const NdsHleProfileDescriptor* descriptor) {
    Prediction p;
    if (!predict(descriptor, p)) return NDS_HLE_RUN_UNSUPPORTED;
    if (!runtime_hle_atomic_allowed(p.cycles)) {
        ++g_stats.atomic_rejects;
        if (!coarse_math_enabled())
            return NDS_HLE_RUN_UNSUPPORTED;
    }

    for (uint32_t i = 0; i < 8u; ++i)
        bus_write_u32(p.stack_address + i * 4u, p.saved_stack[i]);
    for (uint32_t i = 0; i < 12u; ++i)
        bus_write_u32(p.output_address + i * 4u, p.output[i]);

    g_cpu = p.cpu;
    g_insn_count[NDS_ARM9] += kInstructionCount;
    runtime_tick(p.cycles);
    ++g_stats.coarse_hits;
    if (!runtime_call_should_return(g_cpu.R[15]))
        runtime_dispatch_with_exchange(g_cpu.R[14]);
    return NDS_HLE_RUN_HANDLED;
}

NdsHleVerifyResult verify(const NdsHleProfileDescriptor*,
                          NdsHleLleFn lle) {
    lle();
    return NDS_HLE_VERIFY_UNSUPPORTED;
}

const char* diagnostics_json() {
    static char buffer[512];
    std::snprintf(buffer, sizeof buffer,
                  "{\"calls\":%llu,\"descriptor_rejects\":%llu,"
                  "\"context_rejects\":%llu,\"alignment_rejects\":%llu,"
                  "\"range_rejects\":%llu,\"alias_rejects\":%llu,"
                  "\"call_stack_rejects\":%llu,\"atomic_rejects\":%llu,"
                  "\"alias_calls\":%llu,\"predictions\":%llu,"
                  "\"coarse_hits\":%llu}",
                  static_cast<unsigned long long>(g_stats.calls),
                  static_cast<unsigned long long>(g_stats.descriptor_rejects),
                  static_cast<unsigned long long>(g_stats.context_rejects),
                  static_cast<unsigned long long>(g_stats.alignment_rejects),
                  static_cast<unsigned long long>(g_stats.range_rejects),
                  static_cast<unsigned long long>(g_stats.alias_rejects),
                  static_cast<unsigned long long>(g_stats.call_stack_rejects),
                  static_cast<unsigned long long>(g_stats.atomic_rejects),
                  static_cast<unsigned long long>(g_stats.alias_calls),
                  static_cast<unsigned long long>(g_stats.predictions),
                  static_cast<unsigned long long>(g_stats.coarse_hits));
    return buffer;
}

}  // namespace mat4x3_mul

extern "C" const NdsHleHandler sm64ds_mul_mat4x3_mat4x3 = {
    "sm64ds_mul_mat4x3_mat4x3",
    mat4x3_mul::run,
    mat4x3_mul::verify,
    0u,
    mat4x3_mul::diagnostics_json};

namespace particle_delta_probe {

constexpr uint32_t kStart = 0x020499BCu;
constexpr uint32_t kEnd = 0x02049BA8u;
constexpr uint32_t kInstructionCount = 123u;
constexpr char kRoutineId[] = "sm64ds.particle_delta";
constexpr char kBank[] = "sm64ds_arm9";
constexpr char kContentSha1[] = "ec662159423cac116acc54f84169911301b522a2";

struct Stats {
    uint64_t calls = 0;
    uint64_t descriptor_rejects = 0;
    uint64_t context_rejects = 0;
    uint64_t alignment_rejects = 0;
    uint64_t range_rejects = 0;
    uint64_t predictions = 0;
    uint64_t verify_matches = 0;
    uint64_t verify_mismatches = 0;
    uint64_t verify_unwound = 0;
    uint64_t small_atomic_possible = 0;
    uint64_t atomic_blocked = 0;
    uint64_t a2_total = 0;
    uint32_t a2_min = 0xffffffffu;
    uint32_t a2_max = 0;
    uint64_t a2_bins[10]{};
    uint32_t first_mismatch_addr = 0;
    uint32_t first_mismatch_expected = 0;
    uint32_t first_mismatch_actual = 0;
};

Stats g_stats;

uint32_t rough_cycles(uint32_t bytes) {
    const uint32_t half = bytes >> 1u;
    const uint32_t inner = half > 2u ? half - 2u : 0u;
    const uint32_t flush_lines =
        ((bytes + (g_cpu.R[0] & 31u) + 31u) >> 5u) +
        ((bytes + (g_cpu.R[1] & 31u) + 31u) >> 5u);
    return 40u + inner * 18u + flush_lines * 5u;
}

unsigned a2_bin(uint32_t bytes) {
    if (bytes == 0u) return 0u;
    if (bytes <= 4u) return 1u;
    if (bytes <= 8u) return 2u;
    if (bytes <= 16u) return 3u;
    if (bytes <= 32u) return 4u;
    if (bytes <= 64u) return 5u;
    if (bytes <= 128u) return 6u;
    if (bytes <= 256u) return 7u;
    if (bytes <= 512u) return 8u;
    return 9u;
}

bool descriptor_matches(const NdsHleProfileDescriptor* descriptor) {
    return descriptor && descriptor->id && descriptor->bank &&
           descriptor->content_sha1 && descriptor->address == kStart &&
           descriptor->end_address == kEnd && descriptor->thumb == 0u &&
           descriptor->instruction_count == kInstructionCount &&
           std::strcmp(descriptor->id, kRoutineId) == 0 &&
           std::strcmp(descriptor->bank, kBank) == 0 &&
           std::strcmp(descriptor->content_sha1, kContentSha1) == 0;
}

bool in_main_ram(uint32_t address, uint32_t size) {
    return address >= 0x02000000u &&
           static_cast<uint64_t>(address) + size <= 0x02400000ull;
}

bool ranges_overlap(uint32_t a, uint32_t a_size, uint32_t b,
                    uint32_t b_size) {
    const uint64_t a_end = static_cast<uint64_t>(a) + a_size;
    const uint64_t b_end = static_cast<uint64_t>(b) + b_size;
    return static_cast<uint64_t>(a) < b_end && static_cast<uint64_t>(b) < a_end;
}

enum class ObserveResult : uint8_t {
    Reject,
    Supported,
};

ObserveResult observe(const NdsHleProfileDescriptor* descriptor) {
    ++g_stats.calls;
    if (!descriptor_matches(descriptor)) {
        ++g_stats.descriptor_rejects;
        return ObserveResult::Reject;
    }
    if (g_nds_active != NDS_ARM9 || g_cpu.R[15] != kStart ||
        (g_cpu.cpsr & CPSR_T_BIT) != 0u) {
        ++g_stats.context_rejects;
        return ObserveResult::Reject;
    }

    const uint32_t a0 = g_cpu.R[0];
    const uint32_t a1 = g_cpu.R[1];
    const uint32_t bytes = g_cpu.R[2];
    const uint32_t ctx = g_cpu.R[3];
    if ((a0 | a1 | bytes | ctx | g_cpu.R[13]) & 1u) {
        ++g_stats.alignment_rejects;
        return ObserveResult::Reject;
    }

    ++g_stats.a2_bins[a2_bin(bytes)];
    g_stats.a2_total += bytes;
    if (bytes < g_stats.a2_min) g_stats.a2_min = bytes;
    if (bytes > g_stats.a2_max) g_stats.a2_max = bytes;

    if ((bytes & 1u) != 0u || bytes < 4u ||
        !in_main_ram(a0, bytes) || !in_main_ram(a1, bytes) ||
        !in_main_ram(ctx + 12u, 8u) ||
        g_cpu.R[13] < 0x02000020u ||
        !in_main_ram(g_cpu.R[13] - 32u, 32u)) {
        ++g_stats.range_rejects;
        return ObserveResult::Reject;
    }
    if (ranges_overlap(a0, bytes, a1, bytes) ||
        ranges_overlap(a0, bytes, ctx + 0x0cu, 8u) ||
        ranges_overlap(a1, bytes, ctx + 0x0cu, 8u)) {
        ++g_stats.range_rejects;
        return ObserveResult::Reject;
    }

    if (runtime_hle_atomic_allowed(rough_cycles(bytes)))
        ++g_stats.small_atomic_possible;
    else
        ++g_stats.atomic_blocked;
    return ObserveResult::Supported;
}

uint16_t clamp_s16(int32_t value) {
    if (value < -32768) return 0x8000u;
    if (value > 32767) return 0x7fffu;
    return static_cast<uint16_t>(static_cast<int16_t>(value));
}

struct Prediction {
    std::vector<uint16_t> a;
    std::vector<uint16_t> b;
    uint32_t ctx0 = 0;
    uint32_t ctx1 = 0;
};

bool predict(Prediction& prediction) {
    const uint32_t a_addr = g_cpu.R[0];
    const uint32_t b_addr = g_cpu.R[1];
    const uint32_t bytes = g_cpu.R[2];
    const uint32_t ctx = g_cpu.R[3];
    const uint32_t count = bytes >> 1u;
    if (count < 2u) return false;

    prediction.a.resize(count);
    prediction.b.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        prediction.a[i] = bus_read_u16(a_addr + i * 2u);
        prediction.b[i] = bus_read_u16(b_addr + i * 2u);
    }

    int32_t carry[2]{};
    for (uint32_t i = 0; i < 2u; ++i) {
        const uint32_t src = count - 2u + i;
        const int32_t delta = static_cast<int16_t>(prediction.a[src]) -
                              static_cast<int16_t>(prediction.b[src]);
        carry[i] = static_cast<int16_t>(clamp_s16(delta));
    }

    if (count >= 4u) {
        for (uint32_t index = count - 1u; index >= 2u; --index) {
            const int32_t delta =
                static_cast<int16_t>(prediction.a[index - 2u]) -
                static_cast<int16_t>(prediction.b[index - 2u]);
            prediction.a[index] = clamp_s16(
                static_cast<int16_t>(prediction.a[index]) + delta);
            prediction.b[index] = clamp_s16(
                static_cast<int16_t>(prediction.b[index]) - delta);
        }
    }

    for (int32_t index = 1; index >= 0; --index) {
        const uint32_t i = static_cast<uint32_t>(index);
        const int32_t history =
            static_cast<int32_t>(bus_read_u32(ctx + 0x0cu + i * 4u));
        prediction.a[i] = clamp_s16(
            static_cast<int16_t>(prediction.a[i]) + history);
        prediction.b[i] = clamp_s16(
            static_cast<int16_t>(prediction.b[i]) - history);
    }
    prediction.ctx0 = static_cast<uint32_t>(carry[0]);
    prediction.ctx1 = static_cast<uint32_t>(carry[1]);
    ++g_stats.predictions;
    return true;
}

bool compare_prediction(const Prediction& prediction) {
    const uint32_t a_addr = g_cpu.R[0];
    const uint32_t b_addr = g_cpu.R[1];
    const uint32_t ctx = g_cpu.R[3];
    for (uint32_t i = 0; i < prediction.a.size(); ++i) {
        const uint32_t addr = a_addr + i * 2u;
        const uint32_t actual = bus_read_u16(addr);
        if (actual != prediction.a[i]) {
            g_stats.first_mismatch_addr = addr;
            g_stats.first_mismatch_expected = prediction.a[i];
            g_stats.first_mismatch_actual = actual;
            return false;
        }
    }
    for (uint32_t i = 0; i < prediction.b.size(); ++i) {
        const uint32_t addr = b_addr + i * 2u;
        const uint32_t actual = bus_read_u16(addr);
        if (actual != prediction.b[i]) {
            g_stats.first_mismatch_addr = addr;
            g_stats.first_mismatch_expected = prediction.b[i];
            g_stats.first_mismatch_actual = actual;
            return false;
        }
    }
    const uint32_t actual0 = bus_read_u32(ctx + 0x0cu);
    if (actual0 != prediction.ctx0) {
        g_stats.first_mismatch_addr = ctx + 0x0cu;
        g_stats.first_mismatch_expected = prediction.ctx0;
        g_stats.first_mismatch_actual = actual0;
        return false;
    }
    const uint32_t actual1 = bus_read_u32(ctx + 0x10u);
    if (actual1 != prediction.ctx1) {
        g_stats.first_mismatch_addr = ctx + 0x10u;
        g_stats.first_mismatch_expected = prediction.ctx1;
        g_stats.first_mismatch_actual = actual1;
        return false;
    }
    return true;
}

uint64_t dynamic_instructions(uint32_t bytes) {
    const uint32_t count = bytes >> 1u;
    const uint32_t tail_loop = count >= 4u ? count - 2u : 0u;
    return 28ull + static_cast<uint64_t>(tail_loop) * 28ull + 34ull;
}

void commit_prediction(const Prediction& prediction) {
    const uint32_t a_addr = g_cpu.R[0];
    const uint32_t b_addr = g_cpu.R[1];
    const uint32_t bytes = g_cpu.R[2];
    const uint32_t ctx = g_cpu.R[3];
    const uint32_t lr = g_cpu.R[14];

    for (uint32_t i = 0; i < prediction.a.size(); ++i)
        bus_write_u16(a_addr + i * 2u, prediction.a[i]);
    for (uint32_t i = 0; i < prediction.b.size(); ++i)
        bus_write_u16(b_addr + i * 2u, prediction.b[i]);
    bus_write_u32(ctx + 0x0cu, prediction.ctx0);
    bus_write_u32(ctx + 0x10u, prediction.ctx1);

    g_insn_count[g_nds_active] += dynamic_instructions(bytes);
    g_runtime_cycles += rough_cycles(bytes);
    g_cpu.R[15] = lr & ~1u;
    if (lr & 1u)
        g_cpu.cpsr |= CPSR_T_BIT;
    else
        g_cpu.cpsr &= ~CPSR_T_BIT;
    (void)runtime_call_should_return(g_cpu.R[15]);
}

bool commit_in_place() {
    const uint32_t a_addr = g_cpu.R[0];
    const uint32_t b_addr = g_cpu.R[1];
    const uint32_t bytes = g_cpu.R[2];
    const uint32_t ctx = g_cpu.R[3];
    const uint32_t lr = g_cpu.R[14];
    const uint32_t count = bytes >> 1u;
    if (count < 2u) return false;

    int32_t carry[2]{};
    for (uint32_t i = 0; i < 2u; ++i) {
        const uint32_t src = count - 2u + i;
        const int32_t delta =
            static_cast<int16_t>(bus_read_u16(a_addr + src * 2u)) -
            static_cast<int16_t>(bus_read_u16(b_addr + src * 2u));
        carry[i] = static_cast<int16_t>(clamp_s16(delta));
    }

    if (count >= 4u) {
        for (uint32_t index = count - 1u; index >= 2u; --index) {
            const int32_t delta =
                static_cast<int16_t>(
                    bus_read_u16(a_addr + (index - 2u) * 2u)) -
                static_cast<int16_t>(
                    bus_read_u16(b_addr + (index - 2u) * 2u));
            const uint32_t a_out = a_addr + index * 2u;
            const uint32_t b_out = b_addr + index * 2u;
            bus_write_u16(a_out, clamp_s16(
                static_cast<int16_t>(bus_read_u16(a_out)) + delta));
            bus_write_u16(b_out, clamp_s16(
                static_cast<int16_t>(bus_read_u16(b_out)) - delta));
        }
    }

    for (int32_t index = 1; index >= 0; --index) {
        const uint32_t i = static_cast<uint32_t>(index);
        const int32_t history =
            static_cast<int32_t>(bus_read_u32(ctx + 0x0cu + i * 4u));
        const uint32_t a_out = a_addr + i * 2u;
        const uint32_t b_out = b_addr + i * 2u;
        bus_write_u16(a_out, clamp_s16(
            static_cast<int16_t>(bus_read_u16(a_out)) + history));
        bus_write_u16(b_out, clamp_s16(
            static_cast<int16_t>(bus_read_u16(b_out)) - history));
    }

    bus_write_u32(ctx + 0x0cu, static_cast<uint32_t>(carry[0]));
    bus_write_u32(ctx + 0x10u, static_cast<uint32_t>(carry[1]));

    ++g_stats.predictions;
    g_insn_count[g_nds_active] += dynamic_instructions(bytes);
    g_runtime_cycles += rough_cycles(bytes);
    g_cpu.R[15] = lr & ~1u;
    if (lr & 1u)
        g_cpu.cpsr |= CPSR_T_BIT;
    else
        g_cpu.cpsr &= ~CPSR_T_BIT;
    (void)runtime_call_should_return(g_cpu.R[15]);
    return true;
}

NdsHleRunResult run(const NdsHleProfileDescriptor* descriptor) {
    if (observe(descriptor) != ObserveResult::Supported)
        return NDS_HLE_RUN_UNSUPPORTED;
    if (!commit_in_place())
        return NDS_HLE_RUN_UNSUPPORTED;
    return NDS_HLE_RUN_HANDLED;
}

NdsHleVerifyResult verify(const NdsHleProfileDescriptor* descriptor,
                          NdsHleLleFn lle) {
    if (observe(descriptor) != ObserveResult::Supported) {
        lle();
        return NDS_HLE_VERIFY_UNSUPPORTED;
    }
    Prediction prediction;
    if (!predict(prediction)) {
        lle();
        return NDS_HLE_VERIFY_UNSUPPORTED;
    }
    lle();
    if (runtime_unwinding()) {
        ++g_stats.verify_unwound;
        return NDS_HLE_VERIFY_UNSUPPORTED;
    }
    if (compare_prediction(prediction)) {
        ++g_stats.verify_matches;
        return NDS_HLE_VERIFY_MATCH;
    }
    ++g_stats.verify_mismatches;
    return NDS_HLE_VERIFY_MISMATCH;
}

const char* diagnostics_json() {
    static char out[1024];
    const uint64_t observed =
        g_stats.a2_bins[0] + g_stats.a2_bins[1] + g_stats.a2_bins[2] +
        g_stats.a2_bins[3] + g_stats.a2_bins[4] + g_stats.a2_bins[5] +
        g_stats.a2_bins[6] + g_stats.a2_bins[7] + g_stats.a2_bins[8] +
        g_stats.a2_bins[9];
    const uint32_t min_value = observed ? g_stats.a2_min : 0u;
    std::snprintf(out, sizeof out,
        "{\"calls\":%llu,\"descriptor_rejects\":%llu,"
        "\"context_rejects\":%llu,\"alignment_rejects\":%llu,"
        "\"range_rejects\":%llu,\"predictions\":%llu,"
        "\"verify_matches\":%llu,\"verify_mismatches\":%llu,"
        "\"verify_unwound\":%llu,\"atomic_possible\":%llu,"
        "\"atomic_blocked\":%llu,\"a2_min\":%u,\"a2_max\":%u,"
        "\"a2_avg\":%llu,\"first_mismatch_addr\":%u,"
        "\"first_mismatch_expected\":%u,\"first_mismatch_actual\":%u,"
        "\"a2_bins\":[%llu,%llu,%llu,%llu,%llu,%llu,"
        "%llu,%llu,%llu,%llu]}",
        static_cast<unsigned long long>(g_stats.calls),
        static_cast<unsigned long long>(g_stats.descriptor_rejects),
        static_cast<unsigned long long>(g_stats.context_rejects),
        static_cast<unsigned long long>(g_stats.alignment_rejects),
        static_cast<unsigned long long>(g_stats.range_rejects),
        static_cast<unsigned long long>(g_stats.predictions),
        static_cast<unsigned long long>(g_stats.verify_matches),
        static_cast<unsigned long long>(g_stats.verify_mismatches),
        static_cast<unsigned long long>(g_stats.verify_unwound),
        static_cast<unsigned long long>(g_stats.small_atomic_possible),
        static_cast<unsigned long long>(g_stats.atomic_blocked),
        min_value, g_stats.a2_max,
        static_cast<unsigned long long>(
            observed ? g_stats.a2_total / observed : 0u),
        g_stats.first_mismatch_addr,
        g_stats.first_mismatch_expected,
        g_stats.first_mismatch_actual,
        static_cast<unsigned long long>(g_stats.a2_bins[0]),
        static_cast<unsigned long long>(g_stats.a2_bins[1]),
        static_cast<unsigned long long>(g_stats.a2_bins[2]),
        static_cast<unsigned long long>(g_stats.a2_bins[3]),
        static_cast<unsigned long long>(g_stats.a2_bins[4]),
        static_cast<unsigned long long>(g_stats.a2_bins[5]),
        static_cast<unsigned long long>(g_stats.a2_bins[6]),
        static_cast<unsigned long long>(g_stats.a2_bins[7]),
        static_cast<unsigned long long>(g_stats.a2_bins[8]),
        static_cast<unsigned long long>(g_stats.a2_bins[9]));
    return out;
}

}  // namespace particle_delta_probe

extern "C" const NdsHleHandler sm64ds_particle_delta_probe = {
    "sm64ds_particle_delta_probe",
    particle_delta_probe::run,
    particle_delta_probe::verify,
    0u,
    particle_delta_probe::diagnostics_json};

namespace particle_block_delta_probe {

using particle_delta_probe::ObserveResult;
using particle_delta_probe::a2_bin;
using particle_delta_probe::clamp_s16;
using particle_delta_probe::in_main_ram;
using particle_delta_probe::ranges_overlap;

constexpr uint32_t kStart = 0x020497A0u;
constexpr uint32_t kEnd = 0x020499BCu;
constexpr uint32_t kInstructionCount = 135u;
constexpr char kRoutineId[] = "sm64ds.particle_block_delta";
constexpr char kBank[] = "sm64ds_arm9";
constexpr char kContentSha1[] = "e90080ae7b1ea8d5277be585f4a03674e90db0e8";
constexpr uint32_t kHistoryCount = 24u;
constexpr uint32_t kHistoryBase = 0x0cu;
constexpr uint32_t kHistoryBytes = kHistoryBase + kHistoryCount * 8u + 4u;

struct Stats {
    uint64_t calls = 0;
    uint64_t descriptor_rejects = 0;
    uint64_t context_rejects = 0;
    uint64_t alignment_rejects = 0;
    uint64_t range_rejects = 0;
    uint64_t predictions = 0;
    uint64_t hits = 0;
    uint64_t verify_matches = 0;
    uint64_t verify_mismatches = 0;
    uint64_t verify_unwound = 0;
    uint64_t a2_total = 0;
    uint32_t a2_min = 0xffffffffu;
    uint32_t a2_max = 0;
    uint64_t a2_bins[10]{};
    uint32_t first_mismatch_addr = 0;
    uint32_t first_mismatch_expected = 0;
    uint32_t first_mismatch_actual = 0;
};

Stats g_stats;

bool descriptor_matches(const NdsHleProfileDescriptor* descriptor) {
    return descriptor && descriptor->id && descriptor->bank &&
           descriptor->content_sha1 && descriptor->address == kStart &&
           descriptor->end_address == kEnd && descriptor->thumb == 0u &&
           descriptor->instruction_count == kInstructionCount &&
           std::strcmp(descriptor->id, kRoutineId) == 0 &&
           std::strcmp(descriptor->bank, kBank) == 0 &&
           std::strcmp(descriptor->content_sha1, kContentSha1) == 0;
}

int32_t asr_s32(int32_t value, uint32_t shift) {
    if (shift == 0u) return value;
    uint32_t bits = static_cast<uint32_t>(value) >> shift;
    if (value < 0)
        bits |= (~0u << (32u - shift));
    return static_cast<int32_t>(bits);
}

int32_t read_s16_from(const std::vector<uint16_t>& values, uint32_t index) {
    return static_cast<int32_t>(static_cast<int16_t>(values[index]));
}

struct History {
    int32_t x[kHistoryCount]{};
    int32_t y[kHistoryCount]{};
};

struct Prediction {
    std::vector<uint16_t> a;
    std::vector<uint16_t> b;
    History history;
};

uint32_t rough_cycles(uint32_t bytes) {
    const uint32_t count = bytes >> 1u;
    const uint32_t full_blocks = count > kHistoryCount
        ? (count - 1u) / kHistoryCount
        : 0u;
    const uint32_t remainder = count - full_blocks * kHistoryCount;
    const uint32_t writes = count * 2u + kHistoryCount * 2u;
    const uint32_t flush_lines =
        ((bytes + (g_cpu.R[0] & 31u) + 31u) >> 5u) +
        ((bytes + (g_cpu.R[1] & 31u) + 31u) >> 5u);
    return 80u + full_blocks * 520u + remainder * 24u + writes * 2u +
           flush_lines * 5u;
}

uint64_t dynamic_instructions(uint32_t bytes) {
    const uint32_t count = bytes >> 1u;
    const uint32_t full_blocks = count > kHistoryCount
        ? (count - 1u) / kHistoryCount
        : 0u;
    const uint32_t remainder = count - full_blocks * kHistoryCount;
    return 40ull + static_cast<uint64_t>(full_blocks) * 30ull * kHistoryCount +
           static_cast<uint64_t>(remainder) * 21ull +
           static_cast<uint64_t>(kHistoryCount - remainder) * 7ull +
           static_cast<uint64_t>(remainder) * 11ull + 12ull;
}

ObserveResult observe(const NdsHleProfileDescriptor* descriptor) {
    ++g_stats.calls;
    if (!descriptor_matches(descriptor)) {
        ++g_stats.descriptor_rejects;
        return ObserveResult::Reject;
    }
    if (g_nds_active != NDS_ARM9 || g_cpu.R[15] != kStart ||
        (g_cpu.cpsr & CPSR_T_BIT) != 0u) {
        ++g_stats.context_rejects;
        return ObserveResult::Reject;
    }

    const uint32_t a0 = g_cpu.R[0];
    const uint32_t a1 = g_cpu.R[1];
    const uint32_t bytes = g_cpu.R[2];
    const uint32_t ctx = g_cpu.R[3];
    if ((a0 | a1 | bytes | ctx | g_cpu.R[13]) & 1u) {
        ++g_stats.alignment_rejects;
        return ObserveResult::Reject;
    }

    ++g_stats.a2_bins[a2_bin(bytes)];
    g_stats.a2_total += bytes;
    if (bytes < g_stats.a2_min) g_stats.a2_min = bytes;
    if (bytes > g_stats.a2_max) g_stats.a2_max = bytes;

    if ((bytes & 1u) != 0u || bytes < 2u ||
        !in_main_ram(a0, bytes) || !in_main_ram(a1, bytes) ||
        !in_main_ram(ctx + kHistoryBase, kHistoryBytes - kHistoryBase) ||
        g_cpu.R[13] < 0x02000030u ||
        !in_main_ram(g_cpu.R[13] - 40u, 44u)) {
        ++g_stats.range_rejects;
        return ObserveResult::Reject;
    }
    if (ranges_overlap(a0, bytes, a1, bytes) ||
        ranges_overlap(a0, bytes, ctx + kHistoryBase,
                       kHistoryBytes - kHistoryBase) ||
        ranges_overlap(a1, bytes, ctx + kHistoryBase,
                       kHistoryBytes - kHistoryBase)) {
        ++g_stats.range_rejects;
        return ObserveResult::Reject;
    }
    return ObserveResult::Supported;
}

bool predict(Prediction& prediction) {
    const uint32_t a_addr = g_cpu.R[0];
    const uint32_t b_addr = g_cpu.R[1];
    const uint32_t bytes = g_cpu.R[2];
    const uint32_t ctx = g_cpu.R[3];
    const uint32_t count = bytes >> 1u;
    if (count == 0u) return false;

    prediction.a.resize(count);
    prediction.b.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        prediction.a[i] = bus_read_u16(a_addr + i * 2u);
        prediction.b[i] = bus_read_u16(b_addr + i * 2u);
    }
    for (uint32_t i = 0; i < kHistoryCount; ++i) {
        prediction.history.x[i] =
            static_cast<int32_t>(bus_read_u32(ctx + kHistoryBase + i * 8u));
        prediction.history.y[i] =
            static_cast<int32_t>(bus_read_u32(ctx + kHistoryBase + i * 8u + 4u));
    }

    uint32_t base = 0;
    while (count > base + kHistoryCount) {
        for (uint32_t i = 0; i < kHistoryCount; ++i) {
            const uint32_t index = base + i;
            const int32_t raw_a =
                read_s16_from(prediction.a, index) + prediction.history.x[i];
            const int32_t raw_b =
                read_s16_from(prediction.b, index) + prediction.history.y[i];
            prediction.a[index] = clamp_s16(raw_a);
            prediction.b[index] = clamp_s16(raw_b);
            prediction.history.x[i] = asr_s32(raw_b + 1, 2u);
            prediction.history.y[i] = asr_s32(raw_a + 1, 2u);
        }
        base += kHistoryCount;
    }

    const uint32_t remainder = count - base;
    for (uint32_t i = 0; i < remainder; ++i) {
        const uint32_t index = base + i;
        prediction.a[index] = clamp_s16(
            read_s16_from(prediction.a, index) + prediction.history.x[i]);
        prediction.b[index] = clamp_s16(
            read_s16_from(prediction.b, index) + prediction.history.y[i]);
    }

    const uint32_t shift_count = kHistoryCount - remainder;
    for (uint32_t i = 0; i < shift_count; ++i) {
        prediction.history.x[i] = prediction.history.x[i + remainder];
        prediction.history.y[i] = prediction.history.y[i + remainder];
    }
    for (uint32_t i = 0; i < remainder; ++i) {
        const uint32_t index = base + i;
        const uint32_t dst = i + shift_count;
        prediction.history.x[dst] =
            asr_s32(read_s16_from(prediction.b, index) + 1, 2u);
        prediction.history.y[dst] =
            asr_s32(read_s16_from(prediction.a, index) + 1, 2u);
    }

    ++g_stats.predictions;
    return true;
}

bool compare_prediction(const Prediction& prediction) {
    const uint32_t a_addr = g_cpu.R[0];
    const uint32_t b_addr = g_cpu.R[1];
    const uint32_t ctx = g_cpu.R[3];
    for (uint32_t i = 0; i < prediction.a.size(); ++i) {
        const uint32_t addr = a_addr + i * 2u;
        const uint32_t actual = bus_read_u16(addr);
        if (actual != prediction.a[i]) {
            g_stats.first_mismatch_addr = addr;
            g_stats.first_mismatch_expected = prediction.a[i];
            g_stats.first_mismatch_actual = actual;
            return false;
        }
    }
    for (uint32_t i = 0; i < prediction.b.size(); ++i) {
        const uint32_t addr = b_addr + i * 2u;
        const uint32_t actual = bus_read_u16(addr);
        if (actual != prediction.b[i]) {
            g_stats.first_mismatch_addr = addr;
            g_stats.first_mismatch_expected = prediction.b[i];
            g_stats.first_mismatch_actual = actual;
            return false;
        }
    }
    for (uint32_t i = 0; i < kHistoryCount; ++i) {
        const uint32_t x_addr = ctx + kHistoryBase + i * 8u;
        const uint32_t actual_x = bus_read_u32(x_addr);
        if (actual_x != static_cast<uint32_t>(prediction.history.x[i])) {
            g_stats.first_mismatch_addr = x_addr;
            g_stats.first_mismatch_expected =
                static_cast<uint32_t>(prediction.history.x[i]);
            g_stats.first_mismatch_actual = actual_x;
            return false;
        }
        const uint32_t y_addr = x_addr + 4u;
        const uint32_t actual_y = bus_read_u32(y_addr);
        if (actual_y != static_cast<uint32_t>(prediction.history.y[i])) {
            g_stats.first_mismatch_addr = y_addr;
            g_stats.first_mismatch_expected =
                static_cast<uint32_t>(prediction.history.y[i]);
            g_stats.first_mismatch_actual = actual_y;
            return false;
        }
    }
    return true;
}

void commit_prediction(const Prediction& prediction) {
    const uint32_t a_addr = g_cpu.R[0];
    const uint32_t b_addr = g_cpu.R[1];
    const uint32_t bytes = g_cpu.R[2];
    const uint32_t ctx = g_cpu.R[3];
    const uint32_t lr = g_cpu.R[14];

    for (uint32_t i = 0; i < prediction.a.size(); ++i)
        bus_write_u16(a_addr + i * 2u, prediction.a[i]);
    for (uint32_t i = 0; i < prediction.b.size(); ++i)
        bus_write_u16(b_addr + i * 2u, prediction.b[i]);
    for (uint32_t i = 0; i < kHistoryCount; ++i) {
        bus_write_u32(ctx + kHistoryBase + i * 8u,
                      static_cast<uint32_t>(prediction.history.x[i]));
        bus_write_u32(ctx + kHistoryBase + i * 8u + 4u,
                      static_cast<uint32_t>(prediction.history.y[i]));
    }

    g_insn_count[g_nds_active] += dynamic_instructions(bytes);
    g_runtime_cycles += rough_cycles(bytes);
    g_cpu.R[15] = lr & ~1u;
    if (lr & 1u)
        g_cpu.cpsr |= CPSR_T_BIT;
    else
        g_cpu.cpsr &= ~CPSR_T_BIT;
    (void)runtime_call_should_return(g_cpu.R[15]);
}

NdsHleRunResult run(const NdsHleProfileDescriptor* descriptor) {
    if (observe(descriptor) != ObserveResult::Supported)
        return NDS_HLE_RUN_UNSUPPORTED;
    Prediction prediction;
    if (!predict(prediction))
        return NDS_HLE_RUN_UNSUPPORTED;
    commit_prediction(prediction);
    ++g_stats.hits;
    return NDS_HLE_RUN_HANDLED;
}

NdsHleVerifyResult verify(const NdsHleProfileDescriptor* descriptor,
                          NdsHleLleFn lle) {
    if (observe(descriptor) != ObserveResult::Supported) {
        lle();
        return NDS_HLE_VERIFY_UNSUPPORTED;
    }
    Prediction prediction;
    if (!predict(prediction)) {
        lle();
        return NDS_HLE_VERIFY_UNSUPPORTED;
    }
    lle();
    if (runtime_unwinding()) {
        ++g_stats.verify_unwound;
        return NDS_HLE_VERIFY_UNSUPPORTED;
    }
    if (compare_prediction(prediction)) {
        ++g_stats.verify_matches;
        return NDS_HLE_VERIFY_MATCH;
    }
    ++g_stats.verify_mismatches;
    return NDS_HLE_VERIFY_MISMATCH;
}

const char* diagnostics_json() {
    static char out[1024];
    const uint64_t observed =
        g_stats.a2_bins[0] + g_stats.a2_bins[1] + g_stats.a2_bins[2] +
        g_stats.a2_bins[3] + g_stats.a2_bins[4] + g_stats.a2_bins[5] +
        g_stats.a2_bins[6] + g_stats.a2_bins[7] + g_stats.a2_bins[8] +
        g_stats.a2_bins[9];
    const uint32_t min_value = observed ? g_stats.a2_min : 0u;
    std::snprintf(out, sizeof out,
        "{\"calls\":%llu,\"descriptor_rejects\":%llu,"
        "\"context_rejects\":%llu,\"alignment_rejects\":%llu,"
        "\"range_rejects\":%llu,\"predictions\":%llu,\"hits\":%llu,"
        "\"verify_matches\":%llu,\"verify_mismatches\":%llu,"
        "\"verify_unwound\":%llu,\"a2_min\":%u,\"a2_max\":%u,"
        "\"a2_avg\":%llu,\"first_mismatch_addr\":%u,"
        "\"first_mismatch_expected\":%u,\"first_mismatch_actual\":%u,"
        "\"a2_bins\":[%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu]}",
        static_cast<unsigned long long>(g_stats.calls),
        static_cast<unsigned long long>(g_stats.descriptor_rejects),
        static_cast<unsigned long long>(g_stats.context_rejects),
        static_cast<unsigned long long>(g_stats.alignment_rejects),
        static_cast<unsigned long long>(g_stats.range_rejects),
        static_cast<unsigned long long>(g_stats.predictions),
        static_cast<unsigned long long>(g_stats.hits),
        static_cast<unsigned long long>(g_stats.verify_matches),
        static_cast<unsigned long long>(g_stats.verify_mismatches),
        static_cast<unsigned long long>(g_stats.verify_unwound),
        min_value, g_stats.a2_max,
        static_cast<unsigned long long>(
            observed ? g_stats.a2_total / observed : 0u),
        g_stats.first_mismatch_addr,
        g_stats.first_mismatch_expected,
        g_stats.first_mismatch_actual,
        static_cast<unsigned long long>(g_stats.a2_bins[0]),
        static_cast<unsigned long long>(g_stats.a2_bins[1]),
        static_cast<unsigned long long>(g_stats.a2_bins[2]),
        static_cast<unsigned long long>(g_stats.a2_bins[3]),
        static_cast<unsigned long long>(g_stats.a2_bins[4]),
        static_cast<unsigned long long>(g_stats.a2_bins[5]),
        static_cast<unsigned long long>(g_stats.a2_bins[6]),
        static_cast<unsigned long long>(g_stats.a2_bins[7]),
        static_cast<unsigned long long>(g_stats.a2_bins[8]),
        static_cast<unsigned long long>(g_stats.a2_bins[9]));
    return out;
}

}  // namespace particle_block_delta_probe

extern "C" const NdsHleHandler sm64ds_particle_block_delta_probe = {
    "sm64ds_particle_block_delta_probe",
    particle_block_delta_probe::run,
    particle_block_delta_probe::verify,
    0u,
    particle_block_delta_probe::diagnostics_json};
