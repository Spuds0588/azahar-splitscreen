// Azahar Splitscreen orchestrator: pick a ROM, pick 2-6 players, start a
// loopback room plus one tiled Azahar instance per seat.
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod controllers;
mod layout;
mod process;
mod saves;
mod slots;

use std::path::PathBuf;
use std::sync::Mutex;
use tauri::Manager;

#[derive(Default)]
struct AppState {
    session: Mutex<Option<process::Session>>,
    /// The one-window notice for a start that failed (a bad save path, an
    /// encrypted dump). Detached, so the app has to close it on the way out.
    notice: Mutex<Option<std::process::Child>>,
}

#[tauri::command]
fn list_roms() -> Vec<process::Rom> {
    process::list_roms()
}

#[tauri::command]
fn list_pads() -> Vec<String> {
    controllers::list_pads()
        .into_iter()
        .map(|pad| format!("{} ({})", pad.name, pad.device))
        .collect()
}

#[tauri::command]
fn start_session(
    app: tauri::AppHandle,
    state: tauri::State<AppState>,
    rom: String,
    players: u8,
    save_mode: String,
    save_grouped: String,
    save_seats: Vec<String>,
    view: String,
) -> Result<String, String> {
    // The layout the one window opens in ("both", "top", or empty for the
    // default: stacked for two players, top screen for three or more). Tab and
    // the session's View menu still switch it live.
    let view = match view.as_str() {
        "both" | "top" => view,
        _ => String::new(),
    };
    std::env::set_var("AZAHAR_VIEW", view);
    let mut guard = state.session.lock().map_err(|e| e.to_string())?;
    if let Some(mut running) = guard.take() {
        if running.is_running() {
            *guard = Some(running);
            return Err("a session is already running".to_string());
        }
    }
    let (width, height) = monitor_size(&app);
    let mut notice = state.notice.lock().map_err(|e| e.to_string())?;
    close_notice(&mut notice);
    let mode = mode_from_picker(&save_mode, &save_grouped, &save_seats)?;
    let session = match process::start(&rom, players.clamp(2, 6), width, height, &mode) {
        Ok(session) => session,
        Err(error) => {
            // Nothing booted: put the reason on the TV, not only in the picker.
            *notice = process::show_message(&error);
            return Err(error);
        }
    };
    let summary = session.summary.clone();
    *guard = Some(session);
    // One window: the session window owns the session from here, so the picker stops
    // being a window at all rather than sitting behind the composite.
    if let Some(window) = app.get_webview_window("main") {
        let _ = window.hide();
    }
    Ok(summary)
}

#[tauri::command]
fn close_notice(notice: &mut Option<std::process::Child>) {
    if let Some(mut child) = notice.take() {
        let _ = child.kill();
        let _ = child.wait();
    }
}

#[tauri::command]
fn stop_session(state: tauri::State<AppState>) -> Result<(), String> {
    let mut guard = state.session.lock().map_err(|e| e.to_string())?;
    *guard = None; // Drop terminates the room and every instance
    Ok(())
}

#[tauri::command]
fn session_running(state: tauri::State<AppState>) -> bool {
    let mut guard = match state.session.lock() {
        Ok(guard) => guard,
        Err(_) => return false,
    };
    match guard.as_mut() {
        Some(session) => {
            // The UI polls this while a session runs; a seat whose frames stopped
            // is restarted here rather than reported as dead.
            session.watch();
            session.is_running()
        }
        None => false,
    }
}

#[tauri::command]
fn session_log(state: tauri::State<AppState>) -> String {
    let Ok(guard) = state.session.lock() else {
        return String::new();
    };
    match guard.as_ref() {
        Some(session) => session.log_tail(),
        None => String::new(),
    }
}

/// The picker's saves choice: a directory for every seat, a directory per seat,
/// or nothing (each slot keeps its own saves).
/// Tauri hands the picker's `saveMode` through as this `save_mode` argument, so
/// the page and the command have to keep the same name.
fn mode_from_picker(mode: &str, grouped: &str, seats: &[String]) -> Result<saves::Mode, String> {
    match mode {
        "grouped" => {
            let dir = grouped.trim();
            if dir.is_empty() {
                return Err("pick the save directory to copy into every player".to_string());
            }
            Ok(saves::Mode::Grouped(PathBuf::from(dir)))
        }
        "seats" => {
            let seats: Vec<Option<PathBuf>> = (0..4)
                .map(|seat| {
                    seats
                        .get(seat)
                        .map(|path| path.trim())
                        .filter(|path| !path.is_empty())
                        .map(PathBuf::from)
                })
                .collect();
            if seats.iter().all(Option::is_none) {
                return Err("give at least one player a save directory".to_string());
            }
            Ok(saves::Mode::PerSeat(seats))
        }
        _ => Ok(saves::Mode::Keep),
    }
}

fn monitor_size(app: &tauri::AppHandle) -> (i32, i32) {
    // The work area, not the monitor: a panel/taskbar would otherwise push the
    // bottom row up and make the tiles overlap.
    if let Ok(output) = std::process::Command::new("xprop")
        .args(["-root", "_NET_WORKAREA"])
        .output()
    {
        let text = String::from_utf8_lossy(&output.stdout);
        if let Some(values) = text.split('=').nth(1) {
            let numbers: Vec<i32> = values
                .split(',')
                .filter_map(|value| value.trim().parse().ok())
                .collect();
            if numbers.len() >= 4 {
                return (numbers[2], numbers[3]);
            }
        }
    }
    if let Some(window) = app.get_webview_window("main") {
        if let Ok(Some(monitor)) = window.current_monitor() {
            let size = monitor.size();
            return (size.width as i32, size.height as i32);
        }
    }
    (1920, 1080)
}

/// `--start <rom> [--players N]`: run a session without the picker, for QA
/// scripts and for starting a couch session from a terminal. It stays in the
/// foreground and tears everything down on Ctrl-C, exactly as the window does.
static TERMINATE: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);

extern "C" fn on_terminate(_: libc::c_int) {
    TERMINATE.store(true, std::sync::atomic::Ordering::SeqCst);
}

fn run_headless(rom: &str, players: u8, saves_mode: saves::Mode) -> ! {
    // Ctrl-C has to take the room, the seats and the viewer with it, so the
    // signal only raises a flag: the session is dropped on the normal path.
    unsafe {
        libc::signal(libc::SIGINT, on_terminate as libc::sighandler_t);
        libc::signal(libc::SIGTERM, on_terminate as libc::sighandler_t);
    }
    let mut session = match process::start(rom, players.clamp(2, 6), 1920, 1080, &saves_mode) {
        Ok(session) => session,
        Err(error) => {
            eprintln!("{error}");
            // Nothing is running: put the reason in the one window instead of
            // dropping back to a terminal the player may not even see.
            let mut window = process::show_message(&error);
            if let Some(child) = window.as_mut() {
                while !TERMINATE.load(std::sync::atomic::Ordering::SeqCst) {
                    if let Ok(Some(_)) = child.try_wait() {
                        break;
                    }
                    std::thread::sleep(std::time::Duration::from_millis(200));
                }
                let _ = child.kill();
                let _ = child.wait();
            }
            std::process::exit(1);
        }
    };
    println!("{}", session.summary);
    while !TERMINATE.load(std::sync::atomic::Ordering::SeqCst) {
        session.watch();
        // The session window owns the session here too: closing it ends the run.
        if !session.is_running() || session.viewer_gone() {
            break;
        }
        std::thread::sleep(std::time::Duration::from_millis(500));
    }
    drop(session);
    std::process::exit(0);
}

/// `--saves <dir>` seeds every seat from one save set; `--import <seat>=<dir>`
/// brings a single player's existing saves into that seat only.
fn cli_saves(args: &[String], players: u8) -> saves::Mode {
    if let Some(dir) = args
        .iter()
        .position(|arg| arg == "--saves")
        .and_then(|index| args.get(index + 1))
    {
        return saves::Mode::Grouped(PathBuf::from(dir));
    }
    let mut seats: Vec<Option<PathBuf>> = vec![None; usize::from(players.max(2))];
    let mut any = false;
    for (index, arg) in args.iter().enumerate() {
        if arg != "--import" {
            continue;
        }
        let Some((seat, path)) = args.get(index + 1).and_then(|spec| spec.split_once('=')) else {
            continue;
        };
        let seat = seat.parse::<usize>().unwrap_or(0).wrapping_sub(1);
        if let Some(slot) = seats.get_mut(seat) {
            *slot = Some(PathBuf::from(path));
            any = true;
        }
    }
    if any {
        saves::Mode::PerSeat(seats)
    } else {
        saves::Mode::Keep
    }
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    if let Some(position) = args.iter().position(|arg| arg == "--start") {
        let rom = args.get(position + 1).cloned().unwrap_or_default();
        if rom.is_empty() {
            eprintln!(
                "usage: azahar-splitscreen --start <rom> [--players 2-6] \
                 [--saves <dir>] [--import <seat>=<dir>]"
            );
            std::process::exit(1);
        }
        let players = args
            .iter()
            .position(|arg| arg == "--players")
            .and_then(|index| args.get(index + 1))
            .and_then(|value| value.parse().ok())
            .unwrap_or(2);
        let saves_mode = cli_saves(&args, players);
        run_headless(&rom, players, saves_mode);
    }

    let app = tauri::Builder::default()
        .manage(AppState::default())
        .invoke_handler(tauri::generate_handler![
            list_roms,
            list_pads,
            start_session,
            stop_session,
            session_running,
            session_log
        ])
        .on_window_event(|window, event| {
            // Closing the window is the other way out; take the session with it.
            if let tauri::WindowEvent::CloseRequested { .. } = event {
                let state = window.state::<AppState>();
                let session = state.session.lock().ok().and_then(|mut guard| guard.take());
                if let Ok(mut notice) = state.notice.lock() {
                    close_notice(&mut notice);
                }
                drop(session);
                window.app_handle().exit(0);
            }
        })
        .build(tauri::generate_context!())
        .expect("error while building the Azahar Splitscreen orchestrator");

    // The session window owns the session: when it closes, the room, every seat and the
    // orchestrator go with it. Polled from a thread because a hidden picker leaves the
    // event loop with nothing to do.
    let handle = app.handle().clone();
    std::thread::spawn(move || loop {
        std::thread::sleep(std::time::Duration::from_millis(500));
        let state = handle.state::<AppState>();
        let viewer_gone = match state.session.lock() {
            Ok(mut guard) => guard
                .as_mut()
                .map(|session| session.viewer_gone())
                .unwrap_or(false),
            Err(_) => false,
        };
        if viewer_gone {
            let session = state.session.lock().ok().and_then(|mut guard| guard.take());
            drop(session);
            handle.exit(0);
            return;
        }
    });

    app.run(|app, event| {
        // Quitting the window must take the room and every instance with it.
        if matches!(event, tauri::RunEvent::Exit) {
            let state = app.state::<AppState>();
            let session = state.session.lock().ok().and_then(|mut guard| guard.take());
            if let Ok(mut notice) = state.notice.lock() {
                close_notice(&mut notice);
            }
            drop(session);
        }
    });
}
