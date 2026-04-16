fn emit_line(message: impl AsRef<str>) {
    let message = message.as_ref();
    println!("{message}");
    let _ = jukeboy::log(message);
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let args: Vec<String> = std::env::args().collect();
    let track_count = jukeboy::track_count();

    emit_line("hello from wasm32-wasip1");
    emit_line(format!("args: {:?}", args));
    emit_line(format!("free heap: {} bytes", jukeboy::free_heap()));
    emit_line(format!("uptime: {} ms", jukeboy::uptime_ms()));
    emit_line(format!("track count: {}", track_count));

    if track_count > 0 {
        emit_line(format!("first track: {}", jukeboy::track_title(0)?));
    }

    jukeboy::log(format!(
        "hello.wasm says hi; uptime={}ms heap={}B tracks={}",
        jukeboy::uptime_ms(),
        jukeboy::free_heap(),
        track_count,
    ))?;

    Ok(())
}