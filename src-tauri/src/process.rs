//! Session lifecycle: one `azahar-room` plus one `azahar` per seat, each in
//! its own process group so teardown leaves no orphans.

use crate::{layout, saves, slots};
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
    /// The single window: composites the seats' shared-memory frames.
    viewer: Option<Child>,
    /// Per-seat frame rings and input sockets this session owns.
    rings: Vec<String>,
    sockets: Vec<PathBuf>,
    /// What a stalled seat needs to be spawned again, and how long its ring has
    /// been quiet. `restarts` caps the retries so a ROM that cannot run does not
    /// loop forever.
    seat_args: Vec<Vec<String>>,
    seat_logs: Vec<PathBuf>,
    bin: PathBuf,
    root: PathBuf,
    last_seq: Vec<u64>,
    last_move: Vec<Instant>,
    spawned_at: Vec<Instant>,
    restarts: Vec<u8>,
    pub log_path: PathBuf,
    pub summary: String,
}

impl Session {
    pub fn is_running(&mut self) -> bool {
        self.instances
            .iter_mut()
            .any(|child| matches!(child.try_wait(), Ok(None)))
    }

    /// True once the one window has closed. Closing it is how the app is left, so the
    /// orchestrator takes it as the end of the session rather than a missing child.
    pub fn viewer_gone(&mut self) -> bool {
        match self.viewer.as_mut() {
            Some(viewer) => matches!(viewer.try_wait(), Ok(Some(_))),
            // No viewer at all: the seats are the session (the headless fallback).
            None => false,
        }
    }

    /// Restart a seat whose ring stopped advancing: a crash, or a hang the game
    /// never comes back from. The ring and socket keep their names, so the
    /// viewer re-attaches and the seat rejoins the room on its own.
    pub fn watch(&mut self) {
        const STALL: Duration = Duration::from_secs(15);
        // A cold seat stops publishing for several seconds while the game
        // compiles shaders and joins the room; the stall test only arms once it
        // has had time to settle, or a restart would fight the boot.
        const ARM: Duration = Duration::from_secs(30);
        const MAX_RESTARTS: u8 = 3;

        for seat in 0..self.instances.len() {
            let status = self.instances[seat].try_wait().ok().flatten();
            let dead = status.is_some();
            let seq = ring_seq(&self.rings[seat]);
            if seq > self.last_seq[seat] {
                self.last_seq[seat] = seq;
                self.last_move[seat] = Instant::now();
                // Frames are flowing again: the tile can drop its restart chip.
                let _ = fs::remove_file(restart_marker(&self.seat_logs[seat]));
            }
            if !dead &&
                (self.spawned_at[seat].elapsed() < ARM ||
                    self.last_move[seat].elapsed() < STALL)
            {
                continue;
            }
            if self.restarts[seat] >= MAX_RESTARTS {
                // Out of retries: the tile keeps its last frame rather than
                // claiming a restart that will not happen.
                let _ = fs::remove_file(restart_marker(&self.seat_logs[seat]));
                continue;
            }

            let reason = match status {
                Some(status) => format!("exited ({status})"),
                None => format!("no frames for {:.0}s", self.last_move[seat].elapsed().as_secs_f32()),
            };
            // Tell the one window why the tile is about to go blank. The chip
            // shows until this seat publishes its first frame again.
            let _ = fs::write(restart_marker(&self.seat_logs[seat]), format!("{reason}\n"));
            terminate(&mut self.instances[seat]);
            // A killed seat leaves its ring behind, sometimes mid-resize; give
            // the replacement a clean object (the viewer re-attaches by inode).
            let _ = fs::remove_file(Path::new("/dev/shm").join(&self.rings[seat]));
            // Let the room drop the dead member before the replacement joins it.
            sleep(Duration::from_secs(2));
            self.restarts[seat] += 1;
            let spawned = spawn(&self.bin, &self.seat_args[seat], &self.root, &self.seat_logs[seat]);
            match spawned {
                Ok(child) => {
                    log(
                        &self.log_path,
                        &format!(
                            "Player {}: restarted ({}/{}): {reason}",
                            seat + 1,
                            self.restarts[seat],
                            MAX_RESTARTS
                        ),
                    );
                    self.instances[seat] = child;
                }
                Err(error) => {
                    log(
                        &self.log_path,
                        &format!("Player {}: restart failed: {error}", seat + 1),
                    );
                }
            }
            self.last_seq[seat] = 0;
            self.last_move[seat] = Instant::now();
            self.spawned_at[seat] = Instant::now();
        }
    }

    pub fn log_tail(&self) -> String {
        fs::read_to_string(&self.log_path).unwrap_or_default()
    }
}

impl Drop for Session {
    fn drop(&mut self) {
        if let Some(mut viewer) = self.viewer.take() {
            terminate(&mut viewer);
        }
        for child in &mut self.instances {
            terminate(child);
        }
        terminate(&mut self.room);
        for log in &self.seat_logs {
            let _ = fs::remove_file(restart_marker(log));
        }
        cleanup_artifacts(&self.rings, &self.sockets);
    }
}

/// Qt platform for the seats and the viewer. X11/XWayland is the proven path;
/// set `AZAHAR_QT_PLATFORM=wayland` to run the wayland-native acceptance test.
fn qt_platform() -> String {
    std::env::var("AZAHAR_QT_PLATFORM").unwrap_or_else(|_| "xcb".to_string())
}

/// Remove the session's shared-memory rings and input sockets. The emulator
/// unlinks both on a clean exit; this is what covers a killed or crashed seat.
fn cleanup_artifacts(rings: &[String], sockets: &[PathBuf]) {
    for ring in rings {
        let _ = fs::remove_file(Path::new("/dev/shm").join(ring));
    }
    for socket in sockets {
        let _ = fs::remove_file(socket);
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

pub fn start(
    rom: &str,
    players: u8,
    width: i32,
    height: i32,
    saves_mode: &saves::Mode,
) -> Result<Session, String> {
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

    // Seed the seats before anything boots: a save set is read once, at load.
    for line in saves::apply(&slots, saves_mode, saves::title_id(&rom_path))? {
        log(&log_path, &line);
    }

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
            // One spare slot: a seat that restarts rejoins before the room has
            // finished dropping its dead connection, and a full room rejects it.
            "--max_members".into(),
            (players.max(2) + 1).to_string(),
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

    // The seats no longer own windows, so the grid is only what the viewer
    // draws; log it so a session's shape is in the log next to the fps lines.
    let rects = layout::tiles(players, width, height);
    let (cols, rows) = layout::grid(players);
    let grid = rects
        .first()
        .map(|r| format!("{cols}x{rows} cells of {}x{}", r.width, r.height))
        .unwrap_or_default();
    log(&log_path, &format!("viewer grid: {grid}"));

    let rings: Vec<String> = (1..=slots.len())
        .map(|seat| format!("azsh-{}-{seat}", std::process::id()))
        .collect();
    let sockets: Vec<PathBuf> = slots
        .iter()
        .map(|slot| session_dir.join(format!("{}.sock", slot.name)))
        .collect();

    let mut instances = Vec::with_capacity(slots.len());
    let mut instance_logs = Vec::with_capacity(slots.len());
    let mut seat_args = Vec::with_capacity(slots.len());
    for ((slot, ring), socket) in slots.iter().zip(&rings).zip(&sockets) {
        let instance_log = session_dir.join(format!("{}.log", slot.name));
        // A marker left by a session that crashed says nothing about this one.
        let _ = fs::remove_file(restart_marker(&instance_log));
        instance_logs.push(instance_log.clone());
        let mut args = vec![
            "--user".into(),
            slot.user_dir.display().to_string(),
            // The seat renders into shared memory and takes input from its
            // socket; the viewer owns the only window in the session.
            "--shm-frame".into(),
            ring.clone(),
            "--input-socket".into(),
            socket.display().to_string(),
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
        ];
        // The ROM path has to stay the last argument.
        args.push(rom_path.display().to_string());
        match spawn(&bin, &args, &root, &instance_log) {
            Ok(child) => {
                seat_args.push(args);
                log(
                    &log_path,
                    &format!(
                        "{}: spawned headless, ring {ring}, socket {}, log {}",
                        slot.name,
                        socket.display(),
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
                cleanup_artifacts(&rings, &sockets);
                return Err(format!("failed to start {}: {error}", slot.name));
            }
        }
        sleep(Duration::from_millis(2000)); // staggered, as the sisters' notes require
    }

    // A dump the emulator cannot boot (still encrypted, no keys in the slot)
    // fails within a second of loading. Catch it here instead of leaving the
    // couch with four error dialogs on screen.
    if let Err(message) = check_boots(&slots, &instance_logs, Duration::from_secs(6), &rom_path) {
        for child in &mut instances {
            terminate(child);
        }
        terminate(&mut room);
        cleanup_artifacts(&rings, &sockets);
        return Err(message);
    }

    // Without the viewer there is no window at all - and the picker hides on a
    // successful start - so a missing viewer has to fail the start, not open a
    // session nobody can see.
    let viewer = match spawn_viewer(&root, &session_dir, &rings, &sockets, &instance_logs) {
        Some(viewer) => {
            log(&log_path, "viewer: one window compositing all seats");
            Some(viewer)
        }
        None => {
            for child in &mut instances {
                terminate(child);
            }
            terminate(&mut room);
            cleanup_artifacts(&rings, &sockets);
            return Err(
                "the one-window viewer is missing: build it with \
                 `ninja -C tools/viewer/build` (or use a packaged build)"
                    .to_string(),
            );
        }
    };

    // A fresh slot has no extdata for the game yet, so the 3DS stops on "No
    // extra data found. New extra data will be created." and waits for a tap
    // (Monster Hunter Stories is the case here). The seats have no window to
    // tap in, so clear it for the player.
    clear_boot_prompt(sockets.clone());

    let seat_count = slots.len();
    Ok(Session {
        room,
        instances,
        viewer,
        rings,
        sockets,
        seat_args,
        seat_logs: instance_logs,
        bin,
        root,
        last_seq: vec![0; seat_count],
        last_move: vec![Instant::now(); seat_count],
        spawned_at: vec![Instant::now(); seat_count],
        restarts: vec![0; seat_count],
        log_path,
        summary: format!("{seat_count} players in room splitscreen on port {port}"),
    })
}

/// A notice in the same single window, for a session that never started (an
/// encrypted dump): the reason on the TV instead of a terminal nobody reads.
pub fn show_message(message: &str) -> Option<Child> {
    let root = repo_root();
    let viewer = root.join("tools/viewer/build/azahar-viewer");
    if !viewer.exists() {
        return None;
    }
    spawn(
        &viewer,
        &["--message".into(), message.into()],
        &root,
        &root.join("slots/viewer.log"),
    )
    .ok()
}

/// The single window. Each seat contributes its frame ring, the socket its
/// input goes to, and the marker file that says it is restarting.
fn spawn_viewer(
    root: &Path,
    session_dir: &Path,
    rings: &[String],
    sockets: &[PathBuf],
    seat_logs: &[PathBuf],
) -> Option<Child> {
    let viewer = root.join("tools/viewer/build/azahar-viewer");
    if !viewer.exists() || rings.is_empty() {
        return None;
    }
    let mut args = Vec::with_capacity(rings.len() * 4);
    for ((ring, socket), seat_log) in rings.iter().zip(sockets).zip(seat_logs) {
        args.push("--seat".to_string());
        args.push(format!("{ring}:{}", socket.display()));
        args.push("--restart".to_string());
        args.push(format!("{ring}={}", restart_marker(seat_log).display()));
    }
    // The layout the window opens in; Tab switches it while the session runs.
    if let Ok(view) = std::env::var("AZAHAR_VIEW") {
        if !view.is_empty() {
            args.push("--view".to_string());
            args.push(view);
        }
    }
    spawn(&viewer, &args, root, &session_dir.join("viewer.log")).ok()
}

/// Fail fast when a seat could not load the ROM at all. The loader's verdict is
/// in the slot's own `azahar_log.txt`; the stdout log only carries frontend
/// chatter. Nothing is left running on failure, and the message names the ROM
/// so it can be shown in the one window as-is.
fn check_boots(
    slots: &[slots::Slot],
    logs: &[PathBuf],
    timeout: Duration,
    rom: &Path,
) -> Result<(), String> {
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
                let name = rom
                    .file_name()
                    .map(|name| name.to_string_lossy().to_string())
                    .unwrap_or_else(|| rom.display().to_string());
                return Err(format!(
                    "This game did not boot.\n\n{name}\n\nThe emulator reports an encrypted or \
                     unsupported application, and nothing was started. Pick a decrypted dump \
                     (or put the console's keys in the slot) and start again."
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
        .env("QT_QPA_PLATFORM", qt_platform())
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

/// Beside a seat's log: its presence means the orchestrator is restarting that
/// seat, which is what the viewer's tile chip reports.
fn restart_marker(seat_log: &Path) -> PathBuf {
    seat_log.with_extension("restart")
}

/// The ring's publish counter, or 0 while the seat has not created the object
/// (or has just replaced it).
fn ring_seq(ring: &str) -> u64 {
    use std::os::unix::fs::FileExt;
    let Ok(file) = File::open(format!("/dev/shm/{ring}")) else {
        return 0;
    };
    let mut bytes = [0u8; 8];
    if file.read_at(&mut bytes, 32).is_err() {
        return 0;
    }
    u64::from_le_bytes(bytes)
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

/// Confirm the one-time "No extra data found" prompt a fresh slot shows before a
/// game that keeps data in extdata can boot. One tap on the touch screen's Close
/// button, repeated so a seat that is still booting gets it as well.
fn clear_boot_prompt(sockets: Vec<PathBuf>) {
    std::thread::spawn(move || {
        for delay in [10, 6, 6] {
            sleep(Duration::from_secs(delay));
            for socket in &sockets {
                tap(socket, 200, 396);
            }
        }
    });
}

/// One touch down/up pair at a 400x480 frame coordinate, the same datagram the
/// viewer sends for a click on a tile.
fn tap(socket: &Path, x: u16, y: u16) {
    let Ok(datagram) = std::os::unix::net::UnixDatagram::unbound() else {
        return;
    };
    let mut message = [0u8; 12];
    message[8..10].copy_from_slice(&x.to_le_bytes());
    message[10..12].copy_from_slice(&y.to_le_bytes());
    for kind in [3u8, 5] {
        message[0] = kind;
        let _ = datagram.send_to(&message, socket);
        sleep(Duration::from_millis(120));
    }
}

pub fn log(path: &Path, line: &str) {
    use std::io::Write as _;
    if let Ok(mut file) = fs::OpenOptions::new().append(true).create(true).open(path) {
        let _ = writeln!(file, "{line}");
    }
}
