#![no_std]

extern crate alloc;

use alloc::ffi::CString;
use alloc::string::String;
use alloc::vec;
use alloc::vec::Vec;
use core::ffi::CStr;

pub use jukeboy_sys as sys;

pub mod rt;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum HostError {
    Status(i32),
    InvalidString,
    InvalidUtf8,
}

impl HostError {
    fn from_status(status: i32) -> core::result::Result<(), Self> {
        if status == 0 {
            Ok(())
        }
        else {
            Err(Self::Status(status))
        }
    }
}

impl ufmt::uDisplay for HostError {
    fn fmt<W>(&self, formatter: &mut ufmt::Formatter<'_, W>) -> core::result::Result<(), W::Error>
    where
        W: ufmt::uWrite + ?Sized,
    {
        match self {
            Self::Status(code) => ufmt::uwrite!(formatter, "host call failed with esp_err_t {}", code),
            Self::InvalidString => formatter.write_str("host string contains NUL"),
            Self::InvalidUtf8 => formatter.write_str("host string is not valid UTF-8"),
        }
    }
}

pub type Result<T> = core::result::Result<T, HostError>;

#[repr(i32)]
#[derive(Clone, Copy, Debug, Eq, PartialEq, ufmt::derive::uDebug)]
pub enum PlaybackMode {
    Sequential = 0,
    SingleRepeat = 1,
    Shuffle = 2,
}

impl TryFrom<i32> for PlaybackMode {
    type Error = i32;

    fn try_from(value: i32) -> core::result::Result<Self, Self::Error> {
        match value {
            0 => Ok(Self::Sequential),
            1 => Ok(Self::SingleRepeat),
            2 => Ok(Self::Shuffle),
            other => Err(other),
        }
    }
}

fn c_string(value: &str) -> Result<CString> {
    if value.as_bytes().contains(&0)
    {
        return Err(HostError::InvalidString);
    }

    CString::new(value).map_err(|_| HostError::InvalidString)
}

pub fn log(message: &str) -> Result<()> {
    let message = c_string(message)?;
    let status = unsafe { sys::log(message.as_ptr()) };
    HostError::from_status(status)
}

pub fn next_track() -> Result<()> {
    HostError::from_status(unsafe { sys::next_track() })
}

pub fn previous_track() -> Result<()> {
    HostError::from_status(unsafe { sys::previous_track() })
}

pub fn pause_toggle() -> Result<()> {
    HostError::from_status(unsafe { sys::pause_toggle() })
}

pub fn fast_forward() -> Result<()> {
    HostError::from_status(unsafe { sys::fast_forward() })
}

pub fn rewind() -> Result<()> {
    HostError::from_status(unsafe { sys::rewind() })
}

pub fn volume_up() -> Result<()> {
    HostError::from_status(unsafe { sys::volume_up() })
}

pub fn volume_down() -> Result<()> {
    HostError::from_status(unsafe { sys::volume_down() })
}

pub fn set_playback_mode(mode: PlaybackMode) -> Result<()> {
    HostError::from_status(unsafe { sys::set_playback_mode(mode as i32) })
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

pub fn set_volume_percent(percent: u32) -> Result<()> {
    HostError::from_status(unsafe { sys::set_volume_percent(percent.min(100) as i32) })
}

pub fn sleep_ms(milliseconds: u32) -> Result<()> {
    HostError::from_status(unsafe { sys::sleep_ms(milliseconds.min(i32::MAX as u32) as i32) })
}

pub fn track_count() -> usize {
    unsafe { sys::get_track_count().max(0) as usize }
}

pub fn track_title(index: usize) -> Result<String> {
    let mut buffer = vec![0_u8; 256];
    let status = unsafe {
        sys::get_track_title(
            index.min(i32::MAX as usize) as i32,
            buffer.as_mut_ptr().cast(),
            buffer.len() as u32,
        )
    };
    HostError::from_status(status)?;

    let title = unsafe { CStr::from_ptr(buffer.as_ptr().cast()) };
    let title = title.to_str().map_err(|_| HostError::InvalidUtf8)?;
    Ok(String::from(title))
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