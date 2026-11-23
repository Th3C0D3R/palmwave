//! BCPack, the console's lossless packing of BCn texture blocks (`DSTORAGE_BCPACK_MODE`, the
//! DirectStorage stage between zlib and swizzle): a stream unpacks to plain BCn blocks.
//!
//! A stream has no header. The fields of the blocks are split over 12 or 16 rANS lanes, one
//! symbol per lane for every 8 bytes of output, and the lanes share the byte stream
//! (`lanes.rs`). The blocks are then rebuilt from the symbols; that part differs per format.
//!
//! What is here: the entropy stage for BC1, BC3, BC4, BC5, BC6H and BC7 ([`lane_widths`]); no
//! lane layout is published for BC2. Nothing calls it yet: DirectStorage still refuses every
//! request with a BCPack stage.
//!
//! What is not: the rebuild of blocks from the symbols, for any format.
//!
//! Nothing here was compared with what a console writes for the same input: the entropy stage
//! follows Microsoft's published decoder sample and the public format notes of bcpack-decoder
//! (Tim0219800) and bcpack-rs (Ahmet Dedeler), both MIT, whose authors report them as matching
//! retail streams. The tests check against streams built independently from those notes
//! (`tests/data/bcpack`), not against hardware output.

mod lanes;
mod rans_tables;

/// Why a stream was not unpacked.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Error {
    /// no lane layout for this mode
    Unsupported,
    /// the output is not a whole number of blocks, or too large
    OutputSize,
    /// the stream ends before the lane has its bytes
    Truncated { lane: usize },
    /// a fragment narrows the lane's symbols to nothing
    Width { lane: usize },
    /// the state points at the slot no symbol has
    Symbol { lane: usize },
    /// the lane's last symbol leaves a state no encoder starts from
    EndState { lane: usize },
    /// the lanes wait for each other or for a byte their fragments do not have
    Stuck,
    /// bytes other than zero padding follow the stream
    Trailing { consumed: usize },
}

impl Error {
    /// The kind of failure, without its numbers.
    pub fn kind(&self) -> &'static str {
        match self {
            Error::Unsupported => "unsupported",
            Error::OutputSize => "output size",
            Error::Truncated { .. } => "truncated",
            Error::Width { .. } => "width",
            Error::Symbol { .. } => "reserved slot",
            Error::EndState { .. } => "end state",
            Error::Stuck => "stuck",
            Error::Trailing { .. } => "trailing data",
        }
    }
}

impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match *self {
            Error::Unsupported => write!(f, "no decoder for this mode"),
            Error::OutputSize => write!(f, "the output size is not a whole number of blocks"),
            Error::Truncated { lane } => write!(f, "the stream ends while lane {lane} waits for a byte"),
            Error::Width { lane } => write!(f, "a fragment of lane {lane} narrows its symbols to nothing"),
            Error::Symbol { lane } => write!(f, "lane {lane} reads the reserved slot"),
            Error::EndState { lane } => write!(f, "lane {lane} ends in a state no encoder starts from"),
            Error::Stuck => write!(f, "the lanes cannot go on"),
            Error::Trailing { consumed } => write!(f, "data follows the stream's {consumed} bytes"),
        }
    }
}

/// The symbol width of each lane of a `DSTORAGE_BCPACK_MODE` (1 BC1 … 7 BC7), in stream order.
/// Every lane holds one symbol per 8 bytes of output: one per BC1/BC4 block, two per 16-byte
/// block.
pub fn lane_widths(mode: u8) -> Option<&'static [u8]> {
    Some(match mode {
        1 => &[8, 8, 5, 6, 5, 5, 6, 5, 8, 8, 8, 8],
        3 => &[4, 4, 8, 6, 6, 6, 6, 5, 6, 5, 8, 8],
        4 => &[8, 8, 8, 8, 6, 6, 6, 6, 6, 6, 6, 6],
        5 => &[4, 4, 8, 8, 6, 6, 6, 6, 6, 6, 6, 6],
        6 => &[4, 4, 8, 8, 8, 5, 5, 5, 6, 6, 6, 8, 8, 8, 8, 5],
        7 => &[4, 4, 8, 8, 8, 8, 8, 7, 7, 5, 6, 8, 8, 8, 8, 5],
        _ => return None,
    })
}

/// The symbols of a stream that unpacks to `out_len` bytes, unit by unit
/// (`symbols[unit * lanes + lane]`). Zero padding may follow the stream.
pub fn decode_lanes(mode: u8, src: &[u8], out_len: usize) -> Result<Vec<u8>, Error> {
    let widths = lane_widths(mode).ok_or(Error::Unsupported)?;
    if out_len % 8 != 0 {
        return Err(Error::OutputSize);
    }
    let (symbols, consumed) = lanes::decode(src, widths, out_len / 8)?;
    if src[consumed..].iter().any(|&b| b != 0) {
        return Err(Error::Trailing { consumed });
    }
    Ok(symbols)
}

#[cfg(test)]
pub(crate) mod tests;
