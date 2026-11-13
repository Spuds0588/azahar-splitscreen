//! Window tiling math. 2 players share one row, 3-4 players use a 2x2 grid.

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Rect {
    pub x: i32,
    pub y: i32,
    pub width: i32,
    pub height: i32,
}

/// Returns one rect per player, in seat order, from a monitor work area.
pub fn tiles(players: u8, width: i32, height: i32) -> Vec<Rect> {
    const GUTTER: i32 = 1;
    let players = players.clamp(2, 4);
    let cols = if players <= 2 { 2 } else { 2 };
    let rows = if players <= 2 { 1 } else { 2 };
    let cell_w = (width - GUTTER * (cols - 1)) / cols;
    let cell_h = (height - GUTTER * (rows - 1)) / rows;

    (0..players)
        .map(|seat| {
            let col = seat as i32 % cols;
            let row = seat as i32 / cols;
            Rect {
                x: col * (cell_w + GUTTER),
                y: row * (cell_h + GUTTER),
                width: cell_w,
                height: cell_h,
            }
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn two_players_share_one_row() {
        let rects = tiles(2, 1920, 1080);
        assert_eq!(rects.len(), 2);
        assert_eq!(rects[0], Rect { x: 0, y: 0, width: 959, height: 1080 });
        assert_eq!(rects[1], Rect { x: 960, y: 0, width: 959, height: 1080 });
    }

    #[test]
    fn four_players_use_quadrants() {
        let rects = tiles(4, 1920, 1080);
        assert_eq!(rects.len(), 4);
        assert_eq!(rects[3], Rect { x: 960, y: 540, width: 959, height: 539 });
    }

    #[test]
    fn three_players_leave_the_last_cell_empty() {
        assert_eq!(tiles(3, 1920, 1080).len(), 3);
    }
}
