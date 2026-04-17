extern crate alloc;

use alloc::string::String;
use alloc::vec::Vec;
use core::convert::Infallible;
use core::ffi::{c_char, CStr};
use core::fmt::{self, Write as _};
use core::panic::PanicInfo;

use dlmalloc::GlobalDlmalloc;

#[global_allocator]
static ALLOCATOR: GlobalDlmalloc = GlobalDlmalloc;

static PANIC_MESSAGE: &[u8] = b"jukeboy rust panic\0";

struct FixedLogBuffer<const N: usize>
{
    bytes: [u8; N],
    len: usize,
}

impl<const N: usize> FixedLogBuffer<N>
{
    const fn new() -> Self
    {
        Self {
            bytes: [0; N],
            len: 0,
        }
    }

    fn as_str(&self) -> &str
    {
        core::str::from_utf8(&self.bytes[..self.len]).unwrap_or("")
    }
}

impl<const N: usize> fmt::Write for FixedLogBuffer<N>
{
    fn write_str(&mut self, text: &str) -> fmt::Result
    {
        if self.len >= self.bytes.len()
        {
            return Ok(());
        }

        let remaining = self.bytes.len() - self.len;
        let copy_len = remaining.min(text.len());

        self.bytes[self.len..self.len + copy_len].copy_from_slice(&text.as_bytes()[..copy_len]);
        self.len += copy_len;
        Ok(())
    }
}

fn log_without_alloc(message: &str)
{
    let mut buffer = [0_u8; 192];
    let message_bytes = message.as_bytes();
    let copy_len = message_bytes.len().min(buffer.len().saturating_sub(1));

    buffer[..copy_len].copy_from_slice(&message_bytes[..copy_len]);
    buffer[copy_len] = 0;

    unsafe {
        crate::sys::log(buffer.as_ptr().cast());
    }
}

pub struct StringWriter<'a>
{
    inner: &'a mut String,
}

impl<'a> StringWriter<'a>
{
    pub fn new(inner: &'a mut String) -> Self
    {
        Self { inner }
    }
}

impl ufmt::uWrite for StringWriter<'_>
{
    type Error = Infallible;

    fn write_str(&mut self, text: &str) -> Result<(), Self::Error>
    {
        self.inner.push_str(text);
        Ok(())
    }
}

pub fn udisplay_to_string<T>(value: &T) -> String
where
    T: ufmt::uDisplay + ?Sized,
{
    let mut text = String::new();
    let mut writer = StringWriter::new(&mut text);
    let _ = ufmt::uwrite!(&mut writer, "{}", value);
    text
}

pub fn emit_line(message: &str)
{
    let _ = crate::log(message);
}

pub unsafe fn args_from_raw(argc: i32, argv: *const *const c_char) -> Vec<String>
{
    let mut args = Vec::new();

    if argc <= 0 || argv.is_null()
    {
        return args;
    }

    let raw_args = core::slice::from_raw_parts(argv, argc as usize);
    for raw_arg in raw_args
    {
        if raw_arg.is_null()
        {
            args.push(String::new());
            continue;
        }

        let c_str = CStr::from_ptr(*raw_arg);
        args.push(String::from_utf8_lossy(c_str.to_bytes()).into_owned());
    }

    args
}

pub unsafe fn user_args_from_raw(argc: i32, argv: *const *const c_char) -> Vec<String>
{
    let raw_args = args_from_raw(argc, argv);

    if raw_args.len() <= 1
    {
        return Vec::new();
    }

    raw_args.into_iter().skip(1).collect()
}

#[cfg(not(test))]
#[panic_handler]
fn panic(info: &PanicInfo<'_>) -> !
{
    let mut message = FixedLogBuffer::<191>::new();

    let _ = write!(&mut message, "panic: {}", info);
    if message.len > 0
    {
        log_without_alloc(message.as_str());
    }
    else {
        unsafe {
            crate::sys::log(PANIC_MESSAGE.as_ptr().cast());
        }
    }
    trap()
}

#[cfg(target_arch = "wasm32")]
#[inline(always)]
fn trap() -> !
{
    core::arch::wasm32::unreachable()
}

#[cfg(not(target_arch = "wasm32"))]
#[inline(always)]
fn trap() -> !
{
    loop {
        core::hint::spin_loop();
    }
}