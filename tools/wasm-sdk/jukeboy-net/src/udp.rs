use std::io;
use std::net::SocketAddr;
use std::time::Duration;

use crate::addr::SocketType;
use crate::sys;

#[derive(Debug)]
pub struct UdpSocket {
    fd: u32,
}

impl UdpSocket {
    pub fn bind(addr: SocketAddr) -> io::Result<Self> {
        let fd = sys::sock_open(sys::address_family_for(addr), SocketType::Dgram)?;

        if let Err(error) = sys::sock_set_reuse_addr(fd, true) {
            let _ = sys::sock_close(fd);
            return Err(error.into());
        }

        if let Err(error) = sys::sock_bind(fd, addr) {
            let _ = sys::sock_close(fd);
            return Err(error.into());
        }

        Ok(Self { fd })
    }

    pub fn connect(&self, addr: SocketAddr) -> io::Result<()> {
        sys::sock_connect(self.fd, addr)?;
        Ok(())
    }

    pub fn send(&self, buf: &[u8]) -> io::Result<usize> {
        sys::sock_send(self.fd, buf, 0).map_err(Into::into)
    }

    pub fn recv(&self, buf: &mut [u8]) -> io::Result<usize> {
        Ok(sys::sock_recv(self.fd, buf, 0)?.0)
    }

    pub fn send_to(&self, buf: &[u8], addr: SocketAddr) -> io::Result<usize> {
        sys::sock_send_to(self.fd, buf, 0, addr).map_err(Into::into)
    }

    pub fn recv_from(&self, buf: &mut [u8]) -> io::Result<(usize, SocketAddr)> {
        sys::sock_recv_from(self.fd, buf, 0).map_err(Into::into)
    }

    pub fn local_addr(&self) -> io::Result<SocketAddr> {
        Ok(sys::sock_addr_local(self.fd)?)
    }

    pub fn peer_addr(&self) -> io::Result<SocketAddr> {
        Ok(sys::sock_addr_remote(self.fd)?)
    }

    pub fn set_broadcast(&self, enabled: bool) -> io::Result<()> {
        sys::sock_set_broadcast(self.fd, enabled)?;
        Ok(())
    }

    pub fn broadcast(&self) -> io::Result<bool> {
        Ok(sys::sock_get_broadcast(self.fd)?)
    }

    pub fn set_read_timeout(&self, timeout: Option<Duration>) -> io::Result<()> {
        sys::sock_set_read_timeout(self.fd, timeout)?;
        Ok(())
    }

    pub fn set_write_timeout(&self, timeout: Option<Duration>) -> io::Result<()> {
        sys::sock_set_write_timeout(self.fd, timeout)?;
        Ok(())
    }
}

impl Drop for UdpSocket {
    fn drop(&mut self) {
        let _ = sys::sock_close(self.fd);
    }
}