use std::{
    io::{self, Read, Write},
    net::{Ipv4Addr, Shutdown, SocketAddr, SocketAddrV4, TcpStream},
    time::Duration,
};

const MAX_FRAME: usize = u8::MAX as usize;

pub struct Proxy {
    stream: Option<TcpStream>,
    timeout: Duration,
}

trait TimeoutStream: Read + Write {
    fn set_read_timeout(&self, timeout: Option<Duration>) -> io::Result<()>;
    fn set_write_timeout(&self, timeout: Option<Duration>) -> io::Result<()>;
}

impl TimeoutStream for TcpStream {
    fn set_read_timeout(&self, timeout: Option<Duration>) -> io::Result<()> {
        TcpStream::set_read_timeout(self, timeout)
    }

    fn set_write_timeout(&self, timeout: Option<Duration>) -> io::Result<()> {
        TcpStream::set_write_timeout(self, timeout)
    }
}

impl Proxy {
    pub fn connect(host: Ipv4Addr, port: u16, timeout: Duration) -> io::Result<Self> {
        if port == 0 || timeout.is_zero() {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "port and timeout must be nonzero",
            ));
        }
        let address = SocketAddr::V4(SocketAddrV4::new(host, port));
        let stream = TcpStream::connect_timeout(&address, timeout)?;
        stream.set_nodelay(true)?;
        Ok(Self {
            stream: Some(stream),
            timeout,
        })
    }

    /// Send one BonCasLink length-prefixed APDU and read its matching response.
    pub fn transmit(&mut self, apdu: &[u8]) -> io::Result<Vec<u8>> {
        if apdu.is_empty() || apdu.len() > MAX_FRAME {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "APDU length must be between 1 and 255 bytes",
            ));
        }
        let result = self.transmit_frame(apdu, self.timeout);
        if result.is_err() {
            self.poison();
        }
        result
    }

    fn transmit_frame(&mut self, apdu: &[u8], timeout: Duration) -> io::Result<Vec<u8>> {
        let stream = self.stream.as_mut().ok_or_else(|| {
            io::Error::new(
                io::ErrorKind::NotConnected,
                "BonCasLink connection is closed",
            )
        })?;
        let mut request = Vec::with_capacity(apdu.len() + 1);
        request.push(apdu.len() as u8);
        request.extend_from_slice(apdu);
        write_until(stream, &request, timeout)?;

        receive_response(stream, timeout)
    }

    fn poison(&mut self) {
        if let Some(stream) = self.stream.take() {
            let _ = stream.shutdown(Shutdown::Both);
        }
    }
}

fn receive_response<S: TimeoutStream>(stream: &mut S, timeout: Duration) -> io::Result<Vec<u8>> {
    let mut length = [0u8; 1];
    read_until(stream, &mut length, timeout)?;
    let mut response = vec![0u8; length[0] as usize];
    if response.is_empty() {
        return Err(io::Error::new(
            io::ErrorKind::TimedOut,
            "BonCasLink response frame is empty",
        ));
    }
    read_until(stream, &mut response, timeout)?;
    Ok(response)
}

fn write_until<S: TimeoutStream>(
    stream: &mut S,
    bytes: &[u8],
    timeout: Duration,
) -> io::Result<()> {
    let mut offset = 0;
    while offset < bytes.len() {
        stream.set_write_timeout(Some(timeout))?;
        match stream.write(&bytes[offset..]) {
            Ok(0) => return Err(io::ErrorKind::WriteZero.into()),
            Ok(written) => offset += written,
            Err(error) if error.kind() == io::ErrorKind::Interrupted => {}
            Err(error) => return Err(error),
        }
    }
    Ok(())
}

fn read_until<S: TimeoutStream>(
    stream: &mut S,
    bytes: &mut [u8],
    timeout: Duration,
) -> io::Result<()> {
    let mut offset = 0;
    while offset < bytes.len() {
        stream.set_read_timeout(Some(timeout))?;
        match stream.read(&mut bytes[offset..]) {
            Ok(0) => return Err(io::ErrorKind::UnexpectedEof.into()),
            Ok(read) => offset += read,
            Err(error) if error.kind() == io::ErrorKind::Interrupted => {}
            Err(error) => return Err(error),
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::{receive_response, TimeoutStream};
    use std::{
        cell::RefCell,
        collections::VecDeque,
        io::{self, Read, Write},
        time::Duration,
    };

    struct TimedResponse {
        response: VecDeque<u8>,
        timeout: Duration,
        applied_timeouts: RefCell<Vec<Option<Duration>>>,
        simulated_elapsed: Duration,
    }

    impl TimeoutStream for TimedResponse {
        fn set_read_timeout(&self, timeout: Option<Duration>) -> io::Result<()> {
            self.applied_timeouts.borrow_mut().push(timeout);
            Ok(())
        }

        fn set_write_timeout(&self, _: Option<Duration>) -> io::Result<()> {
            Ok(())
        }
    }

    impl Read for TimedResponse {
        fn read(&mut self, bytes: &mut [u8]) -> io::Result<usize> {
            let length = bytes.len().min(self.response.len());
            for byte in bytes.iter_mut().take(length) {
                *byte = self.response.pop_front().unwrap();
            }
            self.simulated_elapsed += Duration::from_millis(75);
            Ok(length)
        }
    }

    impl Write for TimedResponse {
        fn write(&mut self, bytes: &[u8]) -> io::Result<usize> {
            Ok(bytes.len())
        }

        fn flush(&mut self) -> io::Result<()> {
            Ok(())
        }
    }

    #[test]
    fn response_frame_uses_a_fresh_timeout_for_each_read() {
        let timeout = Duration::from_millis(100);
        let mut stream = TimedResponse {
            response: VecDeque::from([2, 0x90, 0x00]),
            timeout,
            applied_timeouts: RefCell::new(Vec::new()),
            simulated_elapsed: Duration::ZERO,
        };

        assert_eq!(
            receive_response(&mut stream, timeout).unwrap(),
            [0x90, 0x00]
        );
        assert!(stream.simulated_elapsed > stream.timeout);
        assert_eq!(
            *stream.applied_timeouts.borrow(),
            [Some(timeout), Some(timeout)]
        );
    }
}
