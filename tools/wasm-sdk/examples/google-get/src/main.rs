#![no_std]
#![no_main]

extern crate alloc;

use alloc::string::String;
use alloc::vec::Vec;
use core::ffi::c_char;
use core::time::Duration;

use jukeboy_net::TcpStream;
use jukeboy::rt::{emit_line, udisplay_to_string, StringWriter};

const DEFAULT_HOST: &str = "google.com";
const DEFAULT_PATH: &str = "/";
const DEFAULT_PORT: u16 = 80;
const MAX_CAPTURE_BYTES: usize = 4096;

fn normalize_path(path: &str) -> String {
    if path.is_empty() || path == "/" {
        return String::from(DEFAULT_PATH);
    }

    if path.starts_with('/') {
        return String::from(path);
    }

    let mut s = String::new();
    let mut writer = StringWriter::new(&mut s);
    let _ = ufmt::uwrite!(&mut writer, "/{}", path);
    s
}

fn build_request(host: &str, path: &str) -> String {
    let mut s = String::new();
    let mut writer = StringWriter::new(&mut s);
    let _ = ufmt::uwrite!(&mut writer,
        "GET {} HTTP/1.1\r\nHost: {}\r\nUser-Agent: jukeboy-google-get/0.1\r\nAccept: */*\r\nConnection: close\r\n\r\n",
        path, host
    );
    s
}

fn read_response(stream: &mut TcpStream) -> Result<(Vec<u8>, usize), String> {
    let mut captured = Vec::with_capacity(MAX_CAPTURE_BYTES);
    let mut total_bytes = 0_usize;
    let mut buffer = [0_u8; 512];

    loop {
        let read = stream.read(&mut buffer).map_err(|err| udisplay_to_string(&err))?;
        if read == 0 {
            break;
        }

        total_bytes += read;

        if captured.len() < MAX_CAPTURE_BYTES {
            let remaining = MAX_CAPTURE_BYTES - captured.len();
            let copy_len = remaining.min(read);
            captured.extend_from_slice(&buffer[..copy_len]);
        }
    }

    Ok((captured, total_bytes))
}

fn run(argc: i32, argv: *const *const c_char) -> Result<(), String> {
    let args = unsafe { jukeboy::rt::user_args_from_raw(argc, argv) };
    let host = args.get(0).map(String::as_str).unwrap_or(DEFAULT_HOST);
    let path = normalize_path(args.get(1).map(String::as_str).unwrap_or(DEFAULT_PATH));
    let port = match args.get(2) {
        Some(port) => port.parse::<u16>().map_err(|_| String::from("invalid port"))?,
        None => DEFAULT_PORT,
    };

    let mut msg = String::new();
    let mut writer = StringWriter::new(&mut msg);
    let _ = ufmt::uwrite!(&mut writer, "google-get connecting to {}:{}{}", host, port, path.as_str());
    emit_line(&msg);
    emit_line("note: this example uses plain HTTP; google.com usually redirects to HTTPS");

    let mut stream = TcpStream::connect_host(host, port).map_err(|err| udisplay_to_string(&err))?;
    stream.set_nodelay(true).map_err(|err| udisplay_to_string(&err))?;
    stream.set_read_timeout(Some(Duration::from_secs(10))).map_err(|err| udisplay_to_string(&err))?;
    stream.set_write_timeout(Some(Duration::from_secs(10))).map_err(|err| udisplay_to_string(&err))?;

    let request = build_request(host, &path);
    stream.write_all(request.as_bytes()).map_err(|err| udisplay_to_string(&err))?;

    let (response, total_bytes) = read_response(&mut stream)?;
    let response_text = String::from_utf8_lossy(&response);

    if let Some(status_line) = response_text.lines().next() {
        msg.clear();
        let mut writer = StringWriter::new(&mut msg);
        let _ = ufmt::uwrite!(&mut writer, "status: {}", status_line);
        emit_line(&msg);
    }

    if let Some(location) = response_text.lines().find_map(|line| {
        line.strip_prefix("Location: ")
            .or_else(|| line.strip_prefix("location: "))
    }) {
        msg.clear();
        let mut writer = StringWriter::new(&mut msg);
        let _ = ufmt::uwrite!(&mut writer, "location: {}", location);
        emit_line(&msg);
    }

    msg.clear();
    let mut writer = StringWriter::new(&mut msg);
    let _ = ufmt::uwrite!(&mut writer,
        "received {} bytes (showing up to {} bytes)",
        total_bytes, response.len()
    );
    emit_line(&msg);

    emit_line("--- response preview start ---");
    for line in response_text.lines() {
        emit_line(line);
    }
    if !response_text.ends_with('\n') && !response_text.is_empty() {
        emit_line("");
    }
    emit_line("--- response preview end ---");

    if total_bytes > response.len() {
        msg.clear();
        let mut writer = StringWriter::new(&mut msg);
        let _ = ufmt::uwrite!(&mut writer, "response truncated by {} bytes", total_bytes - response.len());
        emit_line(&msg);
    }

    Ok(())
}

#[export_name = "main"]
pub extern "C" fn wasm_main(argc: i32, argv: *const *const c_char) -> i32
{
    match run(argc, argv) {
        Ok(()) => 0,
        Err(err) => {
            let mut msg = String::from("error: ");
            msg.push_str(&err);
            emit_line(&msg);
            1
        }
    }
}