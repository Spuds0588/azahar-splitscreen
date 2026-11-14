// Azahar Splitscreen orchestrator: pick a ROM, pick 2-4 players, start a
// loopback room plus one tiled Azahar instance per seat.
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod controllers;
mod layout;
mod process;
mod slots;

use std::sync::Mutex;
use tauri::Manager;

#[derive(Default)]
struct AppState {
    session: Mutex<Option<process::Session>>,
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
) -> Result<String, String> {
    let mut guard = state.session.lock().map_err(|e| e.to_string())?;
    if let Some(mut running) = guard.take() {
        if running.is_running() {
            *guard = Some(running);
            return Err("a session is already running".to_string());
        }
    }
    let (width, height) = monitor_size(&app);
    let session = process::start(&rom, players.clamp(2, 4), width, height)?;
    let summary = session.summary.clone();
    *guard = Some(session);
    Ok(summary)
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
        Some(session) => session.is_running(),
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

fn run_headless(rom: &str, players: u8) -> ! {
    // Ctrl-C has to take the room, the seats and the viewer with it, so the
    // signal only raises a flag: the session is dropped on the normal path.
    unsafe {
        libc::signal(libc::SIGINT, on_terminate as libc::sighandler_t);
        libc::signal(libc::SIGTERM, on_terminate as libc::sighandler_t);
    }
    let mut session = match process::start(rom, players.clamp(2, 4), 1920, 1080) {
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
    while session.is_running() && !TERMINATE.load(std::sync::atomic::Ordering::SeqCst) {
        std::thread::sleep(std::time::Duration::from_millis(500));
    }
    drop(session);
    std::process::exit(0);
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    if let Some(position) = args.iter().position(|arg| arg == "--start") {
        let rom = args.get(position + 1).cloned().unwrap_or_default();
        if rom.is_empty() {
            eprintln!("usage: azahar-splitscreen --start <rom> [--players 2-4]");
            std::process::exit(1);
        }
        let players = args
            .iter()
            .position(|arg| arg == "--players")
            .and_then(|index| args.get(index + 1))
            .and_then(|value| value.parse().ok())
            .unwrap_or(2);
        run_headless(&rom, players);
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
                drop(session);
                window.app_handle().exit(0);
            }
        })
        .build(tauri::generate_context!())
        .expect("error while building the Azahar Splitscreen orchestrator");

    app.run(|app, event| {
        // Quitting the window must take the room and every instance with it.
        if matches!(event, tauri::RunEvent::Exit) {
            let state = app.state::<AppState>();
            let session = state.session.lock().ok().and_then(|mut guard| guard.take());
            drop(session);
        }
    });
}
