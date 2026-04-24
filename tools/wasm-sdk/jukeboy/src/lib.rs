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

    fn from_nonnegative(value: i32) -> Result<i32> {
        if value < 0 {
            Err(Self::Status(value))
        }
        else {
            Ok(value)
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

#[repr(i32)]
#[derive(Clone, Copy, Debug, Eq, PartialEq, ufmt::derive::uDebug)]
pub enum Rail {
    Dac = 0,
    Led = 1,
}

#[repr(i32)]
#[derive(Clone, Copy, Debug, Eq, PartialEq, ufmt::derive::uDebug)]
pub enum Override {
    Auto = 0,
    On = 1,
    Off = 2,
}

impl TryFrom<i32> for Override {
    type Error = i32;

    fn try_from(value: i32) -> core::result::Result<Self, Self::Error> {
        match value {
            0 => Ok(Self::Auto),
            1 => Ok(Self::On),
            2 => Ok(Self::Off),
            other => Err(other),
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Button {
    Main1 = 0,
    Main2 = 1,
    Main3 = 2,
    Misc1 = 3,
    Misc2 = 4,
    Misc3 = 5,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ButtonSet(u32);

impl ButtonSet {
    pub fn bits(self) -> u32 {
        self.0
    }

    pub fn is_pressed(self, button: Button) -> bool {
        (self.0 & (1_u32 << button as u32)) != 0
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Rgb {
    pub r: u8,
    pub g: u8,
    pub b: u8,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct RailStatus {
    pub enabled: bool,
    pub refcount: usize,
    pub override_mode: Override,
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

pub fn rail_status(rail: Rail) -> Result<RailStatus> {
    let enabled = HostError::from_nonnegative(unsafe { sys::power_rail_is_enabled(rail as i32) })? != 0;
    let refcount = HostError::from_nonnegative(unsafe { sys::power_rail_get_refcount(rail as i32) })? as usize;
    let override_mode = HostError::from_nonnegative(unsafe { sys::power_rail_get_override(rail as i32) })?;
    let override_mode = Override::try_from(override_mode).map_err(HostError::Status)?;

    Ok(RailStatus {
        enabled,
        refcount,
        override_mode,
    })
}

pub fn rail_set_override(rail: Rail, override_mode: Override) -> Result<()> {
    HostError::from_status(unsafe { sys::power_rail_set_override(rail as i32, override_mode as i32) })
}

pub fn buttons() -> Result<ButtonSet> {
    let bits = HostError::from_nonnegative(unsafe { sys::hid_get_buttons() })? as u32;
    Ok(ButtonSet(bits))
}

pub fn led_set(rgb: Rgb) -> Result<()> {
    HostError::from_status(unsafe { sys::hid_led_set_rgb(rgb.r as i32, rgb.g as i32, rgb.b as i32) })
}

pub fn led_set_brightness(percent: u8) -> Result<()> {
    HostError::from_status(unsafe { sys::hid_led_set_brightness(percent.min(100) as i32) })
}

pub fn led_off() -> Result<()> {
    HostError::from_status(unsafe { sys::hid_led_off() })
}