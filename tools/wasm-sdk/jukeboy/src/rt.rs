extern crate alloc;

use alloc::string::String;
use alloc::vec::Vec;
use core::convert::Infallible;
use core::ffi::{c_char, CStr};
use core::panic::PanicInfo;

use dlmalloc::GlobalDlmalloc;

#[global_allocator]
static ALLOCATOR: GlobalDlmalloc = GlobalDlmalloc;

static PANIC_MESSAGE: &[u8] = b"jukeboy rust panic\0";
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
fn panic(_info: &PanicInfo<'_>) -> !
{
    unsafe {
        crate::sys::log(PANIC_MESSAGE.as_ptr().cast());
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