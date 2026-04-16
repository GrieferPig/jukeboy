#![no_std]

extern crate alloc;

pub mod addr;
pub mod error;
pub mod sys;
pub mod tcp;
pub mod udp;

pub use addr::{AddressFamily, DisplaySocketAddr, SocketType, WasiAddr, WasiAddrInfo, WasiAddrInfoHints};
pub use error::{Error, Result};
pub use core::net::{IpAddr, Ipv4Addr, Ipv6Addr, SocketAddr, SocketAddrV4, SocketAddrV6};
pub use tcp::{TcpListener, TcpStream};
pub use udp::UdpSocket;

pub fn resolve(host: &str, port: u16) -> Result<alloc::vec::Vec<SocketAddr>> {
    sys::resolve_socket_addrs(host, port, SocketType::Any)
}