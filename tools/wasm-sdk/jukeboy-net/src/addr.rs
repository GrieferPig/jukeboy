#![allow(clippy::module_name_repetitions)]

use core::net::{IpAddr, Ipv4Addr, Ipv6Addr, SocketAddr};

pub const ANY_ADDRESS_POOL_FD: i32 = -1;

#[repr(u8)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum SocketType {
    Any = 0,
    Datagram = 1,
    Stream = 2,
}

impl Default for SocketType {
    fn default() -> Self {
        Self::Any
    }
}

#[repr(u8)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum AddressFamily {
    Unspec = 0,
    Inet4 = 1,
    Inet6 = 2,
}

impl Default for AddressFamily {
    fn default() -> Self {
        Self::Unspec
    }
}

#[repr(C)]
#[derive(Clone, Copy)]
pub union WasiAddrIp {
    pub v4: [u8; 4],
    pub v6: [u16; 8],
}

impl Default for WasiAddrIp {
    fn default() -> Self {
        Self { v4: [0; 4] }
    }
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct WasiAddr {
    pub kind: AddressFamily,
    pub reserved0: u8,
    pub port: u16,
    pub ip: WasiAddrIp,
}

impl Default for WasiAddr {
    fn default() -> Self {
        Self {
            kind: AddressFamily::Inet4,
            reserved0: 0,
            port: 0,
            ip: WasiAddrIp::default(),
        }
    }
}

impl WasiAddr {
    pub fn to_socket_addr(&self) -> Option<SocketAddr> {
        unsafe {
            match self.kind {
                AddressFamily::Inet4 => {
                    let ip = self.ip.v4;
                    Some(SocketAddr::new(IpAddr::V4(Ipv4Addr::new(ip[0], ip[1], ip[2], ip[3])), self.port))
                }
                AddressFamily::Inet6 => {
                    let ip = self.ip.v6;
                    Some(SocketAddr::new(
                        IpAddr::V6(Ipv6Addr::new(
                            ip[0], ip[1], ip[2], ip[3], ip[4], ip[5], ip[6], ip[7],
                        )),
                        self.port,
                    ))
                }
                AddressFamily::Unspec => None,
            }
        }
    }
}

impl From<SocketAddr> for WasiAddr {
    fn from(value: SocketAddr) -> Self {
        match value {
            SocketAddr::V4(addr) => Self {
                kind: AddressFamily::Inet4,
                reserved0: 0,
                port: addr.port(),
                ip: WasiAddrIp { v4: addr.ip().octets() },
            },
            SocketAddr::V6(addr) => {
                let segments = addr.ip().segments();
                Self {
                    kind: AddressFamily::Inet6,
                    reserved0: 0,
                    port: addr.port(),
                    ip: WasiAddrIp { v6: segments },
                }
            }
        }
    }
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct WasiAddrInfo {
    pub address: WasiAddr,
    pub socket_type: SocketType,
    pub is_internal: bool,
}

impl Default for WasiAddrInfo {
    fn default() -> Self {
        Self {
            address: WasiAddr::default(),
            socket_type: SocketType::Any,
            is_internal: false,
        }
    }
}

impl WasiAddrInfo {
    pub fn to_socket_addr(&self) -> Option<SocketAddr> {
        self.address.to_socket_addr()
    }
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct WasiAddrInfoHints {
    pub family: AddressFamily,
    pub socket_type: SocketType,
}

impl WasiAddrInfoHints {
    pub const fn new(socket_type: SocketType, family: AddressFamily) -> Self {
        Self {
            family,
            socket_type,
        }
    }
}

impl Default for WasiAddrInfoHints {
    fn default() -> Self {
        Self {
            family: AddressFamily::Unspec,
            socket_type: SocketType::Any,
        }
    }
}

pub struct DisplaySocketAddr<'a>(pub &'a SocketAddr);

fn write_hex_u16<W>(formatter: &mut ufmt::Formatter<'_, W>, value: u16) -> core::result::Result<(), W::Error>
where
    W: ufmt::uWrite + ?Sized,
{
    let digits = [
        ((value >> 12) & 0x0f) as u8,
        ((value >> 8) & 0x0f) as u8,
        ((value >> 4) & 0x0f) as u8,
        (value & 0x0f) as u8,
    ];
    let mut emitted = false;

    for (index, digit) in digits.into_iter().enumerate() {
        if digit != 0 || emitted || index == digits.len() - 1 {
            emitted = true;
            formatter.write_str(match digit {
                0 => "0",
                1 => "1",
                2 => "2",
                3 => "3",
                4 => "4",
                5 => "5",
                6 => "6",
                7 => "7",
                8 => "8",
                9 => "9",
                10 => "a",
                11 => "b",
                12 => "c",
                13 => "d",
                14 => "e",
                _ => "f",
            })?;
        }
    }

    Ok(())
}

impl ufmt::uDisplay for DisplaySocketAddr<'_> {
    fn fmt<W>(&self, formatter: &mut ufmt::Formatter<'_, W>) -> core::result::Result<(), W::Error>
    where
        W: ufmt::uWrite + ?Sized,
    {
        match self.0 {
            SocketAddr::V4(addr) => {
                let octets = addr.ip().octets();
                ufmt::uwrite!(formatter, "{}.{}.{}.{}:{}", octets[0], octets[1], octets[2], octets[3], addr.port())
            }
            SocketAddr::V6(addr) => {
                let segments = addr.ip().segments();
                formatter.write_str("[")?;
                for (index, segment) in segments.into_iter().enumerate() {
                    if index > 0 {
                        formatter.write_str(":")?;
                    }
                    write_hex_u16(formatter, segment)?;
                }
                formatter.write_str("]:")?;
                ufmt::uwrite!(formatter, "{}", addr.port())
            }
        }
    }
}