#![no_std]

use core::ffi::c_char;

#[link(wasm_import_module = "jukeboy")]
unsafe extern "C" {
    pub fn log(message: *const c_char) -> i32;

    pub fn next_track() -> i32;
    pub fn previous_track() -> i32;
    pub fn pause_toggle() -> i32;
    pub fn fast_forward() -> i32;
    pub fn rewind() -> i32;
    pub fn volume_up() -> i32;
    pub fn volume_down() -> i32;

    pub fn set_playback_mode(mode: i32) -> i32;
    pub fn get_playback_mode() -> i32;
    pub fn is_paused() -> i32;

    pub fn get_volume_percent() -> i32;
    pub fn set_volume_percent(percent: i32) -> i32;

    pub fn sleep_ms(milliseconds: i32) -> i32;

    pub fn get_track_count() -> i32;
    pub fn get_track_title(index: i32, buffer: *mut c_char, buffer_len: u32) -> i32;

    pub fn wifi_is_connected() -> i32;
    pub fn get_free_heap() -> i32;
    pub fn get_uptime_ms() -> i64;
}