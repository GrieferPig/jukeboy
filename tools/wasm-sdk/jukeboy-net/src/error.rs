use std::fmt;
use std::io;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Error {
    code: u16,
}

impl Error {
    pub const fn new(code: u16) -> Self {
        Self { code }
    }

    pub const fn code(self) -> u16 {
        self.code
    }

    pub fn kind(self) -> io::ErrorKind {
        match self.code {
            2 | 63 | 76 => io::ErrorKind::PermissionDenied,
            3 => io::ErrorKind::AddrInUse,
            4 | 17 => io::ErrorKind::AddrNotAvailable,
            6 => io::ErrorKind::WouldBlock,
            7 | 26 => io::ErrorKind::WouldBlock,
            8 => io::ErrorKind::BrokenPipe,
            13 => io::ErrorKind::ConnectionAborted,
            14 => io::ErrorKind::ConnectionRefused,
            15 => io::ErrorKind::ConnectionReset,
            20 => io::ErrorKind::AlreadyExists,
            23 | 38 | 40 => io::ErrorKind::NotConnected,
            27 => io::ErrorKind::Interrupted,
            28 => io::ErrorKind::InvalidInput,
            29 => io::ErrorKind::Other,
            33 | 41 => io::ErrorKind::Other,
            35 => io::ErrorKind::InvalidData,
            37 => io::ErrorKind::InvalidInput,
            42 | 48 => io::ErrorKind::OutOfMemory,
            44 => io::ErrorKind::NotFound,
            50 | 58 | 66 => io::ErrorKind::Unsupported,
            51 => io::ErrorKind::StorageFull,
            53 | 57 => io::ErrorKind::NotConnected,
            61 => io::ErrorKind::InvalidData,
            64 => io::ErrorKind::BrokenPipe,
            68 => io::ErrorKind::InvalidInput,
            69 => io::ErrorKind::ReadOnlyFilesystem,
            73 => io::ErrorKind::TimedOut,
            _ => io::ErrorKind::Other,
        }
    }
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "WASI socket error {}", self.code)
    }
}

impl std::error::Error for Error {}

impl From<Error> for io::Error {
    fn from(value: Error) -> Self {
        io::Error::new(value.kind(), value)
    }
}

pub type Result<T> = std::result::Result<T, Error>;

pub fn check(code: u16) -> Result<()> {
    if code == 0 {
        Ok(())
    }
    else {
        Err(Error::new(code))
    }
}