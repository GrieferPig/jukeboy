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

    pub const fn message(self) -> &'static str {
        match self.code {
            2 | 63 => "permission denied",
            3 => "address in use",
            4 | 17 => "address not available",
            6 | 7 | 26 => "operation would block",
            8 | 64 => "broken pipe",
            13 => "connection aborted",
            14 => "connection refused",
            15 => "connection reset",
            20 => "already exists",
            23 | 38 | 40 | 53 | 57 => "not connected",
            27 => "interrupted",
            28 | 68 => "invalid input",
            35 => "message too large",
            42 | 48 => "out of memory",
            44 => "not found",
            50 | 58 | 66 => "unsupported",
            73 => "timed out",
            _ => "socket error",
        }
    }
}

impl ufmt::uDisplay for Error {
    fn fmt<W>(&self, formatter: &mut ufmt::Formatter<'_, W>) -> core::result::Result<(), W::Error>
    where
        W: ufmt::uWrite + ?Sized,
    {
        ufmt::uwrite!(formatter, "{} ({})", self.message(), self.code)
    }
}

pub type Result<T> = core::result::Result<T, Error>;

pub fn check(code: u16) -> Result<()> {
    if code == 0 {
        Ok(())
    }
    else {
        Err(Error::new(code))
    }
}