use std::error::Error;
use std::io::{Read, Write};
use std::net::{Ipv4Addr, SocketAddr};
use std::time::Duration;

use jukeboy_net::{TcpListener, TcpStream};

fn emit_line(message: impl AsRef<str>) {
    let message = message.as_ref();
    println!("{message}");
    let _ = jukeboy::log(message);
}

fn run_server(port: u16) -> Result<(), Box<dyn Error>> {
    let bind_addr = SocketAddr::from((Ipv4Addr::UNSPECIFIED, port));
    let listener = TcpListener::bind(bind_addr)?;

    emit_line(format!("net-echo listening on {}", bind_addr));
    jukeboy::log(format!("net-echo listening on {}", bind_addr))?;

    let (mut stream, peer_addr) = listener.accept()?;
    let mut buffer = [0_u8; 512];

    emit_line(format!("accepted {}", peer_addr));
    jukeboy::log(format!("net-echo accepted {}", peer_addr))?;

    loop {
        let received = stream.read(&mut buffer)?;
        if received == 0 {
            break;
        }

        stream.write_all(&buffer[..received])?;
        emit_line(format!("echoed {} bytes", received));
    }

    Ok(())
}

fn run_client(host: &str, port: u16, message: &str) -> Result<(), Box<dyn Error>> {
    let mut stream = TcpStream::connect_host(host, port)?;
    let mut response = vec![0_u8; message.len().max(64)];

    stream.set_nodelay(true)?;
    stream.set_read_timeout(Some(Duration::from_secs(5)))?;
    stream.set_write_timeout(Some(Duration::from_secs(5)))?;

    emit_line(format!("net-echo connecting to {}:{}", host, port));
    jukeboy::log(format!("net-echo connecting to {}:{}", host, port))?;

    stream.write_all(message.as_bytes())?;
    let read = stream.read(&mut response)?;

    emit_line(format!("response: {}", String::from_utf8_lossy(&response[..read])));
    Ok(())
}

fn run() -> Result<(), Box<dyn Error>> {
    let args: Vec<String> = std::env::args().skip(1).collect();

    if args.is_empty() || args[0] == "listen" {
        let port = args.get(1).map(String::as_str).unwrap_or("7000").parse::<u16>()?;
        return run_server(port);
    }

    if args[0] == "client" {
        let host = args.get(1).map(String::as_str).unwrap_or("127.0.0.1");
        let port = args.get(2).map(String::as_str).unwrap_or("7000").parse::<u16>()?;
        let message = args.get(3).map(String::as_str).unwrap_or("hello from net-echo client");
        return run_client(host, port, message);
    }

    emit_line("usage:");
    emit_line("  net-echo listen [port]");
    emit_line("  net-echo client [host] [port] [message]");
    Err("unknown net-echo mode".into())
}

fn main() -> Result<(), Box<dyn Error>> {
    if let Err(err) = run() {
        emit_line(format!("error: {err}"));
        return Err(err);
    }

    Ok(())
}