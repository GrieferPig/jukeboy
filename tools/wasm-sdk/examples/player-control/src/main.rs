use std::error::Error;

use jukeboy::PlaybackMode;

fn emit_line(message: impl AsRef<str>) {
    let message = message.as_ref();
    println!("{message}");
    let _ = jukeboy::log(message);
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
    emit_line(format!("wifi connected: {}", jukeboy::wifi_is_connected()));
    emit_line(format!("paused: {}", jukeboy::is_paused()));
    emit_line(format!("volume: {}%", jukeboy::volume_percent()));
    emit_line(format!("playback mode: {:?}", jukeboy::playback_mode()));
    emit_line(format!("track count: {}", jukeboy::track_count()));
    emit_line(format!("free heap: {} bytes", jukeboy::free_heap()));
}

fn list_tracks() {
    for (index, title) in jukeboy::track_titles().into_iter().enumerate() {
        emit_line(format!("{:02}: {}", index, title));
    }
}

fn parse_mode(value: &str) -> Option<PlaybackMode> {
    match value {
        "sequential" => Some(PlaybackMode::Sequential),
        "single" | "single-repeat" => Some(PlaybackMode::SingleRepeat),
        "shuffle" => Some(PlaybackMode::Shuffle),
        _ => None,
    }
}

fn main() -> Result<(), Box<dyn Error>> {
    let args: Vec<String> = std::env::args().skip(1).collect();

    jukeboy::log("player-control.wasm invoked")?;

    if args.is_empty() {
        print_status();
        return Ok(());
    }

    match args[0].as_str() {
        "status" => print_status(),
        "tracks" => list_tracks(),
        "next" => jukeboy::next_track()?,
        "prev" => jukeboy::previous_track()?,
        "pause" => jukeboy::pause_toggle()?,
        "ff" => jukeboy::fast_forward()?,
        "rewind" => jukeboy::rewind()?,
        "vol-up" => jukeboy::volume_up()?,
        "vol-down" => jukeboy::volume_down()?,
        "set-volume" => {
            let value = args.get(1).ok_or("missing volume value")?.parse::<u32>()?;
            jukeboy::set_volume_percent(value)?;
        }
        "mode" => {
            let mode = args.get(1).ok_or("missing playback mode")?;
            let mode = parse_mode(mode).ok_or("invalid playback mode")?;
            jukeboy::set_playback_mode(mode)?;
        }
        "sleep" => {
            let millis = args.get(1).ok_or("missing sleep duration")?.parse::<u32>()?;
            jukeboy::sleep_ms(millis)?;
        }
        _ => {
            print_usage();
            return Err("unknown command".into());
        }
    }

    if !matches!(args[0].as_str(), "status" | "tracks") {
        print_status();
    }

    Ok(())
}