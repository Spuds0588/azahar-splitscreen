//! Gamepad discovery.
//!
//! This reads what the kernel exposes (`/dev/input/js*` and the names in
//! `/proc/bus/input/devices`). It is deliberately read-only: SDL-level
//! per-seat binding needs the GUIDs only Azahar's SDL stack knows, so the
//! orchestrator reports the pads it can see and the session falls back to
//! keyboard profiles. See `AGENTS.md` for the deferred item.

use std::fs;

pub struct Pad {
    pub name: String,
    pub device: String,
}

pub fn list_pads() -> Vec<Pad> {
    let mut pads = Vec::new();
    let Ok(devices) = fs::read_to_string("/proc/bus/input/devices") else {
        return pads;
    };

    let mut name = String::new();
    for line in devices.lines() {
        if let Some(rest) = line.strip_prefix("N: Name=") {
            name = rest.trim().trim_matches('"').to_string();
        } else if let Some(rest) = line.strip_prefix("H: Handlers=") {
            if let Some(js) = rest.split_whitespace().find(|h| h.starts_with("js")) {
                pads.push(Pad { name: name.clone(), device: format!("/dev/input/{js}") });
            }
        }
    }
    pads.sort_by(|a, b| a.device.cmp(&b.device));
    pads
}
