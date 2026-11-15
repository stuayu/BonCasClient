use encoding_rs::SHIFT_JIS;
use std::net::Ipv4Addr;

const DEFAULT_READER: &str = "BonCasLink Client Card Reader";
const DEFAULT_HOST: Ipv4Addr = Ipv4Addr::new(127, 0, 0, 1);
const DEFAULT_PORT: u16 = 6900;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Reader {
    pub name: String,
    pub host: Ipv4Addr,
    pub port: u16,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ClientConfig {
    pub server_host: Ipv4Addr,
    pub server_port: u16,
    pub readers: Vec<Reader>,
    pub explicit_readers: bool,
    pub local_enabled: bool,
}

impl Default for ClientConfig {
    fn default() -> Self {
        Self {
            server_host: DEFAULT_HOST,
            server_port: DEFAULT_PORT,
            readers: vec![Reader {
                name: DEFAULT_READER.to_owned(),
                host: DEFAULT_HOST,
                port: DEFAULT_PORT,
            }],
            explicit_readers: false,
            local_enabled: false,
        }
    }
}

impl ClientConfig {
    /// Parse the DLL's INI settings while preserving the legacy single-reader fallback.
    pub fn parse_ini(input: &str) -> Self {
        let mut config = Self::default();
        let mut server_host = None;
        let mut server_port = None;
        let mut reader_lines = Vec::new();
        let mut has_readers_section = false;
        let mut local_enabled = false;
        let mut server_host_seen = false;
        let mut server_port_seen = false;
        let mut local_enable_seen = false;
        let mut server_section_seen = false;
        let mut readers_section_seen = false;
        let mut local_section_seen = false;
        let mut in_server_section = false;
        let mut in_readers_section = false;
        let mut in_local_section = false;

        for raw in input.trim_start_matches('\u{feff}').lines() {
            let line = raw.trim();
            if line.is_empty() || line.starts_with(';') || line.starts_with('#') {
                continue;
            }
            if let Some(section_name) = parse_section_header(line) {
                in_server_section = false;
                in_readers_section = false;
                in_local_section = false;
                if section_name.eq_ignore_ascii_case("Server") && !server_section_seen {
                    server_section_seen = true;
                    in_server_section = true;
                }
                if section_name.eq_ignore_ascii_case("Readers") {
                    has_readers_section = true;
                    if !readers_section_seen {
                        readers_section_seen = true;
                        in_readers_section = true;
                    }
                }
                if section_name.eq_ignore_ascii_case("Local") && !local_section_seen {
                    local_section_seen = true;
                    in_local_section = true;
                }
                continue;
            }
            let Some((key, value)) = line.split_once('=') else {
                continue;
            };
            let key = key.trim();
            let value = value.trim();
            if in_server_section {
                if key.eq_ignore_ascii_case("IP") && !server_host_seen {
                    server_host_seen = true;
                    server_host = parse_ipv4(profile_string(value));
                } else if key.eq_ignore_ascii_case("Port") && !server_port_seen {
                    server_port_seen = true;
                    server_port = parse_profile_int(profile_string(value))
                        .and_then(|port| u16::try_from(port).ok())
                        .filter(|port| *port != 0);
                }
            } else if in_readers_section {
                reader_lines.push((key.to_owned(), value.to_owned()));
            } else if in_local_section && key.eq_ignore_ascii_case("Enable") && !local_enable_seen {
                local_enable_seen = true;
                local_enabled = parse_profile_int(profile_string(value)) == Some(1);
            }
        }

        config.server_host = server_host.unwrap_or(DEFAULT_HOST);
        config.server_port = server_port.unwrap_or(DEFAULT_PORT);
        config.explicit_readers = has_readers_section;
        config.local_enabled = local_enabled;

        let mut readers = Vec::new();
        for (raw_name, raw_endpoint) in reader_lines {
            let name = raw_name.trim();
            if name.is_empty() || name.starts_with(';') || name.starts_with('#') {
                continue;
            }
            let Some((host, port)) = parse_endpoint(&raw_endpoint, config.server_host) else {
                continue;
            };
            if readers.iter().any(|reader: &Reader| reader.name == name) {
                continue;
            }
            readers.push(Reader {
                name: name.to_owned(),
                host,
                port,
            });
        }

        if !readers.is_empty() {
            config.readers = readers;
        } else {
            config.readers = vec![Reader {
                name: DEFAULT_READER.to_owned(),
                host: config.server_host,
                port: config.server_port,
            }];
        }
        config
    }

    /// Parse profile bytes as UTF-8 with BOM, UTF-16 with BOM, or legacy Windows-31J.
    pub fn parse_ini_bytes(input: &[u8]) -> Self {
        if let Some(input) = input.strip_prefix(&[0xEF, 0xBB, 0xBF]) {
            return Self::parse_ini(&String::from_utf8_lossy(input));
        }
        if let Some(input) = input.strip_prefix(&[0xFF, 0xFE]) {
            return Self::parse_ini(&decode_utf16(input, true));
        }
        if let Some(input) = input.strip_prefix(&[0xFE, 0xFF]) {
            return Self::parse_ini(&decode_utf16(input, false));
        }
        let (decoded, _, _) = SHIFT_JIS.decode(input);
        Self::parse_ini(&decoded)
    }

    /// Resolve a configured virtual reader, including the legacy single-reader fallback.
    pub fn reader(&self, name: Option<&str>) -> Option<&Reader> {
        match name {
            Some(name) => {
                let reader = self.readers.iter().find(|reader| reader.name == name);
                reader.or_else(|| {
                    if self.explicit_readers {
                        None
                    } else {
                        self.readers.first()
                    }
                })
            }
            None if !self.explicit_readers => self.readers.first(),
            None => None,
        }
    }
}

fn decode_utf16(input: &[u8], little_endian: bool) -> String {
    let words = input.chunks_exact(2).map(|pair| {
        let bytes = [pair[0], pair[1]];
        if little_endian {
            u16::from_le_bytes(bytes)
        } else {
            u16::from_be_bytes(bytes)
        }
    });
    char::decode_utf16(words)
        .map(|character| character.unwrap_or(char::REPLACEMENT_CHARACTER))
        .collect()
}

fn parse_endpoint(raw: &str, default_host: Ipv4Addr) -> Option<(Ipv4Addr, u16)> {
    let value = raw.trim();
    if let Some((host, port)) = value.split_once(':') {
        Some((parse_ipv4(host.trim())?, parse_port(port.trim())?))
    } else {
        Some((default_host, parse_port(value)?))
    }
}

fn parse_section_header(line: &str) -> Option<&str> {
    let inner = line.strip_prefix('[')?;
    let close = inner.find(']')?;
    Some(inner[..close].trim())
}

fn profile_string(value: &str) -> &str {
    let value = value.trim();
    let bytes = value.as_bytes();
    if bytes.len() >= 2
        && ((bytes[0] == b'"' && bytes[bytes.len() - 1] == b'"')
            || (bytes[0] == b'\'' && bytes[bytes.len() - 1] == b'\''))
    {
        &value[1..value.len() - 1]
    } else {
        value
    }
}

fn parse_port(value: &str) -> Option<u16> {
    if value.is_empty() || !value.bytes().all(|byte| byte.is_ascii_digit()) {
        return None;
    }
    value.parse::<u16>().ok().filter(|port| *port != 0)
}

fn parse_profile_int(value: &str) -> Option<i128> {
    let value = value.trim_start();
    let (negative, digits) = match value.as_bytes().first() {
        Some(b'-') => (true, &value[1..]),
        Some(b'+') => (false, &value[1..]),
        _ => (false, value),
    };
    let digits_end = digits
        .bytes()
        .position(|byte| !byte.is_ascii_digit())
        .unwrap_or(digits.len());
    if digits_end == 0 {
        return None;
    }
    let magnitude = digits[..digits_end].parse::<i128>().ok()?;
    Some(if negative { -magnitude } else { magnitude })
}

fn parse_ipv4(value: &str) -> Option<Ipv4Addr> {
    let mut octets = [0u8; 4];
    let mut parts = value.split('.');
    for octet in &mut octets {
        let part = parts.next()?;
        if part.is_empty() || !part.bytes().all(|byte| byte.is_ascii_digit()) {
            return None;
        }
        *octet = part.parse::<u8>().ok()?;
    }
    if parts.next().is_some() {
        return None;
    }
    Some(Ipv4Addr::from(octets))
}
