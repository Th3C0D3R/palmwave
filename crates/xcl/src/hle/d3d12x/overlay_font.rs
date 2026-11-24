//! Small original 5 x 7 glyphs, placed in 8 x 8 ASCII cells for point-sampled text.
pub(super) const WIDTH: u32 = 128;
pub(super) const HEIGHT: u32 = 64;
pub(super) const CELL_WIDTH: u32 = 8;
pub(super) const CELL_HEIGHT: u32 = 8;

fn rows(character: u8) -> [u8; 7] {
    match character {
        b'A' => [14, 17, 17, 31, 17, 17, 17],
        b'B' => [30, 17, 17, 30, 17, 17, 30],
        b'C' => [14, 17, 16, 16, 16, 17, 14],
        b'D' => [30, 17, 17, 17, 17, 17, 30],
        b'E' => [31, 16, 16, 30, 16, 16, 31],
        b'F' => [31, 16, 16, 30, 16, 16, 16],
        b'G' => [14, 17, 16, 23, 17, 17, 15],
        b'H' => [17, 17, 17, 31, 17, 17, 17],
        b'I' => [14, 4, 4, 4, 4, 4, 14],
        b'J' => [7, 2, 2, 2, 18, 18, 12],
        b'K' => [17, 18, 20, 24, 20, 18, 17],
        b'L' => [16, 16, 16, 16, 16, 16, 31],
        b'M' => [17, 27, 21, 21, 17, 17, 17],
        b'N' => [17, 25, 25, 21, 19, 19, 17],
        b'O' => [14, 17, 17, 17, 17, 17, 14],
        b'P' => [30, 17, 17, 30, 16, 16, 16],
        b'Q' => [14, 17, 17, 17, 21, 18, 13],
        b'R' => [30, 17, 17, 30, 20, 18, 17],
        b'S' => [15, 16, 16, 14, 1, 1, 30],
        b'T' => [31, 4, 4, 4, 4, 4, 4],
        b'U' => [17, 17, 17, 17, 17, 17, 14],
        b'V' => [17, 17, 17, 17, 17, 10, 4],
        b'W' => [17, 17, 17, 21, 21, 21, 10],
        b'X' => [17, 17, 10, 4, 10, 17, 17],
        b'Y' => [17, 17, 10, 4, 4, 4, 4],
        b'Z' => [31, 1, 2, 4, 8, 16, 31],
        b'0' => [14, 17, 19, 21, 25, 17, 14],
        b'1' => [4, 12, 4, 4, 4, 4, 14],
        b'2' => [14, 17, 1, 2, 4, 8, 31],
        b'3' => [30, 1, 1, 14, 1, 1, 30],
        b'4' => [2, 6, 10, 18, 31, 2, 2],
        b'5' => [31, 16, 16, 30, 1, 1, 30],
        b'6' => [14, 16, 16, 30, 17, 17, 14],
        b'7' => [31, 1, 2, 4, 8, 8, 8],
        b'8' => [14, 17, 17, 14, 17, 17, 14],
        b'9' => [14, 17, 17, 15, 1, 1, 14],
        b'.' => [0, 0, 0, 0, 0, 6, 6],
        b':' => [0, 6, 6, 0, 6, 6, 0],
        b'/' => [1, 2, 2, 4, 8, 8, 16],
        b'%' => [24, 25, 2, 4, 8, 19, 3],
        b'-' => [0, 0, 0, 31, 0, 0, 0],
        b'(' => [2, 4, 8, 8, 8, 4, 2],
        b')' => [8, 4, 2, 2, 2, 4, 8],
        b'?' => [14, 17, 1, 2, 4, 0, 4],
        _ => [0; 7],
    }
}

/// R8_UNORM pixels; ASCII byte c starts at ((c % 16) * 8, (c / 16) * 8).
pub(super) fn atlas() -> Vec<u8> {
    let mut pixels = vec![0; (WIDTH * HEIGHT) as usize];
    for character in 0..128u8 {
        let x = (character as u32 % 16) * CELL_WIDTH;
        let y = (character as u32 / 16) * CELL_HEIGHT;
        for (row, bits) in rows(character).iter().enumerate() {
            for column in 0..5 {
                if bits & (1 << (4 - column)) != 0 {
                    pixels[((y + row as u32) * WIDTH + x + column) as usize] = 255;
                }
            }
        }
    }
    pixels
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn atlas_keeps_glyphs_inside_ascii_cells() {
        let pixels = atlas();
        assert_eq!(pixels.len(), (WIDTH * HEIGHT) as usize);
        for character in 0..128u8 {
            let x = (character as u32 % 16) * CELL_WIDTH;
            let y = (character as u32 / 16) * CELL_HEIGHT;
            for row in 0..CELL_HEIGHT {
                for column in 0..CELL_WIDTH {
                    let on = row < 7 && column < 5 && rows(character)[row as usize] & (1 << (4 - column)) != 0;
                    assert_eq!(pixels[((y + row) * WIDTH + x + column) as usize], if on { 255 } else { 0 });
                }
            }
        }
    }

    #[test]
    fn font_covers_overlay_characters_and_leaves_space_empty() {
        for character in b"ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.:/%-()?" {
            assert!(rows(*character).iter().any(|row| *row != 0), "missing {}", *character as char);
        }
        assert_eq!(rows(b' '), [0; 7]);
        assert_eq!(rows(b'A')[0], 0b01110);
        assert_eq!(rows(b'A')[3], 0b11111);
        assert_eq!(rows(b'0')[3], 0b10101);
    }
}
