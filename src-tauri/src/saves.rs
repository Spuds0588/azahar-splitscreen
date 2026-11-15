//! Save sets: seeding a seat before it boots.
//!
//! A save set is an Azahar user dir - one of this orchestrator's `slots/pN/user`
//! directories, or an existing single-player install. Copying one into a seat
//! brings that player's SD saves, extra data and the title's NAND save data,
//! and *replaces* what the seat had: both trees are removed before the copy, so
//! a set with no progress really resets the seat instead of inheriting stale
//! files. What is deliberately *not* touched is everything that makes a seat a
//! separate console: `nand/private` (SecureInfo_A and friends) and the shared
//! Mii extdata (`nand/data/<system>/extdata/00048000/F000000B`), so each seat
//! keeps its own identity and its own Mii name while playing someone else's
//! progress.

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
        if !source.join("nand").is_dir() && !source.join("sdmc").is_dir() {
            return Err(format!(
                "not a save set (no nand/ or sdmc/): {}",
                source.display()
            ));
        }
        let files = copy_into(&source, &slot.user_dir, title)
            .map_err(|error| format!("could not copy {} into {}: {error}", source.display(), slot.name))?;
        // Zero files is a real choice: an empty set resets the seat to a new save.
        let size = match files {
            0 => "clean".to_string(),
            files => format!("{files} files"),
        };
        lines.push(format!(
            "{}: save set from {} ({size})",
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
    files += replace_tree(
        &source.join("sdmc").join("Nintendo 3DS"),
        &target.join("sdmc").join("Nintendo 3DS"),
    )?;
    if let Some(title) = title {
        let name = format!("{title:016X}");
        files += replace_tree(
            &source.join("nand").join("data").join(&name),
            &target.join("nand").join("data").join(&name),
        )?;
    }
    Ok(files)
}

/// Make `target` exactly the source's tree: remove what the seat had first, so
/// files the set does not contain cannot survive as stale progress. A source
/// without the tree clears the seat's copy, which is what a clean save means.
fn replace_tree(source: &Path, target: &Path) -> io::Result<usize> {
    match fs::remove_dir_all(target) {
        Ok(()) => {}
        Err(error) if error.kind() == io::ErrorKind::NotFound => {}
        Err(error) => return Err(error),
    }
    if !source.is_dir() {
        return Ok(0);
    }
    copy_tree(source, target)
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
