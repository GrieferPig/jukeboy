use std::error::Error as StdError;
use std::ffi::{CStr, CString};
use std::fmt;

pub use jukeboy_sys as sys;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct HostError(pub i32);

impl HostError {
    pub fn from_code(code: i32) -> Result<(), Self> {
        if code == 0 {
            Ok(())
        }
        else {
            Err(Self(code))
        }
    }
}

impl fmt::Display for HostError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "host call failed with esp_err_t {}", self.0)
    }
}

impl StdError for HostError {}

#[repr(i32)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum PlaybackMode {
    Sequential = 0,
    SingleRepeat = 1,
    Shuffle = 2,
}

impl TryFrom<i32> for PlaybackMode {
    type Error = i32;

    fn try_from(value: i32) -> Result<Self, Self::Error> {
        match value {
            0 => Ok(Self::Sequential),
            1 => Ok(Self::SingleRepeat),
            2 => Ok(Self::Shuffle),
            other => Err(other),
        }
    }
}

fn sanitize_message(message: &str) -> CString {
    CString::new(message.replace('\0', " ")).unwrap_or_else(|_| CString::new("jukeboy log failed").unwrap())
}

pub fn log(message: impl AsRef<str>) -> Result<(), HostError> {
    let message = sanitize_message(message.as_ref());
    let status = unsafe { sys::log(message.as_ptr()) };
    HostError::from_code(status)
}

pub fn next_track() -> Result<(), HostError> {
    HostError::from_code(unsafe { sys::next_track() })
}

pub fn previous_track() -> Result<(), HostError> {
    HostError::from_code(unsafe { sys::previous_track() })
}

pub fn pause_toggle() -> Result<(), HostError> {
    HostError::from_code(unsafe { sys::pause_toggle() })
}

pub fn fast_forward() -> Result<(), HostError> {
    HostError::from_code(unsafe { sys::fast_forward() })
}

pub fn rewind() -> Result<(), HostError> {
    HostError::from_code(unsafe { sys::rewind() })
}

pub fn volume_up() -> Result<(), HostError> {
    HostError::from_code(unsafe { sys::volume_up() })
}

pub fn volume_down() -> Result<(), HostError> {
    HostError::from_code(unsafe { sys::volume_down() })
}

pub fn set_playback_mode(mode: PlaybackMode) -> Result<(), HostError> {
    HostError::from_code(unsafe { sys::set_playback_mode(mode as i32) })
}

pub fn playback_mode() -> Option<PlaybackMode> {
    PlaybackMode::try_from(unsafe { sys::get_playback_mode() }).ok()
}

pub fn is_paused() -> bool {
    unsafe { sys::is_paused() != 0 }
}

pub fn volume_percent() -> u32 {
    unsafe { sys::get_volume_percent().max(0) as u32 }
}

pub fn set_volume_percent(percent: u32) -> Result<(), HostError> {
    HostError::from_code(unsafe { sys::set_volume_percent(percent.min(100) as i32) })
}

pub fn sleep_ms(milliseconds: u32) -> Result<(), HostError> {
    HostError::from_code(unsafe { sys::sleep_ms(milliseconds.min(i32::MAX as u32) as i32) })
}

pub fn track_count() -> usize {
    unsafe { sys::get_track_count().max(0) as usize }
}

pub fn track_title(index: usize) -> Result<String, HostError> {
    let mut buffer = vec![0_i8; 256];
    let status = unsafe { sys::get_track_title(index.min(i32::MAX as usize) as i32, buffer.as_mut_ptr(), buffer.len() as u32) };
    HostError::from_code(status)?;

    let title = unsafe { CStr::from_ptr(buffer.as_ptr()) };
    Ok(title.to_string_lossy().into_owned())
}

pub fn track_titles() -> Vec<String> {
    (0..track_count()).filter_map(|index| track_title(index).ok()).collect()
}

pub fn wifi_is_connected() -> bool {
    unsafe { sys::wifi_is_connected() != 0 }
}

pub fn free_heap() -> usize {
    unsafe { sys::get_free_heap().max(0) as usize }
}

pub fn uptime_ms() -> i64 {
    unsafe { sys::get_uptime_ms() }
}