use std::error::Error;
use std::io::{Read, Write};
use std::time::Duration;

use jukeboy_net::TcpStream;

const DEFAULT_HOST: &str = "google.com";
const DEFAULT_PATH: &str = "/";
const DEFAULT_PORT: u16 = 80;
const MAX_CAPTURE_BYTES: usize = 4096;

fn emit_line(message: impl AsRef<str>) {
    let message = message.as_ref();
    println!("{message}");
    let _ = jukeboy::log(message);
}

fn normalize_path(path: &str) -> String {
    if path.is_empty() || path == "/" {
        return DEFAULT_PATH.to_string();
    }

    if path.starts_with('/') {
        return path.to_string();
    }

    format!("/{path}")
}

fn build_request(host: &str, path: &str) -> String {
    format!(
        "GET {path} HTTP/1.1\r\nHost: {host}\r\nUser-Agent: jukeboy-google-get/0.1\r\nAccept: */*\r\nConnection: close\r\n\r\n"
    )
}

fn read_response(stream: &mut TcpStream) -> Result<(Vec<u8>, usize), Box<dyn Error>> {
    let mut captured = Vec::with_capacity(MAX_CAPTURE_BYTES);
    let mut total_bytes = 0_usize;
    let mut buffer = [0_u8; 512];

    loop {
        let read = stream.read(&mut buffer)?;
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

fn run() -> Result<(), Box<dyn Error>> {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let host = args.get(0).map(String::as_str).unwrap_or(DEFAULT_HOST);
    let path = normalize_path(args.get(1).map(String::as_str).unwrap_or(DEFAULT_PATH));
    let port = match args.get(2) {
        Some(port) => port.parse::<u16>()?,
        None => DEFAULT_PORT,
    };

    emit_line(format!("google-get connecting to {host}:{port}{path}"));
    emit_line("note: this example uses plain HTTP; google.com usually redirects to HTTPS");

    let mut stream = TcpStream::connect_host(host, port)?;
    stream.set_nodelay(true)?;
    stream.set_read_timeout(Some(Duration::from_secs(10)))?;
    stream.set_write_timeout(Some(Duration::from_secs(10)))?;

    let request = build_request(host, &path);
    stream.write_all(request.as_bytes())?;

    let (response, total_bytes) = read_response(&mut stream)?;
    let response_text = String::from_utf8_lossy(&response);

    if let Some(status_line) = response_text.lines().next() {
        emit_line(format!("status: {status_line}"));
    }

    if let Some(location) = response_text.lines().find_map(|line| {
        line.strip_prefix("Location: ")
            .or_else(|| line.strip_prefix("location: "))
    }) {
        emit_line(format!("location: {location}"));
    }

    emit_line(format!(
        "received {total_bytes} bytes (showing up to {} bytes)",
        response.len()
    ));
    println!("--- response preview start ---");
    print!("{response_text}");
    if !response_text.ends_with('\n') {
        println!();
    }
    println!("--- response preview end ---");

    if total_bytes > response.len() {
        emit_line(format!(
            "response truncated by {} bytes",
            total_bytes - response.len()
        ));
    }

    Ok(())
}

fn main() -> Result<(), Box<dyn Error>> {
    if let Err(err) = run() {
        emit_line(format!("error: {err}"));
        return Err(err);
    }

    Ok(())
}