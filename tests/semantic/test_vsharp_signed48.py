"""Execute emitted SPIR-V for Palmwave's signed48 normalized V# contract.

The synthetic heap is 16 GiB and the imported allocation is at 512 MiB, so a zero-extended
V# would read a different address by exactly 2^48. Ordinary 64-bit pointers must keep that
unsigned value; only the V# decoding sites sign-extend. No game files or GPU required.
"""
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)
import support
import test_scalar_mem_smem as smem
import test_value_semantics as values
from shader_ir import parse_line

HEAP = 0x400000000
IMPORTED = 0x20000000
MASK64 = (1 << 64) - 1
FIELD = (1 << 48) - (HEAP - IMPORTED)


class SignedVsharpAddressTests(unittest.TestCase):
    def scalar(self, line, base, expected_address):
        gen = support.rdna2spirv.Gen("ps")
        ins = parse_line(line)
        gen.scalar_load(ins["op"], ins["args"], ins["mods"])
        registers = {24: base & 0xFFFFFFFF, 25: (base >> 32) & 0xFFFFFFFF, 26: 64, 27: 0}
        memory = smem.SmemMemoryBank()
        memory.write_dword(expected_address, 0x4A6B7C8D)
        run = smem.SmemBodyEvaluator(gen, registers, memory, heap_base=HEAP)
        self.assertEqual(run.reads, [expected_address])
        self.assertEqual(run.sgpr[12], 0x4A6B7C8D)

    def test_scalar_negative_base_and_offset(self):
        self.scalar("s_buffer_load_dword s12, s[24:27], 12", FIELD, IMPORTED + 12)

    def test_scalar_negative_base_rounds_before_offset(self):
        self.scalar("s_buffer_load_dword s12, s[24:27], 15", FIELD + 3, IMPORTED + 12)

    def test_scalar_positive_base_unchanged(self):
        self.scalar("s_buffer_load_dword s12, s[24:27], 12", 0x2000, HEAP + 0x200C)

    def test_full64_scalar_pointer_is_not_sign_extended(self):
        self.scalar("s_load_dword s12, s[24:25], 12", FIELD, HEAP + FIELD + 12)
        self.scalar("s_load_dword s12, s[24:25], 12", 0xFFFF000000002000,
                    (HEAP + 0xFFFF00000000200C) & MASK64)

    def test_base_sign_extension_precedes_offset_addition(self):
        # The base is positive; adding 4 crosses bit47. Extending the final sum instead would be wrong.
        self.scalar("s_buffer_load_dword s12, s[24:27], 4", (1 << 47) - 4, HEAP + (1 << 47))

    def vector(self, operation, expected, initial=0x12345678, reads=True):
        lines = ["v_mov_b32 v1, 5", operation, "s_waitcnt vmcnt(0)",
                 "buffer_store_dword v1, off, s[0:3], 0", "s_endpgm"]
        asm = support.translate_lines("cs_signed48", lines, fssm=values._fssm(0x8090, (1, 1, 1)))
        push = [0x1000, 0, 64, 0x31016FAC,
                FIELD & 0xFFFFFFFF, FIELD >> 32, 64, 0x31016FAC] + [0] * 24
        push += [HEAP & 0xFFFFFFFF, HEAP >> 32, 0, 0]
        memory = values.ReadLog({IMPORTED + 12: initial})
        support.run_fragment(asm, inputs={"%wg_id": (0, 0, 0), "%local_id": (0, 0, 0)},
                             push_constants=push, memory=memory)
        self.assertEqual(dict.get(memory, HEAP + 0x1000), expected)
        if reads:
            self.assertIn(IMPORTED + 12, memory.reads)
        self.assertNotIn(IMPORTED + 12 + (1 << 48), memory.reads)
        # The older workgroup evaluator must execute the same signed extraction, not silently truncate it.
        if operation.startswith("buffer_load_dword"):
            import test_loop_ballot_barrier as wave
            other = wave.run_compute_workgroup(asm, 1, 32, push_constants=push,
                                                memory={IMPORTED + 12: initial})
            self.assertEqual(other[HEAP + 0x1000], expected)
        return memory

    def test_mubuf_load_negative_base(self):
        self.vector("buffer_load_dword v1, off, s[4:7], 0 offset:12", 0x12345678)

    def test_mtbuf_load_negative_base(self):
        self.vector("tbuffer_load_format_x v1, off, s[4:7], 0 offset:12 format:[BUF_FMT_32_FLOAT]", 0x12345678)

    def test_store_negative_base_writes_original_location(self):
        memory = self.vector("buffer_store_dword v1, off, s[4:7], 0 offset:12", 5, reads=False)
        self.assertEqual(dict.get(memory, IMPORTED + 12), 5)
        self.assertNotIn(IMPORTED + 12 + (1 << 48), memory)

    def test_global_full64_pointer_is_not_sign_extended(self):
        lines = [f"v_mov_b32 v2, {FIELD & 0xFFFFFFFF:#x}", f"v_mov_b32 v3, {FIELD >> 32:#x}",
                 "global_load_dword v1, v[2:3], off offset:12", "s_waitcnt vmcnt(0)",
                 "buffer_store_dword v1, off, s[0:3], 0", "s_endpgm"]
        asm = support.translate_lines("cs_signed48_global", lines, fssm=values._fssm(0x8088, (1, 1, 1)))
        expected_address = HEAP + FIELD + 12
        memory = values.ReadLog({expected_address: 0xA1B2C3D4})
        push = [0x1000, 0, 64, 0x31016FAC] + [0] * 28 + [HEAP & 0xFFFFFFFF, HEAP >> 32, 0, 0]
        support.run_fragment(asm, inputs={"%wg_id": (0, 0, 0), "%local_id": (0, 0, 0)},
                             push_constants=push, memory=memory)
        self.assertEqual(dict.get(memory, HEAP + 0x1000), 0xA1B2C3D4)
        self.assertIn(expected_address, memory.reads)
        self.assertNotIn(IMPORTED + 12, memory.reads)

    def test_old_unsigned_decoder_is_detected(self):
        from unittest import mock
        with mock.patch.object(support.rdna2spirv.Gen, "signed_vsharp_base", lambda self, base: base):
            with self.assertRaises(AssertionError):
                self.scalar("s_buffer_load_dword s12, s[24:27], 12", FIELD, IMPORTED + 12)
            with self.assertRaises(AssertionError):
                self.vector("buffer_load_dword v1, off, s[4:7], 0 offset:12", 0x12345678)

    def test_atomic_negative_base_reads_and_writes_same_bytes(self):
        memory = self.vector("buffer_atomic_add v1, off, s[4:7], 0 offset:12 glc", 0x12345678)
        self.assertEqual(dict.get(memory, IMPORTED + 12), 0x1234567D)


if __name__ == "__main__":
    unittest.main()