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

fn main() {
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
