#![no_std]
#![no_main]

extern crate alloc;

use alloc::string::String;
use core::ffi::c_char;

use jukeboy::{Button, Override, Rail, Rgb};
use jukeboy::rt::{emit_line, udisplay_to_string, StringWriter};

fn render_error<T>(error: &T) -> String
where
    T: ufmt::uDisplay + ?Sized,
{
    udisplay_to_string(error)
}

fn print_status() -> Result<(), String> {
    let buttons = jukeboy::buttons().map_err(|err| render_error(&err))?;
    let rail_status = jukeboy::rail_status(Rail::Led).map_err(|err| render_error(&err))?;
    let mut message = String::new();

    let mut writer = StringWriter::new(&mut message);
    let _ = ufmt::uwrite!(
        &mut writer,
        "LED rail: enabled={} refcount={} override={:?}",
        rail_status.enabled,
        rail_status.refcount,
        rail_status.override_mode,
    );
    emit_line(&message);

    message.clear();
    let mut writer = StringWriter::new(&mut message);
    let _ = ufmt::uwrite!(
        &mut writer,
        "buttons: main1={} main2={} main3={} misc1={} misc2={} misc3={} bitmap=0x{:x}",
        buttons.is_pressed(Button::Main1),
        buttons.is_pressed(Button::Main2),
        buttons.is_pressed(Button::Main3),
        buttons.is_pressed(Button::Misc1),
        buttons.is_pressed(Button::Misc2),
        buttons.is_pressed(Button::Misc3),
        buttons.bits(),
    );
    emit_line(&message);

    Ok(())
}

fn run(argc: i32, argv: *const *const c_char) -> Result<(), String> {
    let _args = unsafe { jukeboy::rt::user_args_from_raw(argc, argv) };

    emit_line("hid-demo starting");
    jukeboy::rail_set_override(Rail::Led, Override::Auto).map_err(|err| render_error(&err))?;
    jukeboy::led_set_brightness(20).map_err(|err| render_error(&err))?;
    jukeboy::led_set(Rgb { r: 255, g: 0, b: 0 }).map_err(|err| render_error(&err))?;
    jukeboy::sleep_ms(1000).map_err(|err| render_error(&err))?;
    print_status()?;
    jukeboy::led_off().map_err(|err| render_error(&err))?;

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