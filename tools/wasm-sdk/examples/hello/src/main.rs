#![no_std]
#![no_main]

extern crate alloc;

use alloc::string::String;
use core::ffi::c_char;

use jukeboy::rt::{emit_line, udisplay_to_string, StringWriter};

fn push_message(message: &mut String, text: &str)
{
    message.clear();
    message.push_str(text);
}

fn push_args(message: &mut String, args: &[String])
{
    message.clear();
    message.push('[');
    for (index, arg) in args.iter().enumerate() {
        if index > 0 {
            message.push_str(", ");
        }
        message.push('"');
        message.push_str(arg);
        message.push('"');
    }
    message.push(']');
}

fn run(argc: i32, argv: *const *const c_char) -> Result<(), String>
{
    let args = unsafe { jukeboy::rt::args_from_raw(argc, argv) };
    let track_count = jukeboy::track_count();
    let mut message = String::new();

    emit_line("hello from wasm32-unknown-unknown");

    push_args(&mut message, &args);
    let args_text = message.clone();
    push_message(&mut message, "args: ");
    message.push_str(&args_text);
    emit_line(&message);

    message.clear();
    let mut writer = StringWriter::new(&mut message);
    let _ = ufmt::uwrite!(&mut writer, "free heap: {} bytes", jukeboy::free_heap());
    emit_line(&message);

    message.clear();
    let mut writer = StringWriter::new(&mut message);
    let _ = ufmt::uwrite!(&mut writer, "uptime: {} ms", jukeboy::uptime_ms());
    emit_line(&message);

    message.clear();
    let mut writer = StringWriter::new(&mut message);
    let _ = ufmt::uwrite!(&mut writer, "track count: {}", track_count);
    emit_line(&message);

    if track_count > 0 {
        let title = jukeboy::track_title(0).map_err(|err| udisplay_to_string(&err))?;
        push_message(&mut message, "first track: ");
        message.push_str(&title);
        emit_line(&message);
    }

    message.clear();
    let mut writer = StringWriter::new(&mut message);
    let _ = ufmt::uwrite!(
        &mut writer,
        "hello.wasm says hi; uptime={}ms heap={}B tracks={}",
        jukeboy::uptime_ms(),
        jukeboy::free_heap(),
        track_count,
    );
    jukeboy::log(&message).map_err(|err| udisplay_to_string(&err))?;

    Ok(())
}

#[export_name = "main"]
pub extern "C" fn wasm_main(argc: i32, argv: *const *const c_char) -> i32
{
    match run(argc, argv) {
        Ok(()) => 0,
        Err(err) => {
            let mut message = String::from("error: ");
            message.push_str(&err);
            emit_line(&message);
            1
        }
    }
}