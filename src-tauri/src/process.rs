//! Session lifecycle: one `azahar-room` plus one `azahar` per seat, each in
//! its own process group so teardown leaves no orphans.

use crate::{layout, slots};
use std::fs::{self, File};
use std::io;
use std::net::UdpSocket;
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::thread::sleep;
use std::time::{Duration, Instant};

#[derive(Clone, serde::Serialize)]
pub struct Rom {
    pub path: String,
    pub name: String,
}

pub struct Session {
    room: Child,
    instances: Vec<Child>,
    /// Optional single-window container that adopts the seat windows (X11 only).
    viewer: Option<Child>,
    pub log_path: PathBuf,
    pub summary: String,
}

impl Session {
    pub fn is_running(&mut self) -> bool {
        self.instances
            .iter_mut()
            .any(|child| matches!(child.try_wait(), Ok(None)))
    }

    pub fn log_tail(&self) -> String {
        fs::read_to_string(&self.log_path).unwrap_or_default()
    }
}

impl Drop for Session {
    fn drop(&mut self) {
        // Stop the container first: it reparents the seat windows back to the
        // root window on a clean exit, so the instances keep their viewers.
        if let Some(mut viewer) = self.viewer.take() {
            terminate(&mut viewer);
        }
        for child in &mut self.instances {
            terminate(child);
        }
        terminate(&mut self.room);
    }
}

pub fn repo_root() -> PathBuf {
    if let Ok(root) = std::env::var("AZAHAR_SPLITS_ROOT") {
        return PathBuf::from(root);
    }
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .map(Path::to_path_buf)
        .unwrap_or_else(|| PathBuf::from("."))
}

pub fn azahar_bin(root: &Path) -> PathBuf {
    match std::env::var("AZAHAR_BIN") {
        Ok(bin) => PathBuf::from(bin),
        Err(_) => root.join("build/bin/Release/azahar"),
    }
}

pub fn list_roms() -> Vec<Rom> {
    let root = repo_root();
    let mut roms = Vec::new();
    for dir in [root.join("Test Roms"), root.clone()] {
        let Ok(entries) = fs::read_dir(&dir) else {
            continue;
        };
        for entry in entries.flatten() {
            let path = entry.path();
            if !path.is_file() {
                continue;
            }
            let ext = path
                .extension()
                .and_then(|e| e.to_str())
                .unwrap_or_default()
                .to_ascii_lowercase();
            if matches!(ext.as_str(), "3ds" | "cci" | "cxi" | "3dsx") {
                roms.push(Rom {
                    path: path.display().to_string(),
                    name: path
                        .file_name()
                        .map(|n| n.to_string_lossy().to_string())
                        .unwrap_or_default(),
                });
            }
        }
    }
    roms.sort_by(|a, b| a.name.cmp(&b.name));
    roms.dedup_by(|a, b| a.path == b.path);
    roms
}

pub fn start(rom: &str, players: u8, width: i32, height: i32) -> Result<Session, String> {
    let root = repo_root();
    let bin = azahar_bin(&root);
    let room_bin = root.join("build/bin/Release/azahar-room");
    if !bin.exists() {
        return Err(format!("azahar not found at {}", bin.display()));
    }
    if !room_bin.exists() {
        return Err(format!("azahar-room not found at {}", room_bin.display()));
    }

    let rom_path = if Path::new(rom).is_absolute() {
        PathBuf::from(rom)
    } else {
        root.join(rom)
    };
    if !rom_path.exists() {
        return Err(format!("ROM not found: {}", rom_path.display()));
    }

    let slots = slots::prepare(&root, players).map_err(|e| e.to_string())?;
    let session_dir = root.join("slots");
    fs::create_dir_all(&session_dir).map_err(|e| e.to_string())?;
    let log_path = session_dir.join("session.log");
    let _ = fs::write(&log_path, "");
    log(&log_path, &format!("rom: {}", rom_path.display()));
    log(&log_path, &format!("players: {players}, area: {width}x{height}"));

    let port = free_udp_port()?;
    log(&log_path, &format!("room: 127.0.0.1:{port}"));

    let room_log_path = session_dir.join("room.log");
    let mut room = spawn(
        &room_bin,
        &[
            "--room-name".into(),
            "splitscreen".into(),
            "--preferred-app".into(),
            "Azahar Splitscreen".into(),
            "--port".into(),
            port.to_string(),
            "--max_members".into(),
            players.max(2).to_string(),
            "--ban-list-file".into(),
            session_dir.join("ban_list.txt").display().to_string(),
        ],
        &root,
        &room_log_path,
    )
    .map_err(|e| format!("failed to start azahar-room: {e}"))?;

    if let Err(error) = wait_for_room(&mut room, &room_log_path) {
        terminate(&mut room);
        return Err(error);
    }
    log(&log_path, "room: open");

    let rects = layout::tiles(players, width, height);
    let mut instances = Vec::with_capacity(slots.len());
    let mut instance_logs = Vec::with_capacity(slots.len());
    for (slot, rect) in slots.iter().zip(rects) {
        let instance_log = session_dir.join(format!("{}.log", slot.name));
        instance_logs.push(instance_log.clone());
        let args = vec![
            "--user".into(),
            slot.user_dir.display().to_string(),
            "--position".into(),
            format!("{},{}", rect.x, rect.y),
            "--size".into(),
            format!("{}x{}", rect.width, rect.height),
            "--borderless".into(),
            "--join-room".into(),
            format!("127.0.0.1:{port}"),
            "--player-name".into(),
            slot.name.clone(),
            // One measured fps/speed/frame-time line per second per seat.
            "--stats".into(),
            session_dir
                .join(format!("{}.fps", slot.name))
                .display()
                .to_string(),
            rom_path.display().to_string(),
        ];
        match spawn(&bin, &args, &root, &instance_log) {
            Ok(child) => {
                log(
                    &log_path,
                    &format!(
                        "{}: spawned {}x{} at {},{}, log {}",
                        slot.name,
                        rect.width,
                        rect.height,
                        rect.x,
                        rect.y,
                        instance_log.display()
                    ),
                );
                instances.push(child);
            }
            Err(error) => {
                for child in &mut instances {
                    terminate(child);
                }
                terminate(&mut room);
                return Err(format!("failed to start {}: {error}", slot.name));
            }
        }
        sleep(Duration::from_millis(2000)); // staggered, as the sisters' notes require
    }

    // A dump the emulator cannot boot (still encrypted, no keys in the slot)
    // fails within a second of loading. Catch it here instead of leaving the
    // couch with four error dialogs on screen.
    if let Err(message) = check_boots(&slots, &instance_logs, Duration::from_secs(6)) {
        for child in &mut instances {
            terminate(child);
        }
        terminate(&mut room);
        return Err(message);
    }

    let viewer = spawn_viewer(&root, &session_dir, slots.len());
    if viewer.is_some() {
        log(&log_path, "viewer: single-window container adopted the seats");
    }

    Ok(Session {
        room,
        instances,
        viewer,
        log_path,
        summary: format!(
            "{} players in room splitscreen on port {port}",
            slots.len()
        ),
    })
}

/// Adopt the seat windows into one window when the optional container is built.
fn spawn_viewer(root: &Path, session_dir: &Path, seats: usize) -> Option<Child> {
    let viewer = root.join("tools/quadview/build/azahar-quadview");
    if !viewer.exists() {
        return None;
    }
    let args = vec!["--expect".to_string(), seats.to_string()];
    spawn(&viewer, &args, root, &session_dir.join("quadview.log")).ok()
}

/// Fail fast when a seat could not load the ROM at all. The loader's verdict is
/// in the slot's own `azahar_log.txt`; the stdout log only carries frontend
/// chatter.
fn check_boots(slots: &[slots::Slot], logs: &[PathBuf], timeout: Duration) -> Result<(), String> {
    let deadline = Instant::now() + timeout;
    while Instant::now() < deadline {
        for (slot, path) in slots.iter().zip(logs) {
            let mut text = fs::read_to_string(path).unwrap_or_default();
            text.push_str(
                &fs::read_to_string(slot.user_dir.join("log/azahar_log.txt")).unwrap_or_default(),
            );
            if text.contains("Failed to determine system mode") ||
                text.contains("Encrypted applications are not supported")
            {
                return Err(format!(
                    "This ROM did not boot — {} reports an encrypted or unsupported \
                     application. Nothing was started; pick a decrypted dump.",
                    path.file_stem().unwrap_or_default().to_string_lossy()
                ));
            }
        }
        sleep(Duration::from_millis(200));
    }
    Ok(())
}

fn spawn(bin: &Path, args: &[String], cwd: &Path, log_path: &Path) -> io::Result<Child> {
    let out = File::create(log_path)?;
    let err = out.try_clone()?;
    Command::new("setsid")
        .arg(bin)
        .args(args)
        .current_dir(cwd)
        .env("QT_QPA_PLATFORM", "xcb")
        // Exact tile sizes: Qt's DPI rounding would otherwise turn 960x540 into 1000x563.
        .env("QT_SCALE_FACTOR", "1")
        .env("QT_ENABLE_HIGHDPI_SCALING", "0")
        .stdin(Stdio::null())
        .stdout(Stdio::from(out))
        .stderr(Stdio::from(err))
        .spawn()
}

fn wait_for_room(room: &mut Child, log_path: &Path) -> Result<(), String> {
    let deadline = Instant::now() + Duration::from_secs(10);
    while Instant::now() < deadline {
        if let Ok(log) = fs::read_to_string(log_path) {
            if log.contains("Room is open") {
                return Ok(());
            }
        }
        if let Ok(Some(status)) = room.try_wait() {
            return Err(format!("azahar-room exited early ({status})"));
        }
        sleep(Duration::from_millis(200));
    }
    Err("azahar-room did not report an open room within 10s".to_string())
}

fn free_udp_port() -> Result<u16, String> {
    let socket = UdpSocket::bind("127.0.0.1:0").map_err(|e| e.to_string())?;
    let port = socket.local_addr().map_err(|e| e.to_string())?.port();
    drop(socket);
    Ok(port)
}

fn terminate(child: &mut Child) {
    let pid = child.id();
    // Each child is a session leader via setsid, so the negative PID reaches
    // its whole group (including any helper threads/children).
    let _ = Command::new("kill")
        .args(["-TERM", &format!("-{pid}")])
        .status();
    let _ = child.kill();
    let _ = child.wait();
    let _ = Command::new("kill")
        .args(["-KILL", &format!("-{pid}")])
        .status();
}

pub fn log(path: &Path, line: &str) {
    use std::io::Write as _;
    if let Ok(mut file) = fs::OpenOptions::new().append(true).create(true).open(path) {
        let _ = writeln!(file, "{line}");
    }
}
