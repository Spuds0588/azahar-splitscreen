//! Per-slot user directories. Each seat gets its own `user/`, so NAND, saves,
//! config and shader cache cannot collide between instances.

use std::fs;
use std::io;
use std::path::{Path, PathBuf};

pub struct Slot {
    pub name: String,
    pub user_dir: PathBuf,
}

/// Creates (or reuses) `slots/pN/user/config` for every seat and writes the
/// settings the orchestrator needs. Existing NAND/save data is left alone.
pub fn prepare(root: &Path, players: u8) -> io::Result<Vec<Slot>> {
    let mut slots = Vec::with_capacity(players as usize);
    for seat in 0..players {
        let dir = root.join("slots").join(format!("p{}", seat + 1));
        let user_dir = dir.join("user");
        fs::create_dir_all(user_dir.join("config"))?;

        // Default layout (top screen above the touch screen). LargeScreen is
        // smaller on a TV but makes Monster Hunter Stories abort with a system
        // error dialog; the instance must also keep running unfocused.
        //
        // Audio: seat 1 owns the one device; every other seat uses Azahar's null sink
        // (`SinkType::Null`, 1 in `src/audio_core/sink_details.h`), so it opens no stream
        // and the room hears one copy of the music instead of six drifting copies.
        // `volume` is the console's own volume register, so it stays at 1 everywhere: this
        // is a device choice, not a volume trick.
        let audio = if seat == 0 {
            "[Audio]\nvolume=1\n".to_string()
        } else {
            "[Audio]\nvolume=1\noutput_type=1\n".to_string()
        };
        let config = format!(
            "[Layout]\nlayout_option=0\n\n[UI]\npause_when_in_background=false\n\n{audio}"
        );
        fs::write(user_dir.join("config").join("qt-config.ini"), config)?;

        seed_mii(&user_dir, seat)?;

        // Nicknames must match the room's 4-20 character rule.
        slots.push(Slot { name: format!("Player {}", seat + 1), user_dir });
    }
    Ok(slots)
}

/// Mii Maker's database lives in the shared extdata archive, and MK7 refuses to
/// run its profile flow (and therefore Local Play) without at least one Mii in
/// it. Path mirrors `ArchiveFactory_ExtSaveData` for shared data; the layout is
/// 3dbrew's CFL_DB.dat ("CFOG" + 100 Mii records + "CFHE" list + CRC-16/XMODEM).
fn seed_mii(user_dir: &Path, seat: u8) -> io::Result<()> {
    const SYSTEM_ID: &str = "00000000000000000000000000000000";
    let db = user_dir
        .join("nand")
        .join("data")
        .join(SYSTEM_ID)
        .join("extdata")
        .join("00048000")
        .join("F000000B") // PTM shared extdata id
        .join("user")
        .join("CFL_DB.dat");
    let database = mii_database(seat);
    // Each seat's Mii carries the seat's name, so an older seed gets rewritten.
    if let Ok(current) = fs::read(&db) {
        let name = 0x08 + 0x1A..0x08 + 0x2A;
        if current.get(name.clone()) == Some(&database[name]) {
            return Ok(());
        }
    }
    fs::create_dir_all(db.parent().expect("mii db has a parent"))?;
    fs::write(db, database)
}

const MII_SIZE: usize = 0x5C;
const DB_SIZE: usize = 0x4BD20;
/// Mii Maker keeps 100 owned Miis; the "met" list holds 3000 nodes of 0xE.
const MII_COUNT: usize = 100;
const NODE_COUNT: usize = 3000;
const NODE_SIZE: usize = 0xE;
/// The create-id Mii Maker's own "standard Mii" uses: bit 31 and bit 28 set, a
/// plausible creation date in the low bits. MK7 ignores records that fail this.
const BASE_MII_ID: u32 = 0x9839_1EE4;
const MII_MAC: [u8; 6] = [0x40, 0xF4, 0x07, 0xB7, 0x37, 0x10];

fn mii_database(seat: u8) -> Vec<u8> {
    let mii_id = BASE_MII_ID + u32::from(seat);
    let record = mii_record(&format!("Player {}", seat + 1), mii_id);
    let mut db = vec![0u8; DB_SIZE];

    db[0x0000..0x0004].copy_from_slice(b"CFOG");
    db[0x0004..0x0008].copy_from_slice(&0x100u32.to_le_bytes());
    db[0x0008..0x0008 + MII_SIZE].copy_from_slice(&record);

    // CFHE: one linked-list node pointing at Mii 0; every other node invalid.
    db[0x23F8..0x23FC].copy_from_slice(b"CFHE");
    db[0x23FC..0x23FE].copy_from_slice(&0u16.to_le_bytes()); // tail
    db[0x23FE..0x2400].copy_from_slice(&0u16.to_le_bytes()); // head
    let node = 0x2400;
    db[node..node + 4].copy_from_slice(&mii_id.to_be_bytes());
    db[node + 4..node + 10].copy_from_slice(&MII_MAC);
    db[node + 10..node + 12].copy_from_slice(&0x7FFFu16.to_le_bytes()); // prev
    db[node + 12..node + 14].copy_from_slice(&0x7FFFu16.to_le_bytes()); // next
    for i in 1..NODE_COUNT {
        let offset = node + i * NODE_SIZE;
        db[offset + 10..offset + 12].copy_from_slice(&0x7FFFu16.to_le_bytes());
        db[offset + 12..offset + 14].copy_from_slice(&0x7FFFu16.to_le_bytes());
    }

    // Big-endian checksum over the Mii Maker section.
    let crc = crc16_xmodem(&db[0x0..0xC81E]);
    db[0xC81E..0xC820].copy_from_slice(&crc.to_be_bytes());

    // Empty invitations section, plus its own checksum.
    db[0xC820..0xC824].copy_from_slice(b"CFRA");
    db[0xC824..0xC828].copy_from_slice(&0u32.to_le_bytes());
    db[0xE4AC..0xE4AE].copy_from_slice(&[0x01, 0x00]);
    let crc = crc16_xmodem(&db[0xC820..0xE4BE]);
    db[0xE4BE..0xE4C0].copy_from_slice(&crc.to_be_bytes());

    // Per-node Mii copies: the record without the author name, plus timestamp.
    let copy = 0xE4C0;
    db[copy..copy + 0x48].copy_from_slice(&record[0..0x48]);
    db[copy + 0x48..copy + 0x4C].copy_from_slice(&0x1000_0000u32.to_le_bytes());

    db
}

/// A packed `Mii::MiiData` (see `src/core/hle/mii.h`): big-endian numbers, native
/// (little-endian) UTF-16 names, like the console's own database.
fn mii_record(name: &str, mii_id: u32) -> [u8; MII_SIZE] {
    let mut b = [0u8; MII_SIZE];
    b[0x00] = 0x03; // version
    b[0x01] = 0x01; // copying allowed
    b[0x02] = 0x10; // page 0, slot 1
    b[0x03] = 0x30; // created on a 3DS
    b[0x04..0x0C].copy_from_slice(&0xD285_B6B3_00C8_850Au64.to_be_bytes());
    b[0x0C..0x10].copy_from_slice(&mii_id.to_be_bytes());
    b[0x10..0x16].copy_from_slice(&MII_MAC);
    b[0x18..0x1A].copy_from_slice(&0xA600u16.to_be_bytes()); // male, favourite
    // `Mii::Nickname` is a plain char16_t array, so the name is native UTF-16 (little
    // endian) even though the numeric fields around it are big endian.
    for (i, unit) in name.encode_utf16().take(10).enumerate() {
        b[0x1A + i * 2..0x1C + i * 2].copy_from_slice(&unit.to_le_bytes());
    }
    b[0x2E] = 0x40; // height
    b[0x2F] = 0x40; // width
    b[0x32] = 0x21; // hair style
    b[0x33] = 0x01; // hair colour
    b[0x34..0x38].copy_from_slice(&0x0268_4418u32.to_be_bytes()); // eyes
    b[0x38..0x3C].copy_from_slice(&0x2634_4614u32.to_be_bytes()); // eyebrows
    b[0x3C..0x3E].copy_from_slice(&0x8112u16.to_be_bytes()); // nose
    b[0x3E..0x40].copy_from_slice(&0x1768u16.to_be_bytes()); // mouth
    b[0x40..0x42].copy_from_slice(&0x0D00u16.to_be_bytes()); // moustache
    b[0x42..0x44].copy_from_slice(&0x0029u16.to_be_bytes()); // beard
    b[0x44..0x46].copy_from_slice(&0x0052u16.to_be_bytes()); // glasses
    b[0x46..0x48].copy_from_slice(&0x4850u16.to_be_bytes()); // mole
    for (i, unit) in "Azahar".encode_utf16().take(10).enumerate() {
        b[0x48 + i * 2..0x4A + i * 2].copy_from_slice(&unit.to_le_bytes());
    }
    b
}

/// CRC-16/XMODEM with the trailing 16 zero bits the 3DS database checksums use.
fn crc16_xmodem(data: &[u8]) -> u16 {
    let mut crc: u32 = 0;
    for byte in data {
        for bit in (0..8).rev() {
            let top = (crc & 0x8000) != 0;
            crc = ((crc << 1) | u32::from((byte >> bit) & 1)) & 0xFFFF;
            if top {
                crc ^= 0x1021;
            }
        }
    }
    for _ in 0..16 {
        let top = (crc & 0x8000) != 0;
        crc = (crc << 1) & 0xFFFF;
        if top {
            crc ^= 0x1021;
        }
    }
    crc as u16
}
