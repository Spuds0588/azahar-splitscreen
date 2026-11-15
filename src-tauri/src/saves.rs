//! Save sets: seeding a seat before it boots.
//!
//! A save set is an Azahar user dir - one of this orchestrator's `slots/pN/user`
//! directories, or an existing single-player install. Copying one into a seat
//! brings that player's SD saves, extra data and the title's NAND save data.
//! What is deliberately *not* copied is everything that makes a seat a separate
//! console: `nand/private` (SecureInfo_A and friends) and the shared Mii extdata
//! (`nand/data/<system>/extdata/00048000/F000000B`), so each seat keeps its own
//! identity and its own Mii name while playing with someone else's progress.

use crate::slots::Slot;
use std::fs;
use std::io::{self, Read, Seek, SeekFrom};
use std::path::{Path, PathBuf};

/// What to seed the seats with before the room opens.
pub enum Mode {
    /// Leave every slot as it is: fresh on first run, the player's own saves after.
    Keep,
    /// One save set for the whole group: every seat starts from the same copy.
    Grouped(PathBuf),
    /// A save set per seat, for bringing one player's existing saves in.
    PerSeat(Vec<Option<PathBuf>>),
}

/// The program id of a `.3ds`/`.cci` dump, from its NCSD partition 0 header.
/// `None` for formats without one (a `.3dsx` homebrew has no save data here).
pub fn title_id(rom: &Path) -> Option<u64> {
    let mut file = fs::File::open(rom).ok()?;
    let mut ncsd = [0u8; 0x200];
    file.read_exact(&mut ncsd).ok()?;
    if &ncsd[0x100..0x104] != b"NCSD" {
        return None;
    }
    let offset = u64::from(u32::from_le_bytes(ncsd[0x120..0x124].try_into().ok()?)) * 512;
    file.seek(SeekFrom::Start(offset)).ok()?;
    let mut ncch = [0u8; 0x200];
    file.read_exact(&mut ncch).ok()?;
    Some(u64::from_le_bytes(ncch[0x118..0x120].try_into().ok()?))
}

/// Copy `mode`'s save sets into each seat. Returns one log line per seat that
/// was seeded; a bad path is an error so a session never boots without the save
/// the player asked for.
pub fn apply(slots: &[Slot], mode: &Mode, title: Option<u64>) -> Result<Vec<String>, String> {
    let mut lines = Vec::new();
    for (seat, slot) in slots.iter().enumerate() {
        let source = match mode {
            Mode::Keep => continue,
            Mode::Grouped(source) => source,
            Mode::PerSeat(sources) => match sources.get(seat).and_then(Option::as_ref) {
                Some(source) => source,
                None => continue,
            },
        };
        let source = user_dir(source);
        if !source.is_dir() {
            return Err(format!(
                "save set for {} not found: {}",
                slot.name,
                source.display()
            ));
        }
        let files = copy_into(&source, &slot.user_dir, title)
            .map_err(|error| format!("could not copy {} into {}: {error}", source.display(), slot.name))?;
        if files == 0 {
            return Err(format!(
                "{} has no save data to import ({})",
                slot.name,
                source.display()
            ));
        }
        lines.push(format!(
            "{}: save set from {} ({files} files)",
            slot.name,
            source.display()
        ));
    }
    Ok(lines)
}

/// A slot directory (`.../pN`) or a user directory (`.../pN/user`) both work:
/// the one holding `nand` or `sdmc` is the user dir.
fn user_dir(path: &Path) -> PathBuf {
    let nested = path.join("user");
    if nested.join("nand").is_dir() || nested.join("sdmc").is_dir() {
        nested
    } else {
        path.to_path_buf()
    }
}

fn copy_into(source: &Path, target: &Path, title: Option<u64>) -> io::Result<usize> {
    let mut files = 0;
    let sd = source.join("sdmc").join("Nintendo 3DS");
    if sd.is_dir() {
        files += copy_tree(&sd, &target.join("sdmc").join("Nintendo 3DS"))?;
    }
    if let Some(title) = title {
        let name = format!("{title:016X}");
        let nand = source.join("nand").join("data").join(&name);
        if nand.is_dir() {
            files += copy_tree(&nand, &target.join("nand").join("data").join(&name))?;
        }
    }
    Ok(files)
}

fn copy_tree(source: &Path, target: &Path) -> io::Result<usize> {
    fs::create_dir_all(target)?;
    let mut files = 0;
    for entry in fs::read_dir(source)? {
        let entry = entry?;
        let to = target.join(entry.file_name());
        if entry.file_type()?.is_dir() {
            files += copy_tree(&entry.path(), &to)?;
        } else {
            fs::copy(entry.path(), &to)?;
            files += 1;
        }
    }
    Ok(files)
}
