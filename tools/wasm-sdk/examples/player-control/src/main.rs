#![no_std]
#![no_main]

extern crate alloc;

use alloc::string::String;
use core::ffi::c_char;

use jukeboy::PlaybackMode;
use jukeboy::rt::{emit_line, udisplay_to_string, StringWriter};

fn render_error<T>(error: &T) -> String
where
    T: ufmt::uDisplay + ?Sized,
{
    udisplay_to_string(error)
}

fn print_usage() {
    emit_line("player-control commands:");
    emit_line("  status");
    emit_line("  tracks");
    emit_line("  next | prev | pause | ff | rewind");
    emit_line("  vol-up | vol-down | set-volume <0-100>");
    emit_line("  mode <sequential|single|shuffle>");
    emit_line("  sleep <milliseconds>");
}

fn print_status() {
    let mut msg = String::new();
    let mut writer = StringWriter::new(&mut msg);
    let _ = ufmt::uwrite!(&mut writer, "wifi connected: {}", jukeboy::wifi_is_connected());
    emit_line(&msg);

    msg.clear();
    let mut writer = StringWriter::new(&mut msg);
    let _ = ufmt::uwrite!(&mut writer, "paused: {}", jukeboy::is_paused());
    emit_line(&msg);

    msg.clear();
    let mut writer = StringWriter::new(&mut msg);
    let _ = ufmt::uwrite!(&mut writer, "volume: {}%", jukeboy::volume_percent());
    emit_line(&msg);

    msg.clear();
    let mut writer = StringWriter::new(&mut msg);
    let _ = ufmt::uwrite!(&mut writer, "playback mode: {:?}", jukeboy::playback_mode());
    emit_line(&msg);

    msg.clear();
    let mut writer = StringWriter::new(&mut msg);
    let _ = ufmt::uwrite!(&mut writer, "track count: {}", jukeboy::track_count());
    emit_line(&msg);

    msg.clear();
    let mut writer = StringWriter::new(&mut msg);
    let _ = ufmt::uwrite!(&mut writer, "free heap: {} bytes", jukeboy::free_heap());
    emit_line(&msg);
}

fn list_tracks() -> Result<(), String> {
    let mut msg = String::new();
    for index in 0..jukeboy::track_count() {
        let title = jukeboy::track_title(index).map_err(|err| render_error(&err))?;
        msg.clear();
        let pad = if index < 10 { "0" } else { "" };
        let mut writer = StringWriter::new(&mut msg);
        let _ = ufmt::uwrite!(&mut writer, "{}{}: {}", pad, index, title.as_str());
        emit_line(&msg);
    }

    Ok(())
}

fn parse_mode(value: &str) -> Option<PlaybackMode> {
    match value {
        "sequential" => Some(PlaybackMode::Sequential),
        "single" | "single-repeat" => Some(PlaybackMode::SingleRepeat),
        "shuffle" => Some(PlaybackMode::Shuffle),
        _ => None,
    }
}

fn run(argc: i32, argv: *const *const c_char) -> Result<(), String> {
    let args = unsafe { jukeboy::rt::user_args_from_raw(argc, argv) };

    jukeboy::log("player-control.wasm invoked").map_err(|err| render_error(&err))?;

    if args.is_empty() {
        print_status();
        return Ok(());
    }

    match args[0].as_str() {
        "status" => print_status(),
        "tracks" => list_tracks()?,
        "next" => jukeboy::next_track().map_err(|err| render_error(&err))?,
        "prev" => jukeboy::previous_track().map_err(|err| render_error(&err))?,
        "pause" => jukeboy::pause_toggle().map_err(|err| render_error(&err))?,
        "ff" => jukeboy::fast_forward().map_err(|err| render_error(&err))?,
        "rewind" => jukeboy::rewind().map_err(|err| render_error(&err))?,
        "vol-up" => jukeboy::volume_up().map_err(|err| render_error(&err))?,
        "vol-down" => jukeboy::volume_down().map_err(|err| render_error(&err))?,
        "set-volume" => {
            let value = args
                .get(1)
                .ok_or_else(|| String::from("missing volume value"))?
                .parse::<u32>()
                .map_err(|_| String::from("invalid volume value"))?;
            jukeboy::set_volume_percent(value).map_err(|err| render_error(&err))?;
        }
        "mode" => {
            let mode = args.get(1).ok_or_else(|| String::from("missing playback mode"))?;
            let mode = parse_mode(mode).ok_or_else(|| String::from("invalid playback mode"))?;
            jukeboy::set_playback_mode(mode).map_err(|err| render_error(&err))?;
        }
        "sleep" => {
            let millis = args
                .get(1)
                .ok_or_else(|| String::from("missing sleep duration"))?
                .parse::<u32>()
                .map_err(|_| String::from("invalid sleep duration"))?;
            jukeboy::sleep_ms(millis).map_err(|err| render_error(&err))?;
        }
        _ => {
            print_usage();
            return Err(String::from("unknown command"));
        }
    }

    if !matches!(args[0].as_str(), "status" | "tracks") {
        print_status();
    }

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