use core::net::SocketAddr;
use core::time::Duration;

use crate::addr::{AddressFamily, SocketType};
use crate::error::{Error, Result};
use crate::sys;

#[derive(Debug)]
pub struct TcpListener {
    fd: u32,
}

impl TcpListener {
    pub fn bind(addr: SocketAddr) -> Result<Self> {
        let fd = sys::sock_open(sys::address_family_for(addr), SocketType::Stream)?;

        if let Err(error) = sys::sock_set_reuse_addr(fd, true) {
            if error.code() != 58 && error.code() != 50 && error.code() != 66 {
                let _ = sys::sock_close(fd);
                return Err(error);
            }
        }

        if let Err(error) = sys::sock_bind(fd, addr) {
            let _ = sys::sock_close(fd);
            return Err(error.into());
        }

        if let Err(error) = sys::sock_listen(fd, 8) {
            let _ = sys::sock_close(fd);
            return Err(error.into());
        }

        Ok(Self { fd })
    }

    pub fn accept(&self) -> Result<(TcpStream, SocketAddr)> {
        let fd = sys::sock_accept(self.fd, 0)?;
        let peer = sys::sock_addr_remote(fd)?;
        Ok((TcpStream { fd }, peer))
    }

    pub fn local_addr(&self) -> Result<SocketAddr> {
        Ok(sys::sock_addr_local(self.fd)?)
    }

    pub fn set_reuse_addr(&self, enabled: bool) -> Result<()> {
        sys::sock_set_reuse_addr(self.fd, enabled)
    }
}

impl Drop for TcpListener {
    fn drop(&mut self) {
        let _ = sys::sock_close(self.fd);
    }
}

#[derive(Debug)]
pub struct TcpStream {
    fd: u32,
}

impl TcpStream {
    pub fn connect(addr: SocketAddr) -> Result<Self> {
        let fd = sys::sock_open(sys::address_family_for(addr), SocketType::Stream)?;

        if let Err(error) = sys::sock_connect(fd, addr) {
            let _ = sys::sock_close(fd);
            return Err(error.into());
        }

        Ok(Self { fd })
    }

    pub fn connect_host(host: &str, port: u16) -> Result<Self> {
        let addresses = sys::resolve_socket_addrs(host, port, SocketType::Stream)?;
        let mut last_error = None;

        for addr in addresses {
            match Self::connect(addr) {
                Ok(stream) => return Ok(stream),
                Err(error) => last_error = Some(error),
            }
        }

        Err(last_error.unwrap_or_else(|| Error::new(44)))
    }

    pub fn local_addr(&self) -> Result<SocketAddr> {
        Ok(sys::sock_addr_local(self.fd)?)
    }

    pub fn peer_addr(&self) -> Result<SocketAddr> {
        Ok(sys::sock_addr_remote(self.fd)?)
    }

    pub fn set_nodelay(&self, enabled: bool) -> Result<()> {
        sys::sock_set_tcp_no_delay(self.fd, enabled)?;
        Ok(())
    }

    pub fn nodelay(&self) -> Result<bool> {
        Ok(sys::sock_get_tcp_no_delay(self.fd)?)
    }

    pub fn set_keepalive(&self, keepalive: Option<Duration>) -> Result<()> {
        if let Some(timeout) = keepalive {
            sys::sock_set_keep_alive(self.fd, true)?;
            sys::sock_set_tcp_keep_idle(self.fd, timeout)?;
            sys::sock_set_tcp_keep_intvl(self.fd, timeout)?;
        }
        else {
            sys::sock_set_keep_alive(self.fd, false)?;
        }
        Ok(())
    }

    pub fn set_read_timeout(&self, timeout: Option<Duration>) -> Result<()> {
        sys::sock_set_read_timeout(self.fd, timeout)?;
        Ok(())
    }

    pub fn set_write_timeout(&self, timeout: Option<Duration>) -> Result<()> {
        sys::sock_set_write_timeout(self.fd, timeout)?;
        Ok(())
    }

    pub fn set_recv_buffer_size(&self, size: usize) -> Result<()> {
        sys::sock_set_recv_buf_size(self.fd, size)?;
        Ok(())
    }

    pub fn recv_buffer_size(&self) -> Result<usize> {
        Ok(sys::sock_get_recv_buf_size(self.fd)?)
    }

    pub fn set_send_buffer_size(&self, size: usize) -> Result<()> {
        sys::sock_set_send_buf_size(self.fd, size)?;
        Ok(())
    }

    pub fn send_buffer_size(&self) -> Result<usize> {
        Ok(sys::sock_get_send_buf_size(self.fd)?)
    }

    pub fn read(&mut self, buf: &mut [u8]) -> Result<usize> {
        Ok(sys::sock_recv(self.fd, buf, 0)?.0)
    }

    pub fn write(&mut self, buf: &[u8]) -> Result<usize> {
        sys::sock_send(self.fd, buf, 0)
    }

    pub fn write_all(&mut self, mut buf: &[u8]) -> Result<()> {
        while !buf.is_empty() {
            let written = self.write(buf)?;
            if written == 0 {
                return Err(Error::new(64));
            }
            buf = &buf[written..];
        }

        Ok(())
    }

    pub fn connect_unspecified(port: u16) -> Result<Self> {
        Self::connect(SocketAddr::from(([0, 0, 0, 0], port)))
    }

    pub fn connect_loopback(port: u16) -> Result<Self> {
        Self::connect(SocketAddr::from(([127, 0, 0, 1], port)))
    }

    pub fn connect_family(host: &str, port: u16, family: AddressFamily) -> Result<Self> {
        let addresses = sys::resolve_socket_addrs(host, port, SocketType::Stream)?;

        for addr in addresses {
            if sys::address_family_for(addr) != family {
                continue;
            }

            if let Ok(stream) = Self::connect(addr) {
                return Ok(stream);
            }
        }

        Err(Error::new(44))
    }

    pub fn flush(&mut self) -> Result<()> {
        Ok(())
    }
}

impl Drop for TcpStream {
    fn drop(&mut self) {
        let _ = sys::sock_close(self.fd);
    }
}