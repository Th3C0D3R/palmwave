//! Tests of the BCPack decoder on synthetic streams: the fixtures of `tests/data/bcpack`
//! (built by its `generate.py`, an encoder written from the format notes and not from this
//! decoder) and streams of the encoder below. None of them shows what a console would write.

use super::lanes::{self, L};
use super::rans_tables::FREQ;
use super::{decode_lanes, lane_widths, Error};

pub(crate) fn data(name: &str) -> Option<Vec<u8>> {
    std::fs::read(std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../../tests/data/bcpack").join(name)).ok()
}

/// `bc<mode>-<units>-<seed>` → (mode, units)
fn fixture(stem: &str) -> (u8, usize) {
    let mut p = stem.strip_prefix("bc").unwrap().split('-');
    (p.next().unwrap().parse().unwrap(), p.next().unwrap().parse().unwrap())
}

/// A `.lanes` file (all of lane 0, then lane 1 …) in the decoder's order.
fn by_unit(lane_major: &[u8], lanes: usize) -> Vec<u8> {
    let units = lane_major.len() / lanes;
    (0..units * lanes).map(|i| lane_major[(i % lanes) * units + i / lanes]).collect()
}

const FIXTURES: [&str; 14] = [
    "bc1-1-8", "bc1-2-0", "bc1-16-1", "bc1-16-9", "bc1-700-2", "bc1-8192-3", "bc3-16-1", "bc4-16-1", "bc5-16-1", "bc6-16-1", "bc7-2-0", "bc7-16-1",
    "bc7-700-2", "bc7-8192-3",
];

/// The symbols of the two 64 KiB fixtures, which ship without a `.lanes` file (`generate.py`).
pub(crate) fn large_fixture_symbols(mode: u8) -> Vec<u8> {
    let n = lane_widths(mode).unwrap().len();
    let mut s = vec![0u8; 8192 * n];
    if mode == 1 {
        for i in (0..8192).step_by(511) {
            s[i * n + 2] = 30;
            s[i * n + 8] = 5;
        }
        for i in (31..8192).step_by(31) {
            s[i * n] = 2;
        }
    }
    s
}

#[test]
fn fixtures_decode_to_their_lanes() {
    for stem in FIXTURES {
        let (mode, units) = fixture(stem);
        let n = lane_widths(mode).unwrap().len();
        let bin = data(&format!("{stem}.bin")).unwrap();
        let want = match data(&format!("{stem}.lanes")) {
            Some(l) => by_unit(&l, n),
            None => large_fixture_symbols(mode),
        };
        assert_eq!(want.len(), units * n, "{stem}");
        // bc1-1-8 has states cut to fewer bytes than an encoder that starts at `L` can drop,
        // which the hardware's decoder does not take either
        if stem == "bc1-1-8" {
            assert!(decode_lanes(mode, &bin, units * 8).is_err(), "{stem}");
            continue;
        }
        assert_eq!(decode_lanes(mode, &bin, units * 8).as_deref(), Ok(&want[..]), "{stem}");
        // zero padding after the stream is not part of it; anything else is
        let mut padded = bin.clone();
        padded.extend([0; 37]);
        assert_eq!(decode_lanes(mode, &padded, units * 8).as_deref(), Ok(&want[..]), "{stem} padded");
        padded.push(1);
        assert_eq!(decode_lanes(mode, &padded, units * 8), Err(Error::Trailing { consumed: bin.len() }), "{stem}");
    }
}

/// The 8-bit model 11 as US 11,234,023 prints it (its `base_table[8][12]`, counted from 1).
#[test]
fn model_matches_the_patent_excerpt() {
    let base = [0u32, 7575, 14276, 25440, 41008, 56352, 64256, 65344, 65408];
    let freq = [7575u16, 6701, 5582, 3892, 1918, 494, 34, 1, 1];
    assert_eq!(FREQ[7][11], freq);
    let mut at = 0;
    for k in 0..9 {
        assert_eq!(at, base[k]);
        at += freq[k] as u32 * if k < 2 { 1 } else { 1 << (k - 1) };
    }
    assert_eq!(at, 65536);
    for (bits, per_q) in FREQ.iter().enumerate() {
        for f in per_q {
            let total: u32 = (0..9).map(|k| f[k] as u32 * if k < 2 { 1 } else { 1 << (k - 1) }).sum();
            assert_eq!(total, 65536);
            assert!(f[..bits + 2].iter().all(|&v| v >= 1) && f[bits + 2..].iter().all(|&v| v == 0));
        }
    }
}

// ---------------------------------------------------------------------------------------
// a test encoder: any model, any fragment sizes
// ---------------------------------------------------------------------------------------

struct Rng(u64);

impl Rng {
    fn next(&mut self) -> u32 {
        self.0 = self.0.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407);
        (self.0 >> 33) as u32
    }
    fn below(&mut self, n: u32) -> u32 {
        self.next() % n
    }
}

/// (frequency, first slot) of `sym` in model (`bits`, `q`).
fn slot(bits: usize, q: usize, sym: u32) -> (u32, u32) {
    let seg = (32 - sym.leading_zeros()) as usize;
    let base: u32 = (0..seg).map(|k| FREQ[bits - 1][q][k] as u32 * if k < 2 { 1 } else { 1 << (k - 1) }).sum();
    let f = FREQ[bits - 1][q][seg] as u32;
    let cf = base + f * (sym - ((1u32 << seg) >> 1));
    if cf == 0 {
        (f - 1, 1)
    } else {
        (f, cf)
    }
}

/// One flushed fragment: header, state, payload. `trim`: drop the zero bytes an encoder
/// that starts at `L` may drop (those its first symbol pushed out).
fn fragment(symbols: &[u8], lane_bits: usize, z: usize, q: usize, trim: bool) -> Vec<u8> {
    let mut x = L;
    let mut emitted = Vec::new();
    let mut droppable = 0;
    for (k, &s) in symbols.iter().rev().enumerate() {
        let (f, cf) = slot(lane_bits - z, q, s as u32);
        while x >= f << 16 {
            emitted.push(x as u8);
            x >>= 8;
        }
        if k == 0 {
            droppable = emitted.len();
        }
        x = (x / f) << 16 | (x % f + cf);
    }
    let mut body = x.to_be_bytes().to_vec();
    body.extend(emitted.iter().rev());
    if trim {
        body.truncate(body.len() - droppable);
    }
    let mode = (z << 6 | 0x10 | q) as u8;
    let mut out = if body.len() <= 256 {
        vec![mode, (body.len() - 1) as u8]
    } else {
        let n = body.len() - 257;
        vec![mode | 0x20, n as u8, (n >> 8) as u8]
    };
    out.extend(body);
    out
}

/// A stream of random symbols and fragments, with its symbols (the decoder's order). The
/// lanes' bytes are put in the order the decoder asks for them, so this checks the decoder
/// against the encoder's arithmetic and itself, not the byte order against anything.
fn random_stream(widths: &[u8], units: usize, rng: &mut Rng) -> (Vec<u8>, Vec<u8>) {
    let n = widths.len();
    let mut symbols = vec![0u8; units * n];
    let mut queues: Vec<std::collections::VecDeque<u8>> = Vec::new();
    for (l, &w) in widths.iter().enumerate() {
        let mut bytes = Vec::new();
        let mut at = 0;
        while at < units {
            let longest = if rng.below(4) == 0 { 700 } else { 40 };
            let len = (1 + rng.below(longest) as usize).min(units - at);
            let z = rng.below(4).min(w as u32 - 1) as usize;
            let q = rng.below(16) as usize;
            let bits = w as usize - z;
            // the steeper models leave one slot to most symbols: keep those symbols rare
            let frag: Vec<u8> = (0..len)
                .map(|_| {
                    let top = if q >= 8 && rng.below(8) != 0 { 1u32 << bits.min(2) } else { 1 << bits };
                    rng.below(top) as u8
                })
                .collect();
            for (k, &s) in frag.iter().enumerate() {
                symbols[(at + k) * n + l] = s;
            }
            bytes.extend(fragment(&frag, w as usize, z, q, rng.below(2) == 0));
            at += len;
        }
        queues.push(bytes.into());
    }
    let mut stream = Vec::new();
    let got = lanes::run(widths, units, |lane| {
        let b = queues[lane].pop_front();
        stream.extend(b);
        b
    })
    .expect("the encoder's own fragments decode");
    assert_eq!(got, symbols);
    assert!(queues.iter().all(|q| q.is_empty()), "every fragment was read to its end");
    (stream, symbols)
}

#[test]
fn random_streams_round_trip_in_every_model() {
    let mut rng = Rng(0x5EED);
    let dump = std::env::var_os("XCL_BCPACK_DUMP").map(std::path::PathBuf::from);
    for round in 0..60 {
        for mode in [1u8, 3, 4, 5, 6, 7] {
            let widths = lane_widths(mode).unwrap();
            let units = [1, 2, 7, 64, 300, 2048][round % 6] * if mode == 1 || mode == 4 { 1 } else { 2 };
            let (stream, symbols) = random_stream(widths, units, &mut rng);
            assert_eq!(decode_lanes(mode, &stream, units * 8).as_deref(), Ok(&symbols[..]), "mode {mode} round {round}");
            // every byte of the stream is needed
            assert!(decode_lanes(mode, &stream[..stream.len() - 1], units * 8).is_err());
            if let Some(dir) = &dump {
                // for a comparison with other decoders: the stream and its lanes, lane by lane
                let stem = dir.join(format!("r{round}-bc{mode}-{units}"));
                let n = widths.len();
                let lane_major: Vec<u8> = (0..units * n).map(|i| symbols[(i % units) * n + i / units]).collect();
                std::fs::write(stem.with_extension("bin"), &stream).unwrap();
                std::fs::write(stem.with_extension("lanes"), lane_major).unwrap();
            }
        }
    }
}

/// Damaged input ends in an answer, never a panic or a hang. (The answer can be other
/// symbols: a stream has no checksum, and under a uniform model any bytes are symbols.)
#[test]
fn damaged_streams_end_cleanly() {
    let mut rng = Rng(7);
    for stem in ["bc1-2-0", "bc1-16-1", "bc1-16-9", "bc7-16-1", "bc1-700-2", "bc7-700-2"] {
        let (mode, units) = fixture(stem);
        let bin = data(&format!("{stem}.bin")).unwrap();
        let check = |bytes: &[u8], units: usize| decode_lanes(mode, bytes, units * 8);
        // every byte of a stream is some lane's: a shorter one is never whole
        for cut in (0..bin.len()).step_by((bin.len() / 300).max(1)) {
            assert!(check(&bin[..cut], units).is_err(), "{stem} cut at {cut}");
        }
        for _ in 0..400 {
            let mut b = bin.clone();
            let at = rng.below(b.len() as u32) as usize;
            match rng.below(3) {
                0 => b[at] ^= 1 << rng.below(8),
                1 => b[at] = rng.next() as u8,
                _ => {
                    b.remove(at);
                }
            }
            let _ = check(&b, units);
        }
        // the wrong number of blocks, and a size that is no number of blocks
        assert!(check(&bin, units + 1).is_err());
        assert!(check(&bin, units - 1).is_err());
        assert_eq!(decode_lanes(mode, &bin, units * 8 + 4), Err(Error::OutputSize));
    }
    // noise, and headers that claim more than there is
    for len in [0usize, 1, 2, 3, 47, 48, 64, 500, 5000] {
        for _ in 0..40 {
            let noise: Vec<u8> = (0..len).map(|_| rng.next() as u8).collect();
            for mode in [1u8, 7] {
                let _ = decode_lanes(mode, &noise, 64 * 8);
            }
        }
    }
    assert!(decode_lanes(1, &[0xFF; 4096], 1 << 20).is_err());
    assert!(decode_lanes(7, &[0x00; 4096], 1 << 20).is_err());
    assert_eq!(decode_lanes(2, &[], 16), Err(Error::Unsupported));
    assert_eq!(decode_lanes(1, &[], 0), Ok(Vec::new()));
}
