use std::net::{IpAddr, Ipv4Addr, Ipv6Addr, SocketAddr};

pub const ANY_ADDRESS_POOL_FD: i32 = -1;

#[repr(i32)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum SocketType {
    Any = -1,
    Dgram = 0,
    Stream = 1,
}

#[repr(i32)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum AddressFamily {
    Inet4 = 0,
    Inet6 = 1,
    Unspec = 2,
}

#[repr(i32)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum AddrType {
    Ipv4 = 0,
    Ipv6 = 1,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct WasiAddrIp4 {
    pub n0: u8,
    pub n1: u8,
    pub n2: u8,
    pub n3: u8,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct WasiAddrIp4Port {
    pub addr: WasiAddrIp4,
    pub port: u16,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct WasiAddrIp6 {
    pub n0: u16,
    pub n1: u16,
    pub n2: u16,
    pub n3: u16,
    pub h0: u16,
    pub h1: u16,
    pub h2: u16,
    pub h3: u16,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct WasiAddrIp6Port {
    pub addr: WasiAddrIp6,
    pub port: u16,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub union WasiAddrPayload {
    pub ip4: WasiAddrIp4Port,
    pub ip6: WasiAddrIp6Port,
}

impl Default for WasiAddrPayload {
    fn default() -> Self {
        Self {
            ip4: WasiAddrIp4Port::default(),
        }
    }
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct WasiAddr {
    pub kind: AddrType,
    pub addr: WasiAddrPayload,
}

impl Default for WasiAddr {
    fn default() -> Self {
        Self::from(SocketAddr::from(([0, 0, 0, 0], 0)))
    }
}

impl WasiAddr {
    pub fn family(&self) -> AddressFamily {
        match self.kind {
            AddrType::Ipv4 => AddressFamily::Inet4,
            AddrType::Ipv6 => AddressFamily::Inet6,
        }
    }

    pub fn to_socket_addr(&self) -> Option<SocketAddr> {
        unsafe {
            match self.kind {
                AddrType::Ipv4 => {
                    let ip4 = self.addr.ip4;
                    Some(SocketAddr::new(
                        IpAddr::V4(Ipv4Addr::new(ip4.addr.n0, ip4.addr.n1, ip4.addr.n2, ip4.addr.n3)),
                        ip4.port,
                    ))
                }
                AddrType::Ipv6 => {
                    let ip6 = self.addr.ip6;
                    Some(SocketAddr::new(
                        IpAddr::V6(Ipv6Addr::new(
                            ip6.addr.n0,
                            ip6.addr.n1,
                            ip6.addr.n2,
                            ip6.addr.n3,
                            ip6.addr.h0,
                            ip6.addr.h1,
                            ip6.addr.h2,
                            ip6.addr.h3,
                        )),
                        ip6.port,
                    ))
                }
            }
        }
    }
}

impl From<SocketAddr> for WasiAddr {
    fn from(value: SocketAddr) -> Self {
        match value {
            SocketAddr::V4(addr) => Self {
                kind: AddrType::Ipv4,
                addr: WasiAddrPayload {
                    ip4: WasiAddrIp4Port {
                        addr: WasiAddrIp4 {
                            n0: addr.ip().octets()[0],
                            n1: addr.ip().octets()[1],
                            n2: addr.ip().octets()[2],
                            n3: addr.ip().octets()[3],
                        },
                        port: addr.port(),
                    },
                },
            },
            SocketAddr::V6(addr) => {
                let segments = addr.ip().segments();
                Self {
                    kind: AddrType::Ipv6,
                    addr: WasiAddrPayload {
                        ip6: WasiAddrIp6Port {
                            addr: WasiAddrIp6 {
                                n0: segments[0],
                                n1: segments[1],
                                n2: segments[2],
                                n3: segments[3],
                                h0: segments[4],
                                h1: segments[5],
                                h2: segments[6],
                                h3: segments[7],
                            },
                            port: addr.port(),
                        },
                    },
                }
            }
        }
    }
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct WasiAddrInfo {
    pub addr: WasiAddr,
    pub socket_type: SocketType,
}

impl Default for WasiAddrInfo {
    fn default() -> Self {
        Self {
            addr: WasiAddr::default(),
            socket_type: SocketType::Any,
        }
    }
}

impl WasiAddrInfo {
    pub fn to_socket_addr(&self) -> Option<SocketAddr> {
        self.addr.to_socket_addr()
    }
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct WasiAddrInfoHints {
    pub socket_type: SocketType,
    pub family: AddressFamily,
    pub hints_enabled: u8,
}

impl WasiAddrInfoHints {
    pub const fn new(socket_type: SocketType, family: AddressFamily) -> Self {
        Self {
            socket_type,
            family,
            hints_enabled: 1,
        }
    }
}

impl Default for WasiAddrInfoHints {
    fn default() -> Self {
        Self {
            socket_type: SocketType::Any,
            family: AddressFamily::Unspec,
            hints_enabled: 0,
        }
    }
}