//! Synthetic GPU test for wave64 VS / PS whole-wave reductions in partial waves (where
//! unlaunched lanes inside a 32-lane or 64-lane wave would otherwise read 0 instead of the
//! reduction's identity element).
//!
//! No retail game files or `XCL_SAMPLE_*` variables are used: all RDNA2 shaders are assembled
//! directly from ISA encodings and executed on the local D3D12 GPU via `recomp::runner`.

use windows::Win32::Graphics::Dxgi::Common::{
    DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32_FLOAT,
};

use xcl::gpu::dxbc::SigElement;
use xcl::gpu::rdna2::encode;
use xcl::gpu::recomp::{self, dxc, runner, Mode, PsInterface, VsInterface};

const V: u64 = 256;

fn ins(name: &str, enc: &str, fields: &[(&str, u64)], lit: Option<u32>) -> Vec<u8> {
    encode(name, enc, fields, lit).unwrap_or_else(|e| panic!("{name} ({enc}): {e}"))
}

fn program_of(parts: &[Vec<u8>]) -> Vec<u8> {
    let mut code: Vec<u8> = parts.iter().flat_map(|p| p.iter().copied()).collect();
    code.extend(ins("S_ENDPGM", "ENC_SOPP", &[], None));
    code
}

fn gpu_ready() -> bool {
    match (runner::adapter(), runner::wave64_programs_supported(), dxc::location()) {
        (Ok(_), Ok(true), Ok(_)) => true,
        (a, w, d) => {
            runner::skip_test(if a.is_err() || w.is_err() { "no GPU" } else if d.is_err() { "no DXC" } else { "no wave64" });
            false
        }
    }
}

fn sig(name: &str, sv: u32, reg: u32, mask: u8) -> SigElement {
    SigElement {
        name: name.into(),
        index: 0,
        system_value: sv,
        component_type: 3,
        register: reg,
        mask,
        rw_mask: mask,
        stream: 0,
    }
}

fn vs_interface() -> VsInterface {
    VsInterface {
        inputs: vec![sig("POSITION", 0, 0, 0x3), sig("COLOR", 0, 1, 0xF)],
        outputs: vec![sig("SV_Position", 1, 0, 0xF), sig("COLOR", 0, 1, 0xF)],
        attr_base: 4,
        params: None,
    }
}

fn ps_interface() -> PsInterface {
    PsInterface {
        inputs: vec![sig("SV_Position", 1, 0, 0xF), sig("COLOR", 0, 1, 0xF)],
        outputs: vec![sig("SV_Target", 64, 0, 0xF)],
        input_ena: 0x302,
        attrs: None,
    }
}

fn passthrough_vs() -> Vec<u8> {
    program_of(&[
        ins("V_MOV_B32", "ENC_VOP1", &[("VDST", 0), ("SRC0", V + 4)], None),
        ins("V_MOV_B32", "ENC_VOP1", &[("VDST", 1), ("SRC0", V + 5)], None),
        ins("V_MOV_B32", "ENC_VOP1", &[("VDST", 2), ("SRC0", 128)], None),
        ins("V_MOV_B32", "ENC_VOP1", &[("VDST", 3), ("SRC0", 242)], None),
        ins(
            "EXP",
            "ENC_EXP",
            &[("TGT", 12), ("EN", 0xF), ("VSRC0", 0), ("VSRC1", 1), ("VSRC2", 2), ("VSRC3", 3), ("DONE", 1)],
            None,
        ),
        ins(
            "EXP",
            "ENC_EXP",
            &[("TGT", 32), ("EN", 0xF), ("VSRC0", 8), ("VSRC1", 9), ("VSRC2", 10), ("VSRC3", 11)],
            None,
        ),
    ])
}

fn passthrough_ps() -> Vec<u8> {
    program_of(&[
        ins("V_INTERP_P1_F32", "ENC_VINTRP", &[("VDST", 4), ("VSRC", 0), ("ATTR", 0), ("ATTRCHAN", 0)], None),
        ins("V_INTERP_P2_F32", "ENC_VINTRP", &[("VDST", 4), ("VSRC", 1), ("ATTR", 0), ("ATTRCHAN", 0)], None),
        ins("V_INTERP_P1_F32", "ENC_VINTRP", &[("VDST", 5), ("VSRC", 0), ("ATTR", 0), ("ATTRCHAN", 1)], None),
        ins("V_INTERP_P2_F32", "ENC_VINTRP", &[("VDST", 5), ("VSRC", 1), ("ATTR", 0), ("ATTRCHAN", 1)], None),
        ins("V_INTERP_P1_F32", "ENC_VINTRP", &[("VDST", 6), ("VSRC", 0), ("ATTR", 0), ("ATTRCHAN", 2)], None),
        ins("V_INTERP_P2_F32", "ENC_VINTRP", &[("VDST", 6), ("VSRC", 1), ("ATTR", 0), ("ATTRCHAN", 2)], None),
        ins("V_INTERP_P1_F32", "ENC_VINTRP", &[("VDST", 7), ("VSRC", 0), ("ATTR", 0), ("ATTRCHAN", 3)], None),
        ins("V_INTERP_P2_F32", "ENC_VINTRP", &[("VDST", 7), ("VSRC", 1), ("ATTR", 0), ("ATTRCHAN", 3)], None),
        ins(
            "EXP",
            "ENC_EXP",
            &[("TGT", 0), ("EN", 0xF), ("VSRC0", 4), ("VSRC1", 5), ("VSRC2", 6), ("VSRC3", 7), ("DONE", 1), ("VM", 1)],
            None,
        ),
    ])
}

/// Whole-wave reduction sequence matching console wave64 shader codegen:
/// `s_or_saveexec_b64` → `v_cndmask_b32` with identity → DPP8 xor 1/2/4 → DPP16 row_mirror →
/// `v_permlanex16_b32` + combine → `v_readlane_b32` 32 + combine → `v_readlane_b32` 0 → restore EXEC.
fn whole_wave_reduce(op: &str, identity_code: u64, identity_lit: Option<u32>, src_vgpr: u64, mask_sgpr: Option<u64>, out_sgpr: u64) -> Vec<u8> {
    let mut c = vec![ins("S_OR_SAVEEXEC_B64", "ENC_SOP1", &[("SDST", 30), ("SSRC0", 193)], None)];
    let mask = if let Some(m) = mask_sgpr {
        c.push(ins("S_AND_B64", "ENC_SOP2", &[("SDST", 10), ("SSRC0", m), ("SSRC1", 30)], None));
        10
    } else {
        30
    };
    c.push(ins(
        "V_CNDMASK_B32",
        "ENC_VOP3",
        &[("VDST", 6), ("SRC0", identity_code), ("SRC1", V + src_vgpr), ("SRC2", mask)],
        identity_lit,
    ));
    let names = ["LANE_SEL_0", "LANE_SEL_1", "LANE_SEL_2", "LANE_SEL_3", "LANE_SEL_4", "LANE_SEL_5", "LANE_SEL_6", "LANE_SEL_7"];
    for sel in [[1u64, 0, 3, 2, 5, 4, 7, 6], [2, 3, 0, 1, 6, 7, 4, 5], [4, 5, 6, 7, 0, 1, 2, 3]] {
        let mut f = vec![("VDST", 6u64), ("SRC0", 0xE9), ("VSRC0", 6), ("VSRC1", 6)];
        f.extend(names.iter().copied().zip(sel));
        c.push(ins(op, "VOP2_VOP_DPP8", &f, None));
    }
    c.extend([
        ins(
            op,
            "VOP2_VOP_DPP16",
            &[("VDST", 6), ("SRC0", 0xFA), ("VSRC0", 6), ("VSRC1", 6), ("DPP_CTRL", 0x140), ("ROW_MASK", 0xF), ("BANK_MASK", 0xF)],
            None,
        ),
        ins("V_PERMLANEX16_B32", "ENC_VOP3", &[("VDST", 7), ("SRC0", V + 6), ("SRC1", 128), ("SRC2", 128)], None),
        ins(op, "ENC_VOP2", &[("VDST", 6), ("SRC0", V + 7), ("VSRC1", 6)], None),
        ins("V_READLANE_B32", "ENC_VOP3", &[("VDST", 10), ("SRC0", V + 6), ("SRC1", 128 + 32)], None),
        ins(op, "ENC_VOP2", &[("VDST", 6), ("SRC0", 10), ("VSRC1", 6)], None),
        ins("V_READLANE_B32", "ENC_VOP3", &[("VDST", out_sgpr), ("SRC0", V + 6), ("SRC1", 128)], None),
        ins("S_MOV_B64", "ENC_SOP1", &[("SDST", 126), ("SSRC0", 30)], None),
    ]);
    c.concat()
}

fn draw_16x16(vs: Option<&[u8]>, ps: Option<&[u8]>, verts_px: &[[f32; 2]]) -> Vec<[f32; 4]> {
    let pvs = passthrough_vs();
    let pps = passthrough_ps();
    let compile = |code: &[u8], mode: Mode, profile: &str| {
        let t = recomp::translate(code, &runner::options(mode)).unwrap();
        assert!(t.complete(), "incomplete translation: {:?}", t.unsupported_why);
        dxc::compile(&t.hlsl, "main", profile, &[]).unwrap_or_else(|e| panic!("{e}\n{}", t.hlsl))
    };
    let vsb = compile(vs.unwrap_or(&pvs), Mode::Vertex(vs_interface()), "vs_6_6");
    let psb = compile(ps.unwrap_or(&pps), Mode::Pixel(ps_interface()), "ps_6_6");
    let verts: Vec<f32> = verts_px.iter().flat_map(|p| [p[0] / 8.0 - 1.0, 1.0 - p[1] / 8.0, 0.0, 0.0, 0.0, 0.0]).collect();
    let layout = [
        runner::VertexElement { semantic: "POSITION", index: 0, format: DXGI_FORMAT_R32G32_FLOAT, offset: 0 },
        runner::VertexElement { semantic: "COLOR", index: 0, format: DXGI_FORMAT_R32G32B32A32_FLOAT, offset: 8 },
    ];
    runner::draw(&vsb, &psb, &layout, &verts, 24, &[], &[], 16, [-1.0; 4]).expect("GPU draw failed")
}

/// Partial-wave pixel shader whole-wave reductions (`min_u32`, `max_u32`, `max_i32` over
/// negative values, and `and_b32`). On a 3-quad triangle (6 covered pixels, lanes 12..31
/// unlaunched), unlaunched lanes read 0 on Wave32 GPUs; without whole-wave reduction lowering,
/// `min_u32` (identity -1), `and_b32` (identity -1), and `max_i32` over negative inputs
/// (identity i32::MIN) all return 0 instead of the true wave reduction.
#[test]
fn gpu_wave32_partial_ps_min_max_reductions() {
    if !gpu_ready() {
        return;
    }
    let ps = program_of(&[
        // v12 = y * 16 + x, v13 = v12 + 100 (positive, >= 100), v14 = -(v12 + 10) (negative i32)
        ins("V_CVT_U32_F32", "ENC_VOP1", &[("VDST", 10), ("SRC0", V + 2)], None),
        ins("V_CVT_U32_F32", "ENC_VOP1", &[("VDST", 11), ("SRC0", V + 3)], None),
        ins("V_LSHL_ADD_U32", "ENC_VOP3", &[("VDST", 12), ("SRC0", V + 11), ("SRC1", 128 + 4), ("SRC2", V + 10)], None),
        ins("V_ADD_NC_U32", "ENC_VOP2", &[("VDST", 13), ("SRC0", 255), ("VSRC1", 12)], Some(100)),
        // v14 = -100 - v12 = 0u32.wrapping_sub(v13)
        ins("V_SUB_NC_U32", "ENC_VOP2", &[("VDST", 14), ("SRC0", 128), ("VSRC1", 13)], None),
        // whole-wave reductions over all active pixels in the partial wave:
        // s40 = min_u32(v13)   (identity 0xFFFFFFFF)
        // s41 = max_u32(v13)   (identity 0)
        // s42 = max_i32(v14)   (identity i32::MIN; inputs are all <= -100)
        whole_wave_reduce("V_MIN_U32", 193, None, 13, None, 40),
        whole_wave_reduce("V_MAX_U32", 128, None, 13, None, 41),
        whole_wave_reduce("V_MAX_I32", 255, Some(i32::MIN as u32), 14, None, 42),
        ins("V_CVT_F32_U32", "ENC_VOP1", &[("VDST", 20), ("SRC0", 40)], None),
        ins("V_CVT_F32_U32", "ENC_VOP1", &[("VDST", 21), ("SRC0", 41)], None),
        ins("V_CVT_F32_I32", "ENC_VOP1", &[("VDST", 22), ("SRC0", 42)], None),
        ins("V_MOV_B32", "ENC_VOP1", &[("VDST", 23), ("SRC0", 242)], None),
        ins(
            "EXP",
            "ENC_EXP",
            &[("TGT", 0), ("EN", 0xF), ("VSRC0", 20), ("VSRC1", 21), ("VSRC2", 22), ("VSRC3", 23), ("DONE", 1), ("VM", 1)],
            None,
        ),
    ]);

    // 3-quad triangle: covers 6 pixels (0..2, 0), (0..1, 1), (0, 2) → pixel IDs 0, 1, 2, 16, 17, 32
    let small = draw_16x16(None, Some(&ps), &[[0.0, 0.0], [3.2, 0.0], [0.0, 3.2]]);
    let covered: Vec<(u32, [f32; 4])> = small.iter().enumerate().filter(|(_, p)| p[3] == 1.0).map(|(i, p)| (i as u32, *p)).collect();
    assert_eq!(covered.len(), 6, "expected 6 covered pixels in 3-quad triangle");

    let want_min_u32 = covered.iter().map(|(id, _)| id + 100).min().unwrap();
    let want_max_u32 = covered.iter().map(|(id, _)| id + 100).max().unwrap();
    let want_max_i32 = covered.iter().map(|(id, _)| -((id + 100) as i32)).max().unwrap();
    for (id, p) in &covered {
        assert_eq!(p[0] as u32, want_min_u32, "pixel {id}: partial-wave min_u32 (identity -1)");
        assert_eq!(p[1] as u32, want_max_u32, "pixel {id}: partial-wave max_u32 (identity 0)");
        assert_eq!(p[2] as i32, want_max_i32, "pixel {id}: partial-wave max_i32 over negatives (identity i32::MIN)");
    }

    // 1-quad triangle: covers 1 pixel (0, 0) → lanes 4..31 unlaunched
    let tiny = draw_16x16(None, Some(&ps), &[[0.0, 0.0], [1.6, 0.0], [0.0, 1.6]]);
    let covered_tiny: Vec<(u32, [f32; 4])> = tiny.iter().enumerate().filter(|(_, p)| p[3] == 1.0).map(|(i, p)| (i as u32, *p)).collect();
    assert_eq!(covered_tiny.len(), 1, "expected 1 covered pixel in 1-quad triangle");
    let (id, p) = covered_tiny[0];
    assert_eq!(p[0] as u32, id + 100, "1-quad partial-wave min_u32");
    assert_eq!(p[1] as u32, id + 100, "1-quad partial-wave max_u32");
    assert_eq!(p[2] as i32, -((id + 100) as i32), "1-quad partial-wave max_i32");
}

/// Partial-wave vertex shader whole-wave reductions: a single triangle (3 vertices) runs in a
/// 3-lane partial wave (lanes 3..31 / 3..63 unlaunched). Verifies `min_u32`, `max_u32`, and
/// `min_i32` over vertex IDs {0, 1, 2}.
#[test]
fn gpu_wave32_partial_vs_min_max_reductions() {
    if !gpu_ready() {
        return;
    }
    let vs = program_of(&[
        // v12 = vertex_id (v0 at VS entry is SV_VertexID) + 50 → {50, 51, 52}
        ins("V_ADD_NC_U32", "ENC_VOP2", &[("VDST", 12), ("SRC0", 128 + 50), ("VSRC1", 0)], None),
        ins("V_MOV_B32", "ENC_VOP1", &[("VDST", 0), ("SRC0", V + 4)], None),
        ins("V_MOV_B32", "ENC_VOP1", &[("VDST", 1), ("SRC0", V + 5)], None),
        ins("V_MOV_B32", "ENC_VOP1", &[("VDST", 2), ("SRC0", 128)], None),
        ins("V_MOV_B32", "ENC_VOP1", &[("VDST", 3), ("SRC0", 242)], None),
        whole_wave_reduce("V_MIN_U32", 193, None, 12, None, 40),
        whole_wave_reduce("V_MAX_U32", 128, None, 12, None, 41),
        whole_wave_reduce("V_MIN_I32", 255, Some(i32::MAX as u32), 12, None, 42),
        ins("V_CVT_F32_U32", "ENC_VOP1", &[("VDST", 8), ("SRC0", 40)], None),
        ins("V_CVT_F32_U32", "ENC_VOP1", &[("VDST", 9), ("SRC0", 41)], None),
        ins("V_CVT_F32_I32", "ENC_VOP1", &[("VDST", 10), ("SRC0", 42)], None),
        ins("V_MOV_B32", "ENC_VOP1", &[("VDST", 11), ("SRC0", 242)], None),
        ins(
            "EXP",
            "ENC_EXP",
            &[("TGT", 12), ("EN", 0xF), ("VSRC0", 0), ("VSRC1", 1), ("VSRC2", 2), ("VSRC3", 3), ("DONE", 1)],
            None,
        ),
        ins(
            "EXP",
            "ENC_EXP",
            &[("TGT", 32), ("EN", 0xF), ("VSRC0", 8), ("VSRC1", 9), ("VSRC2", 10), ("VSRC3", 11)],
            None,
        ),
    ]);
    let px = draw_16x16(Some(&vs), None, &[[0.0, 0.0], [0.0, 32.0], [32.0, 0.0]]);
    for (i, p) in px.iter().enumerate() {
        assert_eq!(p[0] as u32, 50, "pixel {i}: VS 3-vertex partial-wave min_u32");
        assert_eq!(p[1] as u32, 52, "pixel {i}: VS 3-vertex partial-wave max_u32");
        assert_eq!(p[2] as i32, 50, "pixel {i}: VS 3-vertex partial-wave min_i32");
    }
}
