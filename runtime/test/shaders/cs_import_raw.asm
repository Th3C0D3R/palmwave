; group: 64,1,1
; rsrc2: 0x808c
; fssm-at: 0x150
; fssm-data: 0x52504d49 0x315f5741
; Palmwave: RAW buffer views of an imported host buffer (Xbox device slot 133) read and written by a translated shader
; through the V#s the layer wrote into a shader-visible descriptor table, next to RAW views of arena buffers in the
; same table. Run: d3d12_import_raw_view_test (a compute PSO found by the SHA-256 of its synthetic FSSM header).
; Nothing here knows where any buffer is: every address comes from a V# loaded from the table, and the translator adds
; the one heap_base the runtime pushes for every resource. Both V# readers run: the vector MUBUF path (buffer_load /
; buffer_store) and the scalar s_buffer_load path, each on the imported input's V#; under the signed48-v1 contract
; both sign-extend the 48-bit base before adding their offsets, so a view placed below heap_base works too.
; User data, from the root signature: [0] four 32-bit constants, [1] a descriptor table of four views. RSRC2 0x808c =
; six user SGPRs and TGID_X, so the group index is s6. D3D12 build: runtime/CMakeLists.txt builds this with
; PALMWAVE_USER_DATA_SHIFT=4, so in the PSO every SGPR named here is 4 higher (the console compute layout starts the
; root arguments at s4, D3D-448): the table pointer then reads s[8:9], s8 the table's 32-bit VA and s9 a user dword
; the runtime leaves zero.
;   s0 = seed   s1..s3 = unused
;   s[4:5] = console VA of the table's first slot. A slot is 32 bytes, its V# in the first 16.
;   slot 0 (+0x00): V#, RAW SRV of the imported input buffer, 256 words (read, vector and scalar)
;   slot 1 (+0x20): V#, RAW SRV of an arena (default-heap) buffer, 256 words (read)
;   slot 2 (+0x40): V#, RAW UAV of the imported output buffer, 256 words (read, then written in place)
;   slot 3 (+0x60): V#, RAW UAV of an arena (default-heap) buffer, 256 words (written)
; Scalar reads of slot 0, the same in every group (expected values in d3d12_import_raw_view_test.cpp, scalar_word):
;   k0 = s_buffer_load at SGPR offset 0x1f3: its low two bits are dropped, so byte 0x1f0, the view's word 124
;   k1 = s_buffer_load at immediate 0x3fc: the view's last word (255), whose end is exactly NUM_RECORDS (1024 bytes)
;   k2 = s_buffer_load at immediate 0x400: past NUM_RECORDS, so 0 and memory is not read
;   k = (k0 + k1) ^ k2
; g = 64 * group + lane, 0..255:  a = slot0[g]  b = slot1[g]  c = slot2[g]
;   slot2[g] = (a ^ b) + c
;   slot3[g] = rotr(a + seed, 7) ^ c ^ k
s_load_dwordx4 s[8:11], s[4:5], 0x0
s_load_dwordx4 s[12:15], s[4:5], 0x20
s_load_dwordx4 s[16:19], s[4:5], 0x40
s_load_dwordx4 s[20:23], s[4:5], 0x60
s_lshl_b32 s7, s6, 6
s_movk_i32 s24, 0x1f3
s_waitcnt lgkmcnt(0)
s_buffer_load_dword s25, s[8:11], s24
s_buffer_load_dword s26, s[8:11], 0x3fc
s_buffer_load_dword s27, s[8:11], 0x400
v_add_nc_u32_e32 v1, s7, v0
v_lshlrev_b32_e32 v2, 2, v1
buffer_load_dword v3, v2, s[8:11], 0 offen
buffer_load_dword v4, v2, s[12:15], 0 offen
buffer_load_dword v5, v2, s[16:19], 0 offen
s_waitcnt vmcnt(0) lgkmcnt(0)
s_add_u32 s25, s25, s26
s_xor_b32 s25, s25, s27
v_xor_b32_e32 v6, v3, v4
v_add_nc_u32_e32 v6, v6, v5
buffer_store_dword v6, v2, s[16:19], 0 offen
v_add_nc_u32_e32 v7, s0, v3
v_alignbit_b32 v7, v7, v7, 7
v_xor_b32_e32 v7, v7, v5
v_xor_b32_e32 v7, s25, v7
buffer_store_dword v7, v2, s[20:23], 0 offen
s_endpgm
