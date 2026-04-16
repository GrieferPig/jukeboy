use std::io::{self, Read, Write};
use std::net::SocketAddr;
use std::time::Duration;

use crate::addr::SocketType;
use crate::sys;

#[derive(Debug)]
pub struct TcpListener {
    fd: u32,
}

impl TcpListener {
    pub fn bind(addr: SocketAddr) -> io::Result<Self> {
        let fd = sys::sock_open(sys::address_family_for(addr), SocketType::Stream)?;

        if let Err(error) = sys::sock_set_reuse_addr(fd, true) {
            let io_error: io::Error = error.into();
            if io_error.kind() != io::ErrorKind::Unsupported {
                let _ = sys::sock_close(fd);
                return Err(io_error);
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

    pub fn accept(&self) -> io::Result<(TcpStream, SocketAddr)> {
        let fd = sys::sock_accept(self.fd, 0)?;
        let peer = sys::sock_addr_remote(fd)?;
        Ok((TcpStream { fd }, peer))
    }

    pub fn local_addr(&self) -> io::Result<SocketAddr> {
        Ok(sys::sock_addr_local(self.fd)?)
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
    pub fn connect(addr: SocketAddr) -> io::Result<Self> {
        let fd = sys::sock_open(sys::address_family_for(addr), SocketType::Stream)?;

        if let Err(error) = sys::sock_connect(fd, addr) {
            let _ = sys::sock_close(fd);
            return Err(error.into());
        }

        Ok(Self { fd })
    }

    pub fn connect_host(host: &str, port: u16) -> io::Result<Self> {
        let addresses = sys::resolve_socket_addrs(host, port, SocketType::Stream)?;
        let mut last_error = None;

        for addr in addresses {
            match Self::connect(addr) {
                Ok(stream) => return Ok(stream),
                Err(error) => last_error = Some(error),
            }
        }

        Err(last_error.unwrap_or_else(|| sys::invalid_input("no socket addresses resolved")))
    }

    pub fn local_addr(&self) -> io::Result<SocketAddr> {
        Ok(sys::sock_addr_local(self.fd)?)
    }

    pub fn peer_addr(&self) -> io::Result<SocketAddr> {
        Ok(sys::sock_addr_remote(self.fd)?)
    }

    pub fn set_nodelay(&self, enabled: bool) -> io::Result<()> {
        sys::sock_set_tcp_no_delay(self.fd, enabled)?;
        Ok(())
    }

    pub fn nodelay(&self) -> io::Result<bool> {
        Ok(sys::sock_get_tcp_no_delay(self.fd)?)
    }

    pub fn set_keepalive(&self, keepalive: Option<Duration>) -> io::Result<()> {
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

    pub fn set_read_timeout(&self, timeout: Option<Duration>) -> io::Result<()> {
        sys::sock_set_read_timeout(self.fd, timeout)?;
        Ok(())
    }

    pub fn set_write_timeout(&self, timeout: Option<Duration>) -> io::Result<()> {
        sys::sock_set_write_timeout(self.fd, timeout)?;
        Ok(())
    }

    pub fn set_recv_buffer_size(&self, size: usize) -> io::Result<()> {
        sys::sock_set_recv_buf_size(self.fd, size)?;
        Ok(())
    }

    pub fn recv_buffer_size(&self) -> io::Result<usize> {
        Ok(sys::sock_get_recv_buf_size(self.fd)?)
    }

    pub fn set_send_buffer_size(&self, size: usize) -> io::Result<()> {
        sys::sock_set_send_buf_size(self.fd, size)?;
        Ok(())
    }

    pub fn send_buffer_size(&self) -> io::Result<usize> {
        Ok(sys::sock_get_send_buf_size(self.fd)?)
    }
}

impl Read for TcpStream {
    fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
        Ok(sys::sock_recv(self.fd, buf, 0)?.0)
    }
}

impl Write for TcpStream {
    fn write(&mut self, buf: &[u8]) -> io::Result<usize> {
        sys::sock_send(self.fd, buf, 0).map_err(Into::into)
    }

    fn flush(&mut self) -> io::Result<()> {
        Ok(())
    }
}

impl Drop for TcpStream {
    fn drop(&mut self) {
        let _ = sys::sock_close(self.fd);
    }
}