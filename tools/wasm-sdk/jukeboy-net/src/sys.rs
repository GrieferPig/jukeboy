use std::ffi::CString;
use std::io;
use std::net::{IpAddr, SocketAddr};
use std::os::raw::c_char;
use std::time::Duration;

use crate::addr::{AddressFamily, SocketType, WasiAddr, WasiAddrInfo, WasiAddrInfoHints, ANY_ADDRESS_POOL_FD};
use crate::error::{check, Error, Result};

#[repr(C)]
#[derive(Clone, Copy)]
pub struct Ciovec {
    pub buf: *const u8,
    pub buf_len: usize,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct Iovec {
    pub buf: *mut u8,
    pub buf_len: usize,
}

#[link(wasm_import_module = "wasi_snapshot_preview1")]
unsafe extern "C" {
    #[link_name = "sock_open"]
    fn raw_sock_open(pool_fd: i32, family: i32, socket_type: i32, fd_out: *mut u32) -> u16;

    #[link_name = "sock_close"]
    fn raw_sock_close(fd: i32) -> u16;

    #[link_name = "sock_bind"]
    fn raw_sock_bind(fd: i32, addr: *const WasiAddr) -> u16;

    #[link_name = "sock_listen"]
    fn raw_sock_listen(fd: i32, backlog: i32) -> u16;

    #[link_name = "sock_accept"]
    fn raw_sock_accept(fd: i32, flags: i32, fd_out: *mut u32) -> u16;

    #[link_name = "sock_connect"]
    fn raw_sock_connect(fd: i32, addr: *const WasiAddr) -> u16;

    #[link_name = "sock_addr_local"]
    fn raw_sock_addr_local(fd: i32, addr: *mut WasiAddr) -> u16;

    #[link_name = "sock_addr_remote"]
    fn raw_sock_addr_remote(fd: i32, addr: *mut WasiAddr) -> u16;

    #[link_name = "sock_addr_resolve"]
    fn raw_sock_addr_resolve(
        host: *const c_char,
        service: *const c_char,
        hints: *const WasiAddrInfoHints,
        addr_info: *mut WasiAddrInfo,
        addr_info_len: u32,
        addr_info_count: *mut u32,
    ) -> u16;

    #[link_name = "sock_recv"]
    fn raw_sock_recv(
        fd: i32,
        iovs: *mut Iovec,
        iovs_len: u32,
        flags: i32,
        data_len_out: *mut u32,
        ro_flags_out: *mut u16,
    ) -> u16;

    #[link_name = "sock_send"]
    fn raw_sock_send(
        fd: i32,
        iovs: *const Ciovec,
        iovs_len: u32,
        flags: i32,
        data_len_out: *mut u32,
    ) -> u16;

    #[link_name = "sock_recv_from"]
    fn raw_sock_recv_from(
        fd: i32,
        iovs: *mut Iovec,
        iovs_len: u32,
        flags: i32,
        addr_out: *mut WasiAddr,
        data_len_out: *mut u32,
    ) -> u16;

    #[link_name = "sock_send_to"]
    fn raw_sock_send_to(
        fd: i32,
        iovs: *const Ciovec,
        iovs_len: u32,
        flags: i32,
        addr: *const WasiAddr,
        data_len_out: *mut u32,
    ) -> u16;

    #[link_name = "sock_set_reuse_addr"]
    fn raw_sock_set_reuse_addr(fd: i32, enabled: i32) -> u16;

    #[link_name = "sock_get_reuse_addr"]
    fn raw_sock_get_reuse_addr(fd: i32, enabled_out: *mut i32) -> u16;

    #[link_name = "sock_set_broadcast"]
    fn raw_sock_set_broadcast(fd: i32, enabled: i32) -> u16;

    #[link_name = "sock_get_broadcast"]
    fn raw_sock_get_broadcast(fd: i32, enabled_out: *mut i32) -> u16;

    #[link_name = "sock_set_recv_timeout"]
    fn raw_sock_set_recv_timeout(fd: i32, timeout_us: i64) -> u16;

    #[link_name = "sock_set_send_timeout"]
    fn raw_sock_set_send_timeout(fd: i32, timeout_us: i64) -> u16;

    #[link_name = "sock_set_recv_buf_size"]
    fn raw_sock_set_recv_buf_size(fd: i32, size: i32) -> u16;

    #[link_name = "sock_get_recv_buf_size"]
    fn raw_sock_get_recv_buf_size(fd: i32, size_out: *mut u32) -> u16;

    #[link_name = "sock_set_send_buf_size"]
    fn raw_sock_set_send_buf_size(fd: i32, size: i32) -> u16;

    #[link_name = "sock_get_send_buf_size"]
    fn raw_sock_get_send_buf_size(fd: i32, size_out: *mut u32) -> u16;

    #[link_name = "sock_set_keep_alive"]
    fn raw_sock_set_keep_alive(fd: i32, enabled: i32) -> u16;

    #[link_name = "sock_get_keep_alive"]
    fn raw_sock_get_keep_alive(fd: i32, enabled_out: *mut i32) -> u16;

    #[link_name = "sock_set_tcp_keep_idle"]
    fn raw_sock_set_tcp_keep_idle(fd: i32, seconds: i32) -> u16;

    #[link_name = "sock_set_tcp_keep_intvl"]
    fn raw_sock_set_tcp_keep_intvl(fd: i32, seconds: i32) -> u16;

    #[link_name = "sock_set_tcp_no_delay"]
    fn raw_sock_set_tcp_no_delay(fd: i32, enabled: i32) -> u16;

    #[link_name = "sock_get_tcp_no_delay"]
    fn raw_sock_get_tcp_no_delay(fd: i32, enabled_out: *mut i32) -> u16;
}

fn bool_to_raw(value: bool) -> i32 {
    if value { 1 } else { 0 }
}

fn raw_to_bool(value: i32) -> bool {
    value != 0
}

fn timeout_to_micros(timeout: Option<Duration>) -> i64 {
    timeout
        .map(|value| value.as_micros().min(i64::MAX as u128) as i64)
        .unwrap_or(0)
}

fn addr_from_query(query: &WasiAddr) -> Result<SocketAddr> {
    query.to_socket_addr().ok_or_else(|| Error::new(28))
}

pub fn address_family_for(addr: SocketAddr) -> AddressFamily {
    match addr {
        SocketAddr::V4(_) => AddressFamily::Inet4,
        SocketAddr::V6(_) => AddressFamily::Inet6,
    }
}

pub fn sock_open(family: AddressFamily, socket_type: SocketType) -> Result<u32> {
    let mut fd = 0_u32;
    check(unsafe { raw_sock_open(ANY_ADDRESS_POOL_FD, family as i32, socket_type as i32, &mut fd) })?;
    Ok(fd)
}

pub fn sock_close(fd: u32) -> Result<()> {
    check(unsafe { raw_sock_close(fd as i32) })
}

pub fn sock_bind(fd: u32, addr: SocketAddr) -> Result<()> {
    let addr = WasiAddr::from(addr);
    check(unsafe { raw_sock_bind(fd as i32, &addr) })
}

pub fn sock_listen(fd: u32, backlog: u32) -> Result<()> {
    check(unsafe { raw_sock_listen(fd as i32, backlog.min(i32::MAX as u32) as i32) })
}

pub fn sock_accept(fd: u32, flags: u16) -> Result<u32> {
    let mut new_fd = 0_u32;
    check(unsafe { raw_sock_accept(fd as i32, i32::from(flags), &mut new_fd) })?;
    Ok(new_fd)
}

pub fn sock_connect(fd: u32, addr: SocketAddr) -> Result<()> {
    let addr = WasiAddr::from(addr);
    check(unsafe { raw_sock_connect(fd as i32, &addr) })
}

pub fn sock_addr_local(fd: u32) -> Result<SocketAddr> {
    let mut addr = WasiAddr::default();
    check(unsafe { raw_sock_addr_local(fd as i32, &mut addr) })?;
    addr_from_query(&addr)
}

pub fn sock_addr_remote(fd: u32) -> Result<SocketAddr> {
    let mut addr = WasiAddr::default();
    check(unsafe { raw_sock_addr_remote(fd as i32, &mut addr) })?;
    addr_from_query(&addr)
}

pub fn sock_addr_resolve(host: &str, service: Option<&str>, hints: Option<WasiAddrInfoHints>) -> Result<Vec<WasiAddrInfo>> {
    let host = CString::new(host).map_err(|_| Error::new(28))?;
    let service = CString::new(service.unwrap_or("")).map_err(|_| Error::new(28))?;
    let hints_value = hints.unwrap_or_default();
    let hints_ptr = if hints.is_some() { &hints_value as *const _ } else { std::ptr::null() };
    let mut info = vec![WasiAddrInfo::default(); 8];
    let mut count = 0_u32;

    check(unsafe {
        raw_sock_addr_resolve(
            host.as_ptr(),
            service.as_ptr(),
            hints_ptr,
            info.as_mut_ptr(),
            info.len() as u32,
            &mut count,
        )
    })?;

    info.truncate((count as usize).min(info.len()));
    Ok(info)
}

pub fn resolve_socket_addrs(host: &str, port: u16, socket_type: SocketType) -> Result<Vec<SocketAddr>> {
    if let Ok(ip_addr) = host.parse::<IpAddr>() {
        return Ok(vec![SocketAddr::new(ip_addr, port)]);
    }

    let service = port.to_string();
    let hints = WasiAddrInfoHints::new(socket_type, AddressFamily::Unspec);
    let mut results = Vec::new();

    for entry in sock_addr_resolve(host, Some(&service), Some(hints))? {
        if let Some(addr) = entry.to_socket_addr() {
            results.push(addr);
        }
    }

    if results.is_empty() {
        Err(Error::new(44))
    }
    else {
        Ok(results)
    }
}

pub fn sock_send(fd: u32, buffer: &[u8], flags: u16) -> Result<usize> {
    let iovec = Ciovec {
        buf: buffer.as_ptr(),
        buf_len: buffer.len(),
    };
    let mut sent = 0_u32;

    check(unsafe { raw_sock_send(fd as i32, &iovec, 1, i32::from(flags), &mut sent) })?;
    Ok(sent as usize)
}

pub fn sock_recv(fd: u32, buffer: &mut [u8], flags: u16) -> Result<(usize, u16)> {
    let mut iovec = Iovec {
        buf: buffer.as_mut_ptr(),
        buf_len: buffer.len(),
    };
    let mut received = 0_u32;
    let mut ro_flags = 0_u16;

    check(unsafe {
        raw_sock_recv(
            fd as i32,
            &mut iovec,
            1,
            i32::from(flags),
            &mut received,
            &mut ro_flags,
        )
    })?;

    Ok((received as usize, ro_flags))
}

pub fn sock_send_to(fd: u32, buffer: &[u8], flags: u16, addr: SocketAddr) -> Result<usize> {
    let iovec = Ciovec {
        buf: buffer.as_ptr(),
        buf_len: buffer.len(),
    };
    let addr = WasiAddr::from(addr);
    let mut sent = 0_u32;

    check(unsafe {
        raw_sock_send_to(
            fd as i32,
            &iovec,
            1,
            i32::from(flags),
            &addr,
            &mut sent,
        )
    })?;

    Ok(sent as usize)
}

pub fn sock_recv_from(fd: u32, buffer: &mut [u8], flags: u16) -> Result<(usize, SocketAddr)> {
    let mut iovec = Iovec {
        buf: buffer.as_mut_ptr(),
        buf_len: buffer.len(),
    };
    let mut addr = WasiAddr::default();
    let mut received = 0_u32;

    check(unsafe {
        raw_sock_recv_from(
            fd as i32,
            &mut iovec,
            1,
            i32::from(flags),
            &mut addr,
            &mut received,
        )
    })?;

    Ok((received as usize, addr_from_query(&addr)?))
}

pub fn sock_set_reuse_addr(fd: u32, enabled: bool) -> Result<()> {
    check(unsafe { raw_sock_set_reuse_addr(fd as i32, bool_to_raw(enabled)) })
}

pub fn sock_get_reuse_addr(fd: u32) -> Result<bool> {
    let mut enabled = 0_i32;
    check(unsafe { raw_sock_get_reuse_addr(fd as i32, &mut enabled) })?;
    Ok(raw_to_bool(enabled))
}

pub fn sock_set_broadcast(fd: u32, enabled: bool) -> Result<()> {
    check(unsafe { raw_sock_set_broadcast(fd as i32, bool_to_raw(enabled)) })
}

pub fn sock_get_broadcast(fd: u32) -> Result<bool> {
    let mut enabled = 0_i32;
    check(unsafe { raw_sock_get_broadcast(fd as i32, &mut enabled) })?;
    Ok(raw_to_bool(enabled))
}

pub fn sock_set_read_timeout(fd: u32, timeout: Option<Duration>) -> Result<()> {
    check(unsafe { raw_sock_set_recv_timeout(fd as i32, timeout_to_micros(timeout)) })
}

pub fn sock_set_write_timeout(fd: u32, timeout: Option<Duration>) -> Result<()> {
    check(unsafe { raw_sock_set_send_timeout(fd as i32, timeout_to_micros(timeout)) })
}

pub fn sock_set_recv_buf_size(fd: u32, size: usize) -> Result<()> {
    check(unsafe { raw_sock_set_recv_buf_size(fd as i32, size.min(i32::MAX as usize) as i32) })
}

pub fn sock_get_recv_buf_size(fd: u32) -> Result<usize> {
    let mut size = 0_u32;
    check(unsafe { raw_sock_get_recv_buf_size(fd as i32, &mut size) })?;
    Ok(size as usize)
}

pub fn sock_set_send_buf_size(fd: u32, size: usize) -> Result<()> {
    check(unsafe { raw_sock_set_send_buf_size(fd as i32, size.min(i32::MAX as usize) as i32) })
}

pub fn sock_get_send_buf_size(fd: u32) -> Result<usize> {
    let mut size = 0_u32;
    check(unsafe { raw_sock_get_send_buf_size(fd as i32, &mut size) })?;
    Ok(size as usize)
}

pub fn sock_set_keep_alive(fd: u32, enabled: bool) -> Result<()> {
    check(unsafe { raw_sock_set_keep_alive(fd as i32, bool_to_raw(enabled)) })
}

pub fn sock_get_keep_alive(fd: u32) -> Result<bool> {
    let mut enabled = 0_i32;
    check(unsafe { raw_sock_get_keep_alive(fd as i32, &mut enabled) })?;
    Ok(raw_to_bool(enabled))
}

pub fn sock_set_tcp_keep_idle(fd: u32, timeout: Duration) -> Result<()> {
    check(unsafe { raw_sock_set_tcp_keep_idle(fd as i32, timeout.as_secs().min(i32::MAX as u64) as i32) })
}

pub fn sock_set_tcp_keep_intvl(fd: u32, timeout: Duration) -> Result<()> {
    check(unsafe { raw_sock_set_tcp_keep_intvl(fd as i32, timeout.as_secs().min(i32::MAX as u64) as i32) })
}

pub fn sock_set_tcp_no_delay(fd: u32, enabled: bool) -> Result<()> {
    check(unsafe { raw_sock_set_tcp_no_delay(fd as i32, bool_to_raw(enabled)) })
}

pub fn sock_get_tcp_no_delay(fd: u32) -> Result<bool> {
    let mut enabled = 0_i32;
    check(unsafe { raw_sock_get_tcp_no_delay(fd as i32, &mut enabled) })?;
    Ok(raw_to_bool(enabled))
}

pub fn invalid_input(message: &str) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidInput, message)
}