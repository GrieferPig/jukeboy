#![no_std]
#![no_main]

extern crate alloc;

use alloc::string::String;
use alloc::vec;
use core::ffi::c_char;
use core::net::{Ipv4Addr, SocketAddr};
use core::time::Duration;

use jukeboy::rt::{emit_line, udisplay_to_string, StringWriter};
use jukeboy_net::{DisplaySocketAddr, TcpListener, TcpStream};

fn render_error<T>(error: &T) -> String
where
    T: ufmt::uDisplay + ?Sized,
{
    udisplay_to_string(error)
}

fn run_server(port: u16) -> Result<(), String> {
    let bind_addr = SocketAddr::from((Ipv4Addr::UNSPECIFIED, port));
    let listener = TcpListener::bind(bind_addr).map_err(|err| render_error(&err))?;

    let mut msg = String::new();
    let mut writer = StringWriter::new(&mut msg);
    let _ = ufmt::uwrite!(&mut writer, "net-echo listening on {}", DisplaySocketAddr(&bind_addr));
    emit_line(&msg);

    let (mut stream, peer_addr) = listener.accept().map_err(|err| render_error(&err))?;
    let mut buffer = [0_u8; 512];

    msg.clear();
    let mut writer = StringWriter::new(&mut msg);
    let _ = ufmt::uwrite!(&mut writer, "accepted {}", DisplaySocketAddr(&peer_addr));
    emit_line(&msg);

    loop {
        let received = stream.read(&mut buffer).map_err(|err| render_error(&err))?;
        if received == 0 {
            break;
        }

        stream.write_all(&buffer[..received]).map_err(|err| render_error(&err))?;

        msg.clear();
        let mut writer = StringWriter::new(&mut msg);
        let _ = ufmt::uwrite!(&mut writer, "echoed {} bytes", received);
        emit_line(&msg);
    }

    Ok(())
}

fn run_client(host: &str, port: u16, message: &str) -> Result<(), String> {
    let mut stream = TcpStream::connect_host(host, port).map_err(|err| render_error(&err))?;
    let mut response = vec![0_u8; message.len().max(64)];

    stream.set_nodelay(true).map_err(|err| render_error(&err))?;
    stream.set_read_timeout(Some(Duration::from_secs(5))).map_err(|err| render_error(&err))?;
    stream.set_write_timeout(Some(Duration::from_secs(5))).map_err(|err| render_error(&err))?;

    let mut msg = String::new();
    let mut writer = StringWriter::new(&mut msg);
    let _ = ufmt::uwrite!(&mut writer, "net-echo connecting to {}:{}", host, port);
    emit_line(&msg);

    stream.write_all(message.as_bytes()).map_err(|err| render_error(&err))?;
    let read = stream.read(&mut response).map_err(|err| render_error(&err))?;

    msg.clear();
    let mut writer = StringWriter::new(&mut msg);
    let _ = ufmt::uwrite!(
        &mut writer,
        "response: {}",
        String::from_utf8_lossy(&response[..read]).as_ref()
    );
    emit_line(&msg);
    Ok(())
}

fn run(argc: i32, argv: *const *const c_char) -> Result<(), String> {
    let args = unsafe { jukeboy::rt::user_args_from_raw(argc, argv) };

    if args.is_empty() || args[0] == "listen" {
        let port = args
            .get(1)
            .map(String::as_str)
            .unwrap_or("7000")
            .parse::<u16>()
            .map_err(|_| String::from("invalid listen port"))?;
        return run_server(port);
    }

    if args[0] == "client" {
        let host = args.get(1).map(String::as_str).unwrap_or("127.0.0.1");
        let port = args
            .get(2)
            .map(String::as_str)
            .unwrap_or("7000")
            .parse::<u16>()
            .map_err(|_| String::from("invalid client port"))?;
        let message = args.get(3).map(String::as_str).unwrap_or("hello from net-echo client");
        return run_client(host, port, message);
    }

    emit_line("usage:");
    emit_line("  net-echo listen [port]");
    emit_line("  net-echo client [host] [port] [message]");
    Err(String::from("unknown net-echo mode"))
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