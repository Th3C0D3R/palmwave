//! Wave64 vertex / pixel shaders in 32-lane hardware waves (docs/wave64-on-wave32.md,
//! "Vertex and pixel shaders"): what the console's lanes 32..63 hold where the program reads
//! them.
//!
//! The driver picks the wave size of a VS or PS, and a GPU with only 32-lane waves (NVIDIA)
//! runs a console wave64 program in waves of 32. Such a wave stands for a console wave whose
//! lanes 32..63 are not populated, as in the last wave of a draw: inactive, but on the console
//! their VGPRs exist and can still be written – by `v_writelane`, which ignores EXEC, and by
//! whole-wave code (EXEC's upper word set, `s_or_saveexec_b64 s, -1`). Here those lanes do
//! not exist. A read of one – `v_readlane` of a lane ≥ 32 or of a lane from an SGPR,
//! `ds_bpermute` – needs the value the console lane would hold. This forward analysis follows
//! it per VGPR lane:
//! - *unwritten*: nothing wrote the lane, it holds what an earlier wave left in the register.
//!   A program cannot depend on that, so any value is as right as the console's;
//! - a *constant*: whole-wave code wrote it – the identity a wave reduction puts into the
//!   lanes outside its mask, which the DPP / `v_permlanex16` steps keep, since they only
//!   combine lanes of the same 32-lane half;
//! - a *shadow*: `v_writelane` wrote it (SGPR spill slots); the emitter keeps a scalar copy.
//!
//! Lanes < 32 exist but need not have been launched (a partial wave, as pixel waves at a
//! primitive's edge often are), and a lane that was not launched loses what `v_writelane`
//! put there: a spill slot < 32 is read from its shadow too – whatever the wave size – as long
//! as no other instruction has written the VGPR since its `v_writelane`.
//!
//! Which instructions write lanes 32..63 depends on EXEC's upper word, followed through the
//! SGPRs as 32-bit words known to be 0, all ones, or anything: 0 at the start; a VALU mask
//! (`v_cmp*`, carries) has a 0 upper word while EXEC's is 0; bitwise SALU and the saveexec
//! forms combine words; everything else is "anything". A VALU write while EXEC's upper word
//! is 0 leaves lanes 32..63 alone; while it is all ones it writes them with the value
//! evaluated here (moves, selects, integer min / max / and / or / xor / add, their DPP forms,
//! `v_permlane(x)16`), and otherwise their state becomes unknown.

use std::collections::{BTreeMap, BTreeSet};

use super::emit::ext_encoding;
use crate::gpu::rdna2::{self, Decoded, Variant, F_CONDITIONAL, F_INDIRECT, F_TERMINATOR};

/// An SGPR's value as far as the analysis needs it.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum Word {
    Zero,
    Ones,
    Any,
}

impl Word {
    fn of(v: u32) -> Word {
        match v {
            0 => Word::Zero,
            u32::MAX => Word::Ones,
            _ => Word::Any,
        }
    }
    fn join(self, o: Word) -> Word {
        if self == o {
            self
        } else {
            Word::Any
        }
    }
    fn not(self) -> Word {
        match self {
            Word::Zero => Word::Ones,
            Word::Ones => Word::Zero,
            Word::Any => Word::Any,
        }
    }
    fn and(self, o: Word) -> Word {
        match (self, o) {
            (Word::Zero, _) | (_, Word::Zero) => Word::Zero,
            (Word::Ones, Word::Ones) => Word::Ones,
            _ => Word::Any,
        }
    }
    fn or(self, o: Word) -> Word {
        self.not().and(o.not()).not()
    }
    fn xor(self, o: Word) -> Word {
        match (self, o) {
            (Word::Any, _) | (_, Word::Any) => Word::Any,
            (a, b) if a == b => Word::Zero,
            _ => Word::Ones,
        }
    }
    /// The 32-bit value, when it is one.
    fn value(self) -> Option<u32> {
        match self {
            Word::Zero => Some(0),
            Word::Ones => Some(u32::MAX),
            Word::Any => None,
        }
    }
}

/// A bitwise SALU operation by its name's second part (`S_ANDN2_SAVEEXEC_B64` → "ANDN2").
fn bitop(op: &str, a: Word, b: Word) -> Option<Word> {
    Some(match op {
        "AND" => a.and(b),
        "OR" => a.or(b),
        "XOR" => a.xor(b),
        "ANDN2" => a.and(b.not()),
        "ORN2" => a.or(b.not()),
        "NAND" => a.and(b).not(),
        "NOR" => a.or(b).not(),
        "XNOR" => a.xor(b).not(),
        "ANDN1" => a.not().and(b),
        "ORN1" => a.not().or(b),
        _ => return None,
    })
}

/// What a lane ≥ 32 of a VGPR holds on the console.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum Hi {
    Unwritten,
    Const(u32),
    Shadow,
    Unknown,
}

impl Hi {
    /// Either of the two (two paths, or a lane a mask may or may not select): an unwritten
    /// lane accepts any value, so it takes the other's.
    fn join(self, o: Hi) -> Hi {
        match (self, o) {
            (Hi::Unwritten, x) | (x, Hi::Unwritten) => x,
            (a, b) if a == b => a,
            _ => Hi::Unknown,
        }
    }
}

const UNWRITTEN: [Hi; 32] = [Hi::Unwritten; 32];

/// Associative, commutative integer wave reduction lowered to HLSL `WaveActive*`.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum ReduceOp {
    MinU32,
    MaxU32,
    MinI32,
    MaxI32,
    Or,
    And,
    Xor,
    Add,
}

impl ReduceOp {
    fn from_valu(name: &str) -> Option<Self> {
        Some(match name {
            "V_MIN_U32" => Self::MinU32,
            "V_MAX_U32" => Self::MaxU32,
            "V_MIN_I32" => Self::MinI32,
            "V_MAX_I32" => Self::MaxI32,
            "V_OR_B32" => Self::Or,
            "V_AND_B32" => Self::And,
            "V_XOR_B32" => Self::Xor,
            "V_ADD_NC_U32" => Self::Add,
            _ => return None,
        })
    }

    fn identity(self) -> u32 {
        match self {
            Self::MinU32 | Self::And => u32::MAX,
            Self::MaxU32 | Self::Or | Self::Xor | Self::Add => 0,
            Self::MinI32 => i32::MAX as u32,
            Self::MaxI32 => i32::MIN as u32,
        }
    }

    fn is_idempotent(self) -> bool {
        !matches!(self, Self::Xor | Self::Add)
    }

    fn combine(self, a: u64, b: u64) -> u64 {
        if a == 0 || b == 0 {
            return 0;
        }
        if !self.is_idempotent() && (a & b) != 0 {
            return 0;
        }
        a | b
    }

    pub fn hlsl(self, v: &str) -> String {
        match self {
            Self::MinU32 => format!("WaveActiveMin({v})"),
            Self::MaxU32 => format!("WaveActiveMax({v})"),
            Self::MinI32 => format!("asuint(WaveActiveMin(asint({v})))"),
            Self::MaxI32 => format!("asuint(WaveActiveMax(asint({v})))"),
            Self::Or => format!("WaveActiveBitOr({v})"),
            Self::And => format!("WaveActiveBitAnd({v})"),
            Self::Xor => format!("WaveActiveBitXor({v})"),
            Self::Add => format!("WaveActiveSum({v})"),
        }
    }
}

/// Per-lane lane-set state of an in-progress whole-wave reduction held in a VGPR.
#[derive(Clone, PartialEq, Eq, Debug)]
struct VReduce {
    seed_pc: u64,
    src_vgpr: u64,
    op: ReduceOp,
    sets: [u64; 64],
}

/// Scalar lane-set state of an in-progress whole-wave reduction held in an SGPR.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
struct SReduce {
    seed_pc: u64,
    src_vgpr: u64,
    op: ReduceOp,
    set: u64,
}

#[derive(Clone, PartialEq)]
struct State {
    s: [Word; 128],
    /// What each SGPR holds in inactive (unlaunched + helper) lanes of 0..31.
    /// Like `s[127]` for lanes 32..63, except `S_WQM` sets helper lanes to `Any`.
    inact_lo: [Word; 128],
    /// VGPR → its lanes 32..63 (absent: all unwritten)
    hi: BTreeMap<u64, [Hi; 32]>,
    /// VGPR → its lanes < 32 (bit n: lane n) whose last write was a `v_writelane`
    lo: BTreeMap<u64, u32>,
    /// VGPR → constant held by every inactive (unlaunched + helper) lane across 0..63
    inact_const: BTreeMap<u64, u32>,
    /// VGPR → in-progress whole-wave reduction state
    red_v: BTreeMap<u64, VReduce>,
    /// SGPR → in-progress whole-wave reduction state (from `v_readlane`)
    red_s: BTreeMap<u64, SReduce>,
}

impl State {
    fn entry() -> State {
        let mut s = [Word::Any; 128];
        s[125] = Word::Zero; // NULL
        s[127] = Word::Zero; // EXEC's upper word: lanes 32..63 are not populated
        let mut inact_lo = [Word::Any; 128];
        inact_lo[125] = Word::Zero;
        inact_lo[126] = Word::Zero; // EXEC's low word: unlaunched + helper lanes are inactive
        State {
            s,
            inact_lo,
            hi: BTreeMap::new(),
            lo: BTreeMap::new(),
            inact_const: BTreeMap::new(),
            red_v: BTreeMap::new(),
            red_s: BTreeMap::new(),
        }
    }

    fn join(&self, o: &State) -> State {
        let mut s = self.s;
        for (a, b) in s.iter_mut().zip(o.s) {
            *a = a.join(b);
        }
        let mut inact_lo = self.inact_lo;
        for (a, b) in inact_lo.iter_mut().zip(o.inact_lo) {
            *a = a.join(b);
        }
        let mut hi = self.hi.clone();
        for (v, l) in &o.hi {
            let e = hi.entry(*v).or_insert(UNWRITTEN);
            for (a, b) in e.iter_mut().zip(l) {
                *a = a.join(*b);
            }
        }
        let lo = self
            .lo
            .iter()
            .filter_map(|(v, m)| {
                let m = m & o.lo.get(v).copied().unwrap_or(0);
                (m != 0).then_some((*v, m))
            })
            .collect();
        let inact_const = self.inact_const.iter().filter(|(v, c)| o.inact_const.get(v) == Some(*c)).map(|(&v, &c)| (v, c)).collect();
        let red_v = self.red_v.iter().filter(|(v, r)| o.red_v.get(v) == Some(*r)).map(|(&v, r)| (v, r.clone())).collect();
        let red_s = self.red_s.iter().filter(|(r, s)| o.red_s.get(r) == Some(*s)).map(|(&r, &s)| (r, s)).collect();
        State { s, inact_lo, hi, lo, inact_const, red_v, red_s }
    }

    fn upper(&self, v: u64) -> [Hi; 32] {
        self.hi.get(&v).copied().unwrap_or(UNWRITTEN)
    }

    fn set_upper(&mut self, v: u64, lanes: [Hi; 32]) {
        if lanes == UNWRITTEN {
            self.hi.remove(&v);
        } else {
            self.hi.insert(v, lanes);
        }
    }

    fn set(&mut self, r: u64, w: Word) {
        if r < 128 && r != 125 {
            self.s[r as usize] = w;
        }
    }

    fn set_inact_lo(&mut self, r: u64, w: Word) {
        if r < 128 && r != 125 {
            self.inact_lo[r as usize] = w;
        }
    }

    /// A 32-bit scalar source in inactive lanes of 0..31.
    fn src_inact_lo(&self, d: &Decoded, code: u64) -> Word {
        match code {
            0..=124 | 126 | 127 => self.inact_lo[code as usize],
            125 | 128 => Word::Zero,
            193 => Word::Ones,
            255 => d.literal.map_or(Word::Any, Word::of),
            _ => Word::Any,
        }
    }

    /// A 32-bit scalar source (operand code).
    fn src32(&self, d: &Decoded, code: u64) -> Word {
        match code {
            0..=124 | 126 | 127 => self.s[code as usize],
            125 | 128 => Word::Zero,
            193 => Word::Ones,
            255 => d.literal.map_or(Word::Any, Word::of),
            _ => Word::Any,
        }
    }

    /// A 64-bit scalar source (operand code) as (low, high) words; a 32-bit literal is the
    /// low word of an integer operand, inline integers are sign-extended (as `Em::code64`).
    fn src64(&self, d: &Decoded, code: u64) -> (Word, Word) {
        match code {
            0..=123 | 126 => (self.s[code as usize], self.s[code as usize + 1]),
            125 | 128 => (Word::Zero, Word::Zero),
            129..=192 => (Word::Any, Word::Zero),
            193 => (Word::Ones, Word::Ones),
            194..=208 => (Word::Any, Word::Ones),
            255 => (d.literal.map_or(Word::Any, Word::of), Word::Zero),
            _ => (Word::Any, Word::Any),
        }
    }
}

/// How the 32-lane form resolves a lane read (`UpperLanes::reads`).
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Read {
    /// the read as it is: the lanes ≥ 32 it can reach are unwritten (or, for `ds_bpermute`,
    /// inactive, which reads 0 here as on the console)
    Native,
    /// a lane ≥ 32 holds this constant
    Const(u32),
    /// a spill slot: from the shadow `v_writelane` keeps
    Shadow,
    /// not followed: the read stays a hazard
    Unknown,
}

#[derive(Default)]
pub struct UpperLanes {
    /// pc → resolution, for every cross-lane read that can reach a lane ≥ 32 and every
    /// `v_readlane` of a spill slot
    pub reads: BTreeMap<u64, Read>,
    /// (VGPR, lane) slots read from their shadow: their `v_writelane`s also write it
    pub shadows: BTreeSet<(u64, u32)>,
    /// pc of the first cross-lane step of a recognised whole-wave reduction → (input VGPR, op)
    pub reduction_seeds: BTreeMap<u64, (u64, ReduceOp)>,
    /// pc of the `v_readlane` / `v_readfirstlane` reading a recognised whole-wave reduction →
    /// seed pc (`xcl_red_<seed>`)
    pub reduction_results: BTreeMap<u64, u64>,
}

impl UpperLanes {
    /// Reads that stay hazards, by kind (`super::wave_hazards`).
    pub fn hazards(&self, insts: &[Decoded]) -> BTreeSet<&'static str> {
        let mut out = BTreeSet::new();
        for d in insts {
            if self.reads.get(&d.pc) == Some(&Read::Unknown) {
                out.insert(match (d.inst.name, lane_operand(d)) {
                    ("V_READLANE_B32", Lane::Const(_)) => "v_readlane: lane ≥ 32",
                    ("V_READLANE_B32", _) => "v_readlane: lane number from an SGPR",
                    _ => "ds_(b)permute: arbitrary lane",
                });
            }
        }
        out
    }

    /// Reads resolved, by kind (for `xcl shaders --recomp`).
    pub fn resolved(&self) -> BTreeSet<&'static str> {
        self.reads
            .values()
            .filter_map(|r| match r {
                Read::Native => Some("lanes ≥ 32 never written (or inactive)"),
                Read::Const(_) => Some("lane ≥ 32 holds a constant (whole-wave identity)"),
                Read::Shadow => Some("spill slot from its v_writelane shadow"),
                Read::Unknown => None,
            })
            .collect()
    }
}

/// A read that can reach a lane ≥ 32 whatever the state (`classify` follows it).
fn reaches_upper(d: &Decoded) -> bool {
    match d.inst.name {
        "V_READLANE_B32" => !matches!(lane_operand(d), Lane::Const(l) if l < 32),
        "DS_BPERMUTE_B32" | "DS_PERMUTE_B32" => true,
        _ => false,
    }
}

/// The (VGPR, lane) of a `v_readlane` / `v_writelane` with a constant lane: the slot of its
/// shadow (`UpperLanes::shadows`).
pub fn slot(d: &Decoded) -> Option<(u64, u32)> {
    let f = |n: &str| rdna2::field_value(d.enc, &d.words, n).unwrap_or(0);
    let v = match d.inst.name {
        "V_READLANE_B32" => f("SRC0").checked_sub(256)?,
        "V_WRITELANE_B32" => f("VDST"),
        _ => return None,
    };
    match lane_operand(d) {
        Lane::Const(l) => Some((v, l)),
        Lane::Sgpr => None,
    }
}

/// The lane operand (SRC1) of `v_readlane` / `v_writelane`.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum Lane {
    Const(u32),
    Sgpr,
}

fn lane_operand(d: &Decoded) -> Lane {
    let code = rdna2::field_value(d.enc, &d.words, "SRC1").unwrap_or(0);
    match code {
        128..=192 => Lane::Const((code - 128) as u32 & 63),
        193..=208 => Lane::Const((-((code - 192) as i32)) as u32 & 63),
        240..=248 => Lane::Const(f32_inline(code).unwrap_or(0) & 63),
        255 => d.literal.map_or(Lane::Sgpr, |l| Lane::Const(l & 63)),
        125 => Lane::Const(0),
        _ => Lane::Sgpr,
    }
}

/// Source lane of a DPP16 control for `lane` (None: no source), as `emit::dpp16_src`.
fn dpp16_lane(ctrl: u64, lane: u32) -> Option<Option<u32>> {
    let n = (ctrl & 15) as u32;
    let row = lane & !15;
    let i = lane & 15;
    Some(match ctrl {
        0x000..=0x0FF => Some((lane & !3) | ((ctrl as u32 >> (2 * (lane & 3))) & 3)),
        0x101..=0x10F => (i + n < 16).then_some(lane + n),
        0x111..=0x11F => (i >= n).then(|| lane - n),
        0x121..=0x12F => Some(row | ((i.wrapping_sub(n)) & 15)),
        0x140 => Some(row | (15 - i)),
        0x141 => Some(row | (i & 8) | (7 - (i & 7))),
        0x150..=0x15F => Some(row | n),
        0x160..=0x16F => Some(lane ^ n),
        _ => return None,
    })
}

fn f32_inline(code: u64) -> Option<u32> {
    Some(
        match code {
            240 => 0.5f32,
            241 => -0.5,
            242 => 1.0,
            243 => -1.0,
            244 => 2.0,
            245 => -2.0,
            246 => 4.0,
            247 => -4.0,
            248 => 0.159_154_94,
            _ => return None,
        }
        .to_bits(),
    )
}

/// A VALU source (9-bit operand code) in console lane `32 + l`.
fn operand(st: &State, d: &Decoded, code: u64, l: usize) -> Hi {
    match code {
        256..=511 => st.upper(code - 256)[l],
        128..=192 => Hi::Const((code - 128) as u32),
        193..=208 => Hi::Const((-((code - 192) as i32)) as u32),
        240..=248 => Hi::Const(f32_inline(code).unwrap_or(0)),
        255 => d.literal.map_or(Hi::Unknown, Hi::Const),
        _ => st.src32(d, code).value().map_or(Hi::Unknown, Hi::Const),
    }
}

/// A two-source integer operation on the values of one lane.
fn binary(name: &str, a: Hi, b: Hi) -> Option<Hi> {
    let f: fn(u32, u32) -> u32 = match name {
        "V_MIN_U32" => |a, b| a.min(b),
        "V_MAX_U32" => |a, b| a.max(b),
        "V_MIN_I32" => |a, b| (a as i32).min(b as i32) as u32,
        "V_MAX_I32" => |a, b| (a as i32).max(b as i32) as u32,
        "V_OR_B32" => |a, b| a | b,
        "V_AND_B32" => |a, b| a & b,
        "V_XOR_B32" => |a, b| a ^ b,
        "V_ADD_NC_U32" => |a, b| a.wrapping_add(b),
        _ => return None,
    };
    // a constant that decides the result whatever the other operand holds
    let absorbing = |c: u32| match name {
        "V_AND_B32" | "V_MIN_U32" => c == 0,
        "V_OR_B32" | "V_MAX_U32" => c == u32::MAX,
        "V_MIN_I32" => c == i32::MIN as u32,
        "V_MAX_I32" => c == i32::MAX as u32,
        _ => false,
    };
    Some(match (a, b) {
        (Hi::Const(x), Hi::Const(y)) => Hi::Const(f(x, y)),
        (Hi::Unknown | Hi::Shadow, _) | (_, Hi::Unknown | Hi::Shadow) => Hi::Unknown,
        (Hi::Const(c), Hi::Unwritten) | (Hi::Unwritten, Hi::Const(c)) if absorbing(c) => Hi::Const(c),
        // x ^ x is 0 whatever x holds, and the operands may be the same register
        _ if name == "V_XOR_B32" => Hi::Unknown,
        _ => Hi::Unwritten, // computed from a lane nothing wrote
    })
}

/// Lanes 32..63 of `vdst` after a VALU instruction that writes all of them (EXEC's upper
/// word all ones); None: not followed.
fn evaluate(d: &Decoded, st: &State, vdst: u64) -> Option<[Hi; 32]> {
    let name = d.inst.name;
    let fv = |f: &str| rdna2::field_value(d.enc, &d.words, f).unwrap_or(0);
    let old = st.upper(vdst);
    if d.enc.name == "ENC_VOP3" && (fv("ABS") != 0 || fv("NEG") != 0 || fv("CLAMP") != 0 || fv("OMOD") != 0) {
        return None;
    }
    if matches!(name, "V_PERMLANE16_B32" | "V_PERMLANEX16_B32") {
        // lane i reads a lane chosen at run time in its own 32-lane half (rows 2 and 3 here):
        // followed when they all hold the same; FI / bound_ctrl only matter for inactive
        // sources, and EXEC's upper word is all ones
        let src = st.upper(fv("SRC0").checked_sub(256)?);
        let first = src[0];
        return src.iter().all(|&x| x == first).then_some([first; 32]).filter(|_| first != Hi::Shadow);
    }
    // source operands: (code of src0 as a 9-bit operand or a VGPR + 256, src1 code)
    let (mut src0, src1, src2) = match d.enc.name {
        "ENC_VOP1" => (fv("SRC0"), None, None),
        "ENC_VOP2" => (fv("SRC0"), Some(256 + fv("VSRC1")), None),
        "ENC_VOP3" => (fv("SRC0"), Some(fv("SRC1")), Some(fv("SRC2"))),
        _ => return None,
    };
    let mut dpp16 = None;
    let mut dpp8 = None;
    match d.variant {
        Variant::Plain => {}
        Variant::Sdwa => return None,
        Variant::Dpp16 => {
            let e = ext_encoding(d)?;
            let x = |n: &str| rdna2::field_value(e, &d.words, n).unwrap_or(0);
            src0 = 256 + x("VSRC0");
            dpp16 = Some((x("DPP_CTRL"), x("BOUND_CTRL") == 1, x("ROW_MASK"), x("BANK_MASK")));
        }
        Variant::Dpp8 => {
            let e = ext_encoding(d)?;
            let x = |n: &str| rdna2::field_value(e, &d.words, n).unwrap_or(0);
            src0 = 256 + x("VSRC0");
            let sel: Vec<u32> = (0..8).map(|i| x(&format!("LANE_SEL_{i}")) as u32).collect();
            dpp8 = Some(sel);
        }
    }
    let mut out = old;
    for (l, o) in out.iter_mut().enumerate() {
        let lane = 32 + l as u32;
        // src0 of this lane after DPP, or None: the lane is not written
        let a = if let Some((ctrl, bound, rm, bm)) = dpp16 {
            if (rm >> (lane >> 4)) & 1 == 0 || (bm >> ((lane >> 2) & 3)) & 1 == 0 {
                continue;
            }
            // every lane ≥ 32 is active (FI does not matter)
            match dpp16_lane(ctrl, lane)? {
                Some(s) => operand(st, d, src0, s as usize - 32),
                None if bound => Hi::Const(0),
                None => continue,
            }
        } else if let Some(sel) = &dpp8 {
            let s = (lane & !7) | sel[(lane & 7) as usize];
            operand(st, d, src0, s as usize - 32)
        } else {
            operand(st, d, src0, l)
        };
        *o = match name {
            "V_MOV_B32" if src1.is_none() => match a {
                Hi::Shadow => Hi::Unknown,
                x => x,
            },
            "V_CNDMASK_B32" => {
                let b = operand(st, d, src1?, l);
                // the mask's upper word: VCC (VOP2) or SRC2
                let m = match src2 {
                    None => st.s[107],
                    Some(c) => st.src64(d, c).1,
                };
                let r = match m {
                    Word::Zero => a,
                    Word::Ones => b,
                    Word::Any => a.join(b),
                };
                if r == Hi::Shadow {
                    Hi::Unknown
                } else {
                    r
                }
            }
            _ => binary(name, a, operand(st, d, src1?, l))?,
        };
    }
    Some(out)
}

/// SGPRs an instruction may write (first register, count), from its output operands.
fn sgpr_outputs(d: &Decoded) -> Vec<(u64, u64)> {
    let mut out = Vec::new();
    for o in d.ienc.operands.iter().filter(|o| o.output) {
        let n = (o.size as u64).div_ceil(32).max(1);
        match (o.field, o.ty) {
            (_, "OPR_VCC") => out.push((106, 2)),
            (_, "OPR_EXEC" | "OPR_SDST_EXEC") => out.push((126, 2)),
            (_, "OPR_SDST_M0") => out.push((124, 1)),
            (Some(f), ty) if !ty.starts_with("OPR_VGPR") && ty != "OPR_SRC_VGPR" => {
                let v = rdna2::field_value(d.enc, &d.words, f).unwrap_or(0) * rdna2::register_scale(f);
                if v < 128 {
                    out.push((v, n));
                }
            }
            _ => {}
        }
    }
    // an SDWA compare with SD = 1 writes its SDST instead of VCC (v_cmpx: EXEC only)
    if d.variant == Variant::Sdwa && d.enc.name == "ENC_VOPC" && !d.inst.name.starts_with("V_CMPX") {
        if let Some(e) = ext_encoding(d) {
            let x = |n: &str| rdna2::field_value(e, &d.words, n).unwrap_or(0);
            out.retain(|&(r, _)| r != 106);
            out.push(if x("SD") == 1 { (x("SDST"), 2) } else { (106, 2) });
        }
    }
    out
}

fn is_valu(d: &Decoded) -> bool {
    d.enc.name.starts_with("ENC_VOP") || d.enc.name.starts_with("VOP")
}

/// The fetch-shader call at the start of a VS (`s_swappc_b64 s[n:n+1], s[n:n+1]`), which the
/// emitter drops.
fn fetch_call(d: &Decoded) -> bool {
    let f = |n: &str| rdna2::field_value(d.enc, &d.words, n);
    d.inst.name == "S_SWAPPC_B64" && f("SDST") == f("SSRC0")
}

/// VGPRs an instruction writes: `rdna2::vgpr_uses`, with an image load's data registers
/// counted from its DMASK (the operand tables give four).
fn vgpr_writes(d: &Decoded) -> Vec<u64> {
    let (_, mut w) = rdna2::vgpr_uses(d);
    let n = d.inst.name;
    let dmask_counts = n.starts_with("IMAGE_SAMPLE") || n.starts_with("IMAGE_GATHER4") || n.starts_with("IMAGE_LOAD") || n == "IMAGE_GET_LOD";
    if d.enc.name.starts_with("ENC_MIMG") && dmask_counts && !w.is_empty() {
        let f = |n: &str| rdna2::field_value(d.enc, &d.words, n).unwrap_or(0);
        let mut n = if d.inst.name.starts_with("IMAGE_GATHER4") { 4 } else { f("DMASK").count_ones().max(1) as u64 };
        if f("D16") == 1 {
            n = n.div_ceil(2);
        }
        n += (f("TFE") | f("LWE")) & 1;
        w.truncate(n as usize);
    }
    w
}

/// A 32-bit compile-time constant from a VALU operand code (independent of lane).
fn const_u32(st: &State, d: &Decoded, code: u64) -> Option<u32> {
    match code {
        128..=192 => Some((code - 128) as u32),
        193..=208 => Some((-((code - 192) as i32)) as u32),
        240..=248 => f32_inline(code),
        255 => d.literal,
        0..=127 => st.src32(d, code).value(),
        _ => None,
    }
}

/// Evaluate a whole-wave reduction step writing `vdst` while EXEC is all ones.
fn eval_reduce(d: &Decoded, st: &State, vdst: u64) -> Option<VReduce> {
    let name = d.inst.name;
    let fv = |f: &str| rdna2::field_value(d.enc, &d.words, f).unwrap_or(0);
    if d.enc.name == "ENC_VOP3" && (fv("ABS") != 0 || fv("NEG") != 0 || fv("CLAMP") != 0 || fv("OMOD") != 0) {
        return None;
    }
    if matches!(name, "V_PERMLANE16_B32" | "V_PERMLANEX16_B32") {
        let src_v = fv("SRC0").checked_sub(256)?;
        let src = st.red_v.get(&src_v)?;
        let xor_row = usize::from(name == "V_PERMLANEX16_B32");
        let s1 = const_u32(st, d, fv("SRC1"));
        let s2 = const_u32(st, d, fv("SRC2"));
        let mut sets = [0u64; 64];
        for row in 0..4 {
            let srow = row ^ xor_row;
            let first = src.sets[srow * 16];
            if first != 0 && (0..16).all(|k| src.sets[srow * 16 + k] == first) {
                sets[row * 16..row * 16 + 16].fill(first);
            } else if let (Some(lo), Some(hi)) = (s1, s2) {
                for i in 0..16 {
                    let word = if i < 8 { lo } else { hi };
                    let n = ((word >> (4 * (i & 7))) & 15) as usize;
                    sets[row * 16 + i] = src.sets[srow * 16 + n];
                }
            }
        }
        return sets.iter().any(|&x| x != 0).then_some(VReduce { sets, ..*src });
    }
    if name == "V_MOV_B32" && d.variant == Variant::Plain {
        let src_v = fv("SRC0").checked_sub(256)?;
        return st.red_v.get(&src_v).cloned();
    }
    let op = ReduceOp::from_valu(name)?;
    let (src0, src1) = match d.enc.name {
        "ENC_VOP2" => (fv("SRC0"), 256 + fv("VSRC1")),
        "ENC_VOP3" => (fv("SRC0"), fv("SRC1")),
        _ => return None,
    };
    match d.variant {
        Variant::Sdwa => None,
        Variant::Dpp8 | Variant::Dpp16 => {
            let e = ext_encoding(d)?;
            let x = |n: &str| rdna2::field_value(e, &d.words, n).unwrap_or(0);
            let v0 = x("VSRC0");
            let v1 = src1.checked_sub(256)?;
            let seed = |v: u64| {
                (st.inact_const.get(&v) == Some(&op.identity())).then(|| VReduce {
                    seed_pc: d.pc,
                    src_vgpr: v,
                    op,
                    sets: std::array::from_fn(|l| 1u64 << l),
                })
            };
            let (r0, r1) = if v0 == v1 {
                let r = st.red_v.get(&v0).filter(|r| r.op == op).cloned().or_else(|| seed(v0))?;
                (r.clone(), r)
            } else {
                let a = st.red_v.get(&v0).filter(|r| r.op == op)?;
                let b = st.red_v.get(&v1).filter(|r| r.op == op && r.seed_pc == a.seed_pc)?;
                (a.clone(), b.clone())
            };
            let mut sets = [0u64; 64];
            if d.variant == Variant::Dpp8 {
                let sel: [usize; 8] = std::array::from_fn(|i| x(&format!("LANE_SEL_{i}")) as usize);
                for l in 0..64 {
                    let s = (l & !7) | sel[l & 7];
                    sets[l] = op.combine(r0.sets[s], r1.sets[l]);
                }
            } else {
                if x("ROW_MASK") != 0xF || x("BANK_MASK") != 0xF {
                    return None;
                }
                let ctrl = x("DPP_CTRL");
                let bound = x("BOUND_CTRL") == 1;
                for l in 0..64 {
                    sets[l] = match dpp16_lane(ctrl, l as u32)? {
                        Some(s) => op.combine(r0.sets[s as usize], r1.sets[l]),
                        None if !bound && vdst == v1 => r1.sets[l],
                        None if bound && op.identity() == 0 => r1.sets[l],
                        None => 0,
                    };
                }
            }
            sets.iter().any(|&x| x != 0).then_some(VReduce { sets, ..r0 })
        }
        Variant::Plain => {
            let mut sets = [0u64; 64];
            let (seed_pc, src_vgpr) = match (src0.checked_sub(256), src1.checked_sub(256)) {
                (Some(v0), Some(v1)) => {
                    let a = st.red_v.get(&v0).filter(|r| r.op == op)?;
                    let b = st.red_v.get(&v1).filter(|r| r.op == op && r.seed_pc == a.seed_pc)?;
                    for l in 0..64 {
                        sets[l] = op.combine(a.sets[l], b.sets[l]);
                    }
                    (a.seed_pc, a.src_vgpr)
                }
                (None, Some(v1)) => {
                    let a = st.red_s.get(&src0).filter(|r| r.op == op)?;
                    let b = st.red_v.get(&v1).filter(|r| r.op == op && r.seed_pc == a.seed_pc)?;
                    for l in 0..64 {
                        sets[l] = op.combine(a.set, b.sets[l]);
                    }
                    (a.seed_pc, a.src_vgpr)
                }
                (Some(v0), None) => {
                    let a = st.red_v.get(&v0).filter(|r| r.op == op)?;
                    let b = st.red_s.get(&src1).filter(|r| r.op == op && r.seed_pc == a.seed_pc)?;
                    for l in 0..64 {
                        sets[l] = op.combine(a.sets[l], b.set);
                    }
                    (a.seed_pc, a.src_vgpr)
                }
                (None, None) => return None,
            };
            sets.iter().any(|&x| x != 0).then_some(VReduce { seed_pc, src_vgpr, op, sets })
        }
    }
}

/// Constant written to every inactive (unlaunched + helper) lane of a VGPR in whole-wave mode.
fn eval_inact_const(d: &Decoded, st: &State) -> Option<u32> {
    if d.variant != Variant::Plain {
        return None;
    }
    let fv = |f: &str| rdna2::field_value(d.enc, &d.words, f).unwrap_or(0);
    if d.enc.name == "ENC_VOP3" && (fv("ABS") != 0 || fv("NEG") != 0 || fv("CLAMP") != 0 || fv("OMOD") != 0) {
        return None;
    }
    match d.inst.name {
        "V_MOV_B32" => const_u32(st, d, fv("SRC0")),
        "V_CNDMASK_B32" => {
            let (mask_inact_lo, mask_hi) = if d.enc.name == "ENC_VOP3" {
                let c = fv("SRC2");
                (st.src_inact_lo(d, c), st.src64(d, c).1)
            } else {
                (st.inact_lo[106], st.s[107])
            };
            (mask_inact_lo == Word::Zero && mask_hi == Word::Zero).then(|| const_u32(st, d, fv("SRC0"))).flatten()
        }
        _ => None,
    }
}

/// One instruction's effect.
fn step(d: &Decoded, st: &mut State) {
    let name = d.inst.name;
    let fv = |f: &str| rdna2::field_value(d.enc, &d.words, f).unwrap_or(0);
    let exec_lo = st.s[126];
    let exec_hi = st.s[127];
    let exec_inact_lo = st.inact_lo[126];
    if name.starts_with("S_MOVRELD") {
        // writes s[dst + m0]: any SGPR
        st.s = [Word::Any; 128];
        st.s[125] = Word::Zero;
        st.s[127] = exec_hi;
        st.inact_lo = [Word::Any; 128];
        st.inact_lo[125] = Word::Zero;
        st.inact_lo[126] = exec_inact_lo;
        st.red_s.clear();
        return;
    }
    // VGPR lanes
    if name == "V_WRITELANE_B32" {
        let v = fv("VDST");
        st.inact_const.remove(&v);
        st.red_v.remove(&v);
        match lane_operand(d) {
            Lane::Const(l) if l >= 32 => {
                let mut u = st.upper(v);
                u[l as usize - 32] = Hi::Shadow;
                st.set_upper(v, u);
            }
            Lane::Const(l) => *st.lo.entry(v).or_default() |= 1 << l,
            Lane::Sgpr => {
                let u = st.upper(v).map(|x| x.join(Hi::Unknown));
                st.set_upper(v, u);
                st.lo.remove(&v);
            }
        }
    } else if name.starts_with("V_MOVREL") || name.starts_with("V_SWAPREL") {
        // writes v[dst + m0] (v_swaprel also v[src0 + m0]): any VGPR
        st.lo.clear();
        st.inact_const.clear();
        st.red_v.clear();
        if exec_hi != Word::Zero {
            let all: Vec<u64> = (0..256).collect();
            for v in all {
                let u = st.upper(v).map(|x| x.join(Hi::Unknown));
                st.set_upper(v, u);
            }
        }
    } else {
        let writes = vgpr_writes(d);
        let single = writes.len() == 1 && is_valu(d);
        // with only some lanes ≥ 32 active, DPP and v_permlane sources may be inactive: not followed
        let cross = d.variant != Variant::Plain || name.starts_with("V_PERMLANE");
        let wwm = exec_lo == Word::Ones && exec_hi == Word::Ones;
        let mut new = Vec::new();
        let mut new_inact = Vec::new();
        let mut new_red = Vec::new();
        for &v in &writes {
            st.lo.remove(&v);
            let red = (single && wwm).then(|| eval_reduce(d, st, v)).flatten();
            new_red.push((v, red));
            if !(exec_inact_lo == Word::Zero && exec_hi == Word::Zero) {
                let ic = (single && wwm).then(|| eval_inact_const(d, st)).flatten();
                new_inact.push((v, ic));
            }
            let lanes = match exec_hi {
                Word::Zero => continue,
                Word::Ones => match single.then(|| evaluate(d, st, v)).flatten() {
                    Some(x) => x,
                    None => [Hi::Unknown; 32],
                },
                Word::Any => {
                    let n = (single && !cross).then(|| evaluate(d, st, v)).flatten().unwrap_or([Hi::Unknown; 32]);
                    let mut o = st.upper(v);
                    for (a, b) in o.iter_mut().zip(n) {
                        *a = a.join(b);
                    }
                    o
                }
            };
            new.push((v, lanes));
        }
        for (v, lanes) in new {
            st.set_upper(v, lanes);
        }
        for (v, ic) in new_inact {
            match ic {
                Some(c) => {
                    st.inact_const.insert(v, c);
                }
                None => {
                    st.inact_const.remove(&v);
                }
            }
        }
        for (v, r) in new_red {
            match r {
                Some(vr) => {
                    st.red_v.insert(v, vr);
                }
                None => {
                    st.red_v.remove(&v);
                }
            }
        }
    }
    // SGPRs
    let op = name.split('_').nth(1).unwrap_or("");
    let b64 = name.ends_with("_B64");
    if name.contains("SAVEEXEC") || name.contains("WREXEC") {
        let s0 = st.src64(d, fv("SSRC0"));
        let s0_inact = st.src_inact_lo(d, fv("SSRC0"));
        let (el, eh) = (st.s[126], st.s[127]);
        let nl = bitop(op, s0.0, el).unwrap_or(Word::Any);
        let nh = bitop(op, s0.1, eh).unwrap_or(Word::Any);
        let nl_inact = bitop(op, s0_inact, exec_inact_lo).unwrap_or(Word::Any);
        let dst = fv("SDST");
        let wr = name.contains("WREXEC");
        st.red_s.remove(&dst);
        if b64 {
            st.red_s.remove(&(dst + 1));
            st.s[126] = nl;
            st.s[127] = nh;
            st.inact_lo[126] = nl_inact;
            st.set(dst, if wr { nl } else { el });
            st.set(dst + 1, if wr { nh } else { eh });
            st.set_inact_lo(dst, if wr { nl_inact } else { exec_inact_lo });
        } else {
            st.s[126] = nl;
            st.inact_lo[126] = nl_inact;
            st.set(dst, if wr { nl } else { el });
            st.set_inact_lo(dst, if wr { nl_inact } else { exec_inact_lo });
        }
        return;
    }
    if d.enc.name == "ENC_SOP1" || d.enc.name == "ENC_SOP2" {
        let dst = fv("SDST");
        let a = |st: &State| if b64 { st.src64(d, fv("SSRC0")) } else { (st.src32(d, fv("SSRC0")), Word::Zero) };
        let b = |st: &State| if b64 { st.src64(d, fv("SSRC1")) } else { (st.src32(d, fv("SSRC1")), Word::Zero) };
        let a_inact = st.src_inact_lo(d, fv("SSRC0"));
        let b_inact = st.src_inact_lo(d, fv("SSRC1"));
        let r: Option<(Word, Word, Word)> = match op {
            "MOV" if d.enc.name == "ENC_SOP1" => {
                let x = a(st);
                Some((x.0, x.1, a_inact))
            }
            "NOT" if d.enc.name == "ENC_SOP1" => {
                let x = a(st);
                Some((x.0.not(), x.1.not(), a_inact.not()))
            }
            // a word of 0 / all ones stays 0 / all ones in upper lanes; helper lanes in launched
            // quads become active, so `inact_lo` becomes Any
            "WQM" if d.enc.name == "ENC_SOP1" => {
                let x = a(st);
                Some((x.0, x.1, Word::Any))
            }
            "CMOV" if d.enc.name == "ENC_SOP1" => {
                let x = a(st);
                let o = (st.s[dst.min(127) as usize], st.s[(dst + 1).min(127) as usize]);
                let oi = st.inact_lo[dst.min(127) as usize];
                Some((x.0.join(o.0), x.1.join(o.1), a_inact.join(oi)))
            }
            "CSELECT" => {
                let (x, y) = (a(st), b(st));
                Some((x.0.join(y.0), x.1.join(y.1), a_inact.join(b_inact)))
            }
            _ if d.enc.name == "ENC_SOP2" => {
                let (x, y) = (a(st), b(st));
                let li = bitop(op, a_inact, b_inact).unwrap_or(Word::Any);
                bitop(op, x.0, y.0).zip(bitop(op, x.1, y.1)).map(|(lo, hi)| (lo, hi, li))
            }
            _ => None,
        };
        if let Some((lo, hi, li)) = r {
            st.set(dst, lo);
            st.set_inact_lo(dst, li);
            st.red_s.remove(&dst);
            if b64 {
                st.set(dst + 1, hi);
                st.set_inact_lo(dst + 1, Word::Any);
                st.red_s.remove(&(dst + 1));
            }
            return;
        }
    }
    if name == "V_READLANE_B32" {
        let dst = fv("VDST");
        st.set(dst, Word::Any);
        st.set_inact_lo(dst, Word::Any);
        let sr = fv("SRC0")
            .checked_sub(256)
            .and_then(|v| st.red_v.get(&v))
            .and_then(|vr| match lane_operand(d) {
                Lane::Const(l) => {
                    let set = vr.sets[l as usize];
                    (set != 0).then_some(SReduce { seed_pc: vr.seed_pc, src_vgpr: vr.src_vgpr, op: vr.op, set })
                }
                Lane::Sgpr => None,
            });
        match sr {
            Some(x) => {
                st.red_s.insert(dst, x);
            }
            None => {
                st.red_s.remove(&dst);
            }
        }
        return;
    }
    let valu = is_valu(d);
    for (r, n) in sgpr_outputs(d) {
        if valu && n == 2 {
            // a lane mask: bits of inactive lanes are 0
            st.set(r, Word::Any);
            st.set_inact_lo(r, if exec_inact_lo == Word::Zero { Word::Zero } else { Word::Any });
            st.set(r + 1, if exec_hi == Word::Zero { Word::Zero } else { Word::Any });
            st.set_inact_lo(r + 1, Word::Any);
            st.red_s.remove(&r);
            st.red_s.remove(&(r + 1));
        } else {
            for k in r..r + n {
                st.set(k, Word::Any);
                st.set_inact_lo(k, Word::Any);
                st.red_s.remove(&k);
            }
        }
    }
}

/// The resolution of a cross-lane read in state `st` (before the instruction), if it is one.
fn classify(d: &Decoded, st: &State) -> Option<(Read, Option<(u64, u32)>)> {
    let fv = |f: &str| rdna2::field_value(d.enc, &d.words, f).unwrap_or(0);
    // all lanes ≥ 32 together (a lane chosen per lane or at run time)
    let any_upper = |v: u64| -> Read {
        match st.upper(v).iter().fold(Hi::Unwritten, |m, &x| m.join(x)) {
            Hi::Unwritten => Read::Native,
            Hi::Const(c) => Read::Const(c),
            _ => Read::Unknown,
        }
    };
    match d.inst.name {
        "V_READLANE_B32" => {
            let v = fv("SRC0").checked_sub(256)?;
            match lane_operand(d) {
                Lane::Const(l) if l >= 32 => Some(match st.upper(v)[l as usize - 32] {
                    Hi::Unwritten => (Read::Native, None),
                    Hi::Const(c) => (Read::Const(c), None),
                    Hi::Shadow => (Read::Shadow, Some((v, l))),
                    Hi::Unknown => (Read::Unknown, None),
                }),
                Lane::Const(l) => (st.lo.get(&v).copied().unwrap_or(0) >> l & 1 == 1).then_some((Read::Shadow, Some((v, l)))),
                Lane::Sgpr => Some((any_upper(v), None)),
            }
        }
        "DS_BPERMUTE_B32" => {
            // source lanes ≥ 32 inactive: 0, as `Em::ds` gives for them
            Some((if st.s[127] == Word::Zero { Read::Native } else { any_upper(fv("DATA0")) }, None))
        }
        "DS_PERMUTE_B32" => Some((Read::Unknown, None)),
        _ => None,
    }
}

/// Successors of each instruction (index); None: unknown (an indirect jump).
fn successors(insts: &[Decoded], pcs: &BTreeMap<u64, usize>) -> Vec<Option<Vec<usize>>> {
    insts
        .iter()
        .enumerate()
        .map(|(i, d)| {
            let next = insts.get(i + 1).map(|n| n.pc).and_then(|pc| pcs.get(&pc).copied());
            if let Some(t) = d.far_target {
                return Some(vec![*pcs.get(&t)?]);
            }
            if fetch_call(d) {
                return Some(next.into_iter().collect());
            }
            if d.inst.flags & F_INDIRECT != 0 || d.inst.name == "S_SETPC_B64" || d.inst.name == "S_SWAPPC_B64" {
                return None;
            }
            let mut out = Vec::new();
            if let Some(t) = rdna2::branch_target(d) {
                out.push(*pcs.get(&t)?);
                if d.inst.flags & F_CONDITIONAL != 0 {
                    out.extend(next);
                }
            } else if d.inst.flags & F_TERMINATOR == 0 {
                out.extend(next);
            }
            Some(out)
        })
        .collect()
}

#[cfg(test)]
thread_local! {
    /// Some: `analyze` notes where EXEC's upper word changes and VGPR lanes ≥ 32 become
    /// unknown (`tests::retail_wave64_graphics_lane_census`)
    pub static TRACE: std::cell::RefCell<Option<Vec<String>>> = const { std::cell::RefCell::new(None) };
}

/// The analysis of a whole program (instructions in address order, far jumps resolved).
pub fn analyze(insts: &[Decoded]) -> UpperLanes {
    let mut out = UpperLanes::default();
    if insts.is_empty() {
        return out;
    }
    let pcs: BTreeMap<u64, usize> = insts.iter().enumerate().map(|(i, d)| (d.pc, i)).collect();
    // (a loop that runs out of its iteration budget, `LOOP_GUARD`, leaves from wherever it is:
    // not followed – the program's results are wrong by then anyway)
    let succ = successors(insts, &pcs);
    let mut at: Vec<Option<State>> = vec![None; insts.len()];
    at[0] = Some(State::entry());
    let mut work = BTreeSet::from([0usize]);
    while let Some(i) = work.pop_first() {
        let mut st = at[i].clone().expect("queued with a state");
        #[cfg(test)]
        let before = st.clone();
        step(&insts[i], &mut st);
        #[cfg(test)]
        TRACE.with(|t| {
            if let Some(t) = t.borrow_mut().as_mut() {
                let d = &insts[i];
                if before.s[127] != st.s[127] {
                    t.push(format!("{:05x}: {} -> exec_hi {:?}", d.pc, rdna2::format(d), st.s[127]));
                }
                for (v, l) in &st.hi {
                    if before.hi.get(v) != Some(l) && l.contains(&Hi::Unknown) {
                        t.push(format!("{:05x}: {} -> v{v} upper {:?}", d.pc, rdna2::format(d), l[0]));
                    }
                }
            }
        });
        let Some(s) = &succ[i] else {
            // an indirect jump (the emitter does not translate one either): nothing is
            // known, every read that may reach a lane ≥ 32 stays a hazard
            for d in insts.iter().filter(|d| reaches_upper(d)) {
                out.reads.insert(d.pc, Read::Unknown);
            }
            return out;
        };
        for &j in s {
            let new = match &at[j] {
                None => st.clone(),
                Some(old) => old.join(&st),
            };
            if at[j].as_ref() != Some(&new) {
                at[j] = Some(new);
                work.insert(j);
            }
        }
    }
    for (d, st) in insts.iter().zip(&at) {
        let Some(st) = st else {
            // unreachable: never runs
            continue;
        };
        if let Some((r, slot)) = classify(d, st) {
            if let (Read::Shadow, Some(s)) = (r, slot) {
                out.shadows.insert(s);
            }
            out.reads.insert(d.pc, r);
        }
        let fv = |f: &str| rdna2::field_value(d.enc, &d.words, f).unwrap_or(0);
        let full = match d.inst.name {
            "V_READLANE_B32" => fv("SRC0")
                .checked_sub(256)
                .and_then(|v| st.red_v.get(&v))
                .filter(|vr| matches!(lane_operand(d), Lane::Const(l) if vr.sets[l as usize] == u64::MAX)),
            "V_READFIRSTLANE_B32" if st.s[126] == Word::Ones && st.s[127] == Word::Ones => fv("SRC0")
                .checked_sub(256)
                .and_then(|v| st.red_v.get(&v))
                .filter(|vr| vr.sets[0] == u64::MAX),
            _ => None,
        };
        if let Some(vr) = full {
            out.reduction_seeds.insert(vr.seed_pc, (vr.src_vgpr, vr.op));
            out.reduction_results.insert(d.pc, vr.seed_pc);
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::gpu::rdna2::encode;

    const V: u64 = 256;
    type I = (&'static str, &'static str, &'static [(&'static str, u64)], Option<u32>);

    fn program(lines: &[I]) -> Vec<Decoded> {
        let mut bytes: Vec<u8> = lines.iter().flat_map(|(n, e, f, l)| encode(n, e, f, *l).unwrap_or_else(|x| panic!("{n}: {x}"))).collect();
        bytes.extend(encode("S_ENDPGM", "ENC_SOPP", &[], None).unwrap());
        let mut out = Vec::new();
        let mut off = 0usize;
        while off < bytes.len() {
            let d = rdna2::decode(&bytes[off..], off as u64).unwrap();
            off += d.len;
            out.push(d);
        }
        rdna2::resolve_far_jumps(&mut out);
        out
    }

    fn reads(lines: &[I]) -> Vec<Read> {
        let p = program(lines);
        let a = analyze(&p);
        p.iter().filter_map(|d| a.reads.get(&d.pc).copied()).collect()
    }

    // the reduction of the retail pixel shaders: identity into the lanes outside the mask,
    // min over 8 lanes (DPP8 xor 1, 2, 4), row_mirror, the other row (v_permlanex16), then
    // v_readlane 32 for the upper half's result
    const WWM: I = ("S_OR_SAVEEXEC_B64", "ENC_SOP1", &[("SDST", 30), ("SSRC0", 193)], None);
    const MASK: I = ("S_AND_B64", "ENC_SOP2", &[("SDST", 10), ("SSRC0", 18), ("SSRC1", 30)], None);
    const SEED: I = ("V_CNDMASK_B32", "ENC_VOP3", &[("VDST", 6), ("SRC0", 193), ("SRC1", V + 55), ("SRC2", 10)], None);
    const X1: I = ("V_MIN_U32", "VOP2_VOP_DPP8", &[("VDST", 6), ("SRC0", 0xE9), ("VSRC0", 6), ("VSRC1", 6), ("LANE_SEL_0", 1), ("LANE_SEL_1", 0), ("LANE_SEL_2", 3), ("LANE_SEL_3", 2), ("LANE_SEL_4", 5), ("LANE_SEL_5", 4), ("LANE_SEL_6", 7), ("LANE_SEL_7", 6)], None);
    const MIRROR: I = ("V_MIN_U32", "VOP2_VOP_DPP16", &[("VDST", 6), ("SRC0", 0xFA), ("VSRC0", 6), ("VSRC1", 6), ("DPP_CTRL", 0x140), ("ROW_MASK", 0xF), ("BANK_MASK", 0xF)], None);
    const PX16: I = ("V_PERMLANEX16_B32", "ENC_VOP3", &[("VDST", 7), ("SRC0", V + 6), ("SRC1", 128), ("SRC2", 128)], None);
    const COMBINE: I = ("V_MIN_U32", "ENC_VOP2", &[("VDST", 6), ("SRC0", V + 7), ("VSRC1", 6)], None);
    const READ32: I = ("V_READLANE_B32", "ENC_VOP3", &[("VDST", 10), ("SRC0", V + 6), ("SRC1", 128 + 32)], None);
    const RESTORE: I = ("S_MOV_B64", "ENC_SOP1", &[("SDST", 126), ("SSRC0", 30)], None);

    #[test]
    fn a_whole_wave_reduction_reads_its_identity() {
        assert_eq!(reads(&[WWM, MASK, SEED, X1, MIRROR, PX16, COMBINE, READ32, RESTORE]), vec![Read::Const(u32::MAX)]);
    }

    #[test]
    fn the_identity_is_only_known_while_the_mask_leaves_the_upper_lanes_out() {
        // the mask from s[18:19] alone: its upper word may select v55's lanes
        const MASK_ANY: I = ("S_MOV_B64", "ENC_SOP1", &[("SDST", 10), ("SSRC0", 18)], None);
        assert_eq!(reads(&[WWM, MASK_ANY, SEED, X1, READ32, RESTORE]), vec![Read::Const(u32::MAX)], "v55 unwritten: either is right");
        // v55's upper lanes written (with something unknown) before: not followed
        const V55: I = ("V_MOV_B32", "ENC_VOP1", &[("VDST", 55), ("SRC0", 20)], None);
        assert_eq!(reads(&[WWM, V55, MASK_ANY, SEED, X1, READ32, RESTORE]), vec![Read::Unknown]);
        // with the saved EXEC in the mask, v55 does not matter
        assert_eq!(reads(&[WWM, V55, MASK, SEED, X1, READ32, RESTORE]), vec![Read::Const(u32::MAX)]);
    }

    #[test]
    fn a_dpp_shift_with_bound_ctrl_breaks_the_constant() {
        // row_shr:1 with bound_ctrl: the first lane of each row takes 0 – min(0, -1) = 0 there
        const SHR: I = ("V_MIN_U32", "VOP2_VOP_DPP16", &[("VDST", 6), ("SRC0", 0xFA), ("VSRC0", 6), ("VSRC1", 6), ("DPP_CTRL", 0x111), ("ROW_MASK", 0xF), ("BANK_MASK", 0xF), ("BOUND_CTRL", 1)], None);
        const READ48: I = ("V_READLANE_B32", "ENC_VOP3", &[("VDST", 10), ("SRC0", V + 6), ("SRC1", 128 + 48)], None);
        const READ49: I = ("V_READLANE_B32", "ENC_VOP3", &[("VDST", 10), ("SRC0", V + 6), ("SRC1", 128 + 49)], None);
        assert_eq!(reads(&[WWM, MASK, SEED, SHR, READ48, READ49, RESTORE]), vec![Read::Const(0), Read::Const(u32::MAX)]);
        // the permlane of lanes that differ is not followed
        assert_eq!(reads(&[WWM, MASK, SEED, SHR, PX16, ("V_READLANE_B32", "ENC_VOP3", &[("VDST", 10), ("SRC0", V + 7), ("SRC1", 128 + 32)], None), RESTORE]), vec![Read::Unknown]);
    }

    #[test]
    fn outside_whole_wave_code_the_upper_lanes_stay_unwritten() {
        // a waterfall loop's read: s_ff1 of the active lanes picks the lane
        let r = reads(&[
            ("V_MOV_B32", "ENC_VOP1", &[("VDST", 51), ("SRC0", 20)], None),
            ("S_AND_B64", "ENC_SOP2", &[("SDST", 0), ("SSRC0", 72), ("SSRC1", 126)], None),
            ("S_FF1_I32_B64", "ENC_SOP1", &[("SDST", 0), ("SSRC0", 0)], None),
            ("V_READLANE_B32", "ENC_VOP3", &[("VDST", 4), ("SRC0", V + 51), ("SRC1", 0)], None),
            ("DS_BPERMUTE_B32", "ENC_DS", &[("VDST", 1), ("ADDR", 2), ("DATA0", 51)], None),
        ]);
        assert_eq!(r, vec![Read::Native, Read::Native]);
        // after s_mov_b64 exec, -1 (no saved copy) the same move writes the upper lanes
        let r = reads(&[
            ("S_MOV_B64", "ENC_SOP1", &[("SDST", 126), ("SSRC0", 193)], None),
            ("V_MOV_B32", "ENC_VOP1", &[("VDST", 51), ("SRC0", 20)], None),
            ("V_READLANE_B32", "ENC_VOP3", &[("VDST", 4), ("SRC0", V + 51), ("SRC1", 0)], None),
            ("DS_BPERMUTE_B32", "ENC_DS", &[("VDST", 1), ("ADDR", 2), ("DATA0", 51)], None),
        ]);
        assert_eq!(r, vec![Read::Unknown, Read::Unknown]);
    }

    #[test]
    fn spill_slots_come_from_their_shadow() {
        let wl = |lane: u64, s: u64| -> I {
            let f: &'static [(&'static str, u64)] = Box::leak(vec![("VDST", 96), ("SRC0", s), ("SRC1", 128 + lane)].into_boxed_slice());
            ("V_WRITELANE_B32", "ENC_VOP3", f, None)
        };
        let rl = |lane: u64| -> I {
            let f: &'static [(&'static str, u64)] = Box::leak(vec![("VDST", 20), ("SRC0", V + 96), ("SRC1", 128 + lane)].into_boxed_slice());
            ("V_READLANE_B32", "ENC_VOP3", f, None)
        };
        let p = program(&[wl(5, 20), wl(36, 21), rl(5), rl(36), rl(37), ("V_MOV_B32", "ENC_VOP1", &[("VDST", 96), ("SRC0", 20)], None), rl(5), rl(36)]);
        let a = analyze(&p);
        let r: Vec<Read> = p.iter().filter_map(|d| a.reads.get(&d.pc).copied()).collect();
        // lane 37 was never written: anything; after the move lane 5 is the move's (a
        // native read, not listed), lane 36 still the spill
        assert_eq!(r, vec![Read::Shadow, Read::Shadow, Read::Native, Read::Shadow]);
        assert_eq!(a.shadows, BTreeSet::from([(96, 5), (96, 36)]));
    }

    #[test]
    fn swaprel_writes_any_register() {
        // v_swaprel_b32 writes v[src0 + m0] and v[dst + m0]: a spill slot is not kept past it
        let r = reads(&[
            ("V_WRITELANE_B32", "ENC_VOP3", &[("VDST", 10), ("SRC0", 20), ("SRC1", 128 + 5)], None),
            ("S_MOV_B32", "ENC_SOP1", &[("SDST", 124), ("SSRC0", 128 + 2)], None),
            ("V_SWAPREL_B32", "ENC_VOP1", &[("VDST", 3), ("SRC0", V + 8)], None),
            ("V_READLANE_B32", "ENC_VOP3", &[("VDST", 21), ("SRC0", V + 10), ("SRC1", 128 + 5)], None),
        ]);
        assert!(!r.contains(&Read::Shadow), "{r:?}");
    }

    #[test]
    fn sdwa_cmpx_writes_exec_not_vcc() {
        // VCC keeps its old (all ones) value: lane 32 of the select takes v7
        let r = reads(&[
            ("S_MOV_B64", "ENC_SOP1", &[("SDST", 106), ("SSRC0", 193)], None),
            ("V_CMPX_EQ_U32", "VOPC_VOP_SDWA_SDST_ENC", &[("SRC0", 0xF9), ("VSRC0", 1), ("VSRC1", 2), ("SRC0_SEL", 6), ("SRC1_SEL", 6)], None),
            WWM,
            ("V_MOV_B32", "ENC_VOP1", &[("VDST", 7), ("SRC0", 128 + 2)], None),
            ("V_CNDMASK_B32", "ENC_VOP2", &[("VDST", 6), ("SRC0", 128 + 1), ("VSRC1", 7)], None),
            RESTORE,
            ("V_READLANE_B32", "ENC_VOP3", &[("VDST", 10), ("SRC0", V + 6), ("SRC1", 128 + 32)], None),
        ]);
        assert!(!r.contains(&Read::Const(1)), "{r:?}");
    }

    #[test]
    fn a_constant_decides_an_unwritten_operand() {
        // v5's lanes 32..63 were never written; 0 & anything is 0
        let and = reads(&[WWM, ("V_AND_B32", "ENC_VOP2", &[("VDST", 6), ("SRC0", 128), ("VSRC1", 5)], None), RESTORE, READ32]);
        assert_eq!(and, vec![Read::Const(0)]);
        // v5 ^ v5 is 0 whatever v5 holds: not "unwritten"
        let xor = reads(&[WWM, ("V_XOR_B32", "ENC_VOP2", &[("VDST", 6), ("SRC0", V + 5), ("VSRC1", 5)], None), RESTORE, READ32]);
        assert_eq!(xor, vec![Read::Unknown]);
    }

    #[test]
    fn paths_join() {
        // one path writes the spill slot, the other a whole-wave constant: not followed
        let r = reads(&[
            ("S_CBRANCH_SCC0", "ENC_SOPP", &[("SIMM16", 3)], None),
            ("V_WRITELANE_B32", "ENC_VOP3", &[("VDST", 6), ("SRC0", 20), ("SRC1", 128 + 32)], None),
            ("S_BRANCH", "ENC_SOPP", &[("SIMM16", 5)], None),
            WWM,
            MASK,
            SEED,
            RESTORE,
            READ32,
        ]);
        assert_eq!(r, vec![Read::Unknown]);
        // both paths the same constant (or nothing): the constant
        let r = reads(&[("S_CBRANCH_SCC0", "ENC_SOPP", &[("SIMM16", 4)], None), WWM, MASK, SEED, RESTORE, READ32]);
        assert_eq!(r, vec![Read::Const(u32::MAX)]);
    }

    #[test]
    fn whole_wave_reduction_is_recognised_for_all_ops() {
        const X2: I = ("V_MIN_U32", "VOP2_VOP_DPP8", &[("VDST", 6), ("SRC0", 0xE9), ("VSRC0", 6), ("VSRC1", 6), ("LANE_SEL_0", 2), ("LANE_SEL_1", 3), ("LANE_SEL_2", 0), ("LANE_SEL_3", 1), ("LANE_SEL_4", 6), ("LANE_SEL_5", 7), ("LANE_SEL_6", 4), ("LANE_SEL_7", 5)], None);
        const X4: I = ("V_MIN_U32", "VOP2_VOP_DPP8", &[("VDST", 6), ("SRC0", 0xE9), ("VSRC0", 6), ("VSRC1", 6), ("LANE_SEL_0", 4), ("LANE_SEL_1", 5), ("LANE_SEL_2", 6), ("LANE_SEL_3", 7), ("LANE_SEL_4", 0), ("LANE_SEL_5", 1), ("LANE_SEL_6", 2), ("LANE_SEL_7", 3)], None);
        const FINAL_COMBINE: I = ("V_MIN_U32", "ENC_VOP2", &[("VDST", 6), ("SRC0", 10), ("VSRC1", 6)], None);
        const READ0: I = ("V_READLANE_B32", "ENC_VOP3", &[("VDST", 40), ("SRC0", V + 6), ("SRC1", 128)], None);
        let p = program(&[WWM, MASK, SEED, X1, X2, X4, MIRROR, PX16, COMBINE, READ32, FINAL_COMBINE, READ0, RESTORE]);
        let a = analyze(&p);
        let seed_pc = p[3].pc; // X1
        let read0_pc = p[11].pc; // READ0
        assert_eq!(a.reduction_seeds.get(&seed_pc), Some(&(6, ReduceOp::MinU32)));
        assert_eq!(a.reduction_results.get(&read0_pc), Some(&seed_pc));

        // wrong identity (0 for V_MIN_U32): must NOT be recognised as a whole-wave reduction
        const BAD_SEED: I = ("V_CNDMASK_B32", "ENC_VOP3", &[("VDST", 6), ("SRC0", 128), ("SRC1", V + 55), ("SRC2", 10)], None);
        let p_bad = program(&[WWM, MASK, BAD_SEED, X1, X2, X4, MIRROR, PX16, COMBINE, READ32, FINAL_COMBINE, READ0, RESTORE]);
        assert!(analyze(&p_bad).reduction_results.is_empty());

        // S_WQM-polluted mask: helper lanes could be 1, so WaveActive* would skip them
        const WQM_EXEC: I = ("S_WQM_B64", "ENC_SOP1", &[("SDST", 126), ("SSRC0", 126)], None);
        let p_wqm = program(&[WQM_EXEC, WWM, MASK, SEED, X1, X2, X4, MIRROR, PX16, COMBINE, READ32, FINAL_COMBINE, READ0, RESTORE]);
        assert!(analyze(&p_wqm).reduction_results.is_empty());
    }
}
