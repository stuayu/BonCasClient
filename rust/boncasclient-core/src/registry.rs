use crate::{ClientConfig, Proxy};
use std::{
    collections::{BTreeMap, BTreeSet},
    error::Error,
    fmt, io,
    net::Ipv4Addr,
    sync::{Arc, Mutex, MutexGuard},
    time::{Duration, Instant},
};

const CONNECT_BACKOFF: Duration = Duration::from_secs(1);

#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct ContextHandle(usize);

impl ContextHandle {
    pub const fn from_raw(raw: usize) -> Self {
        Self(raw)
    }

    pub const fn raw(self) -> usize {
        self.0
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct CardHandle(usize);

impl CardHandle {
    pub const fn from_raw(raw: usize) -> Self {
        Self(raw)
    }

    pub const fn raw(self) -> usize {
        self.0
    }
}

#[derive(Debug)]
pub enum RegistryError {
    InvalidContext,
    InvalidCard,
    UnknownReader,
    HandleSpaceExhausted,
    Io(io::Error),
}

impl fmt::Display for RegistryError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::InvalidContext => f.write_str("invalid smart-card context"),
            Self::InvalidCard => f.write_str("invalid smart-card handle"),
            Self::UnknownReader => f.write_str("unknown virtual reader"),
            Self::HandleSpaceExhausted => f.write_str("smart-card handle space exhausted"),
            Self::Io(error) => error.fmt(f),
        }
    }
}

impl Error for RegistryError {
    fn source(&self) -> Option<&(dyn Error + 'static)> {
        match self {
            Self::Io(error) => Some(error),
            _ => None,
        }
    }
}

impl From<io::Error> for RegistryError {
    fn from(error: io::Error) -> Self {
        Self::Io(error)
    }
}

struct Card {
    context: ContextHandle,
    reader: String,
    proxy: Mutex<Proxy>,
}

struct State {
    config: ClientConfig,
    contexts: BTreeSet<ContextHandle>,
    cards: BTreeMap<CardHandle, Arc<Card>>,
    connect_failures: BTreeMap<(Ipv4Addr, u16), Instant>,
    next_context: usize,
    next_card: usize,
}

fn endpoint_in_connect_backoff(
    failures: &mut BTreeMap<(Ipv4Addr, u16), Instant>,
    endpoint: (Ipv4Addr, u16),
    now: Instant,
) -> bool {
    failures.retain(|_, failed_at| now.duration_since(*failed_at) < CONNECT_BACKOFF);
    failures.contains_key(&endpoint)
}

pub struct Registry {
    state: Mutex<State>,
    timeout: Duration,
}

impl Registry {
    pub fn new(config: ClientConfig, timeout: Duration) -> Self {
        Self {
            state: Mutex::new(State {
                config,
                contexts: BTreeSet::new(),
                cards: BTreeMap::new(),
                connect_failures: BTreeMap::new(),
                next_context: usize::MAX - 1,
                next_card: 1,
            }),
            timeout,
        }
    }

    pub fn establish_context(&self) -> Result<ContextHandle, RegistryError> {
        let mut state = lock(&self.state);
        let context = loop {
            let candidate = ContextHandle(state.next_context);
            if state.next_context == 0 {
                return Err(RegistryError::HandleSpaceExhausted);
            }
            state.next_context -= 1;
            if !state.contexts.contains(&candidate) {
                break candidate;
            }
        };
        state.contexts.insert(context);
        Ok(context)
    }

    /// Update the legacy single-reader endpoint, matching the DLL's SetConfig export.
    pub fn set_config(&self, host: Ipv4Addr, port: u16) {
        let mut state = lock(&self.state);
        state.config.server_host = host;
        state.config.server_port = port;
        if !state.config.explicit_readers {
            if let Some(reader) = state.config.readers.first_mut() {
                reader.host = host;
                reader.port = port;
            }
        }
    }

    pub fn release_context(&self, context: ContextHandle) -> Result<(), RegistryError> {
        let mut state = lock(&self.state);
        if !state.contexts.remove(&context) {
            return Err(RegistryError::InvalidContext);
        }
        state.cards.retain(|_, card| card.context != context);
        Ok(())
    }

    pub fn list_readers(&self, context: ContextHandle) -> Result<Vec<String>, RegistryError> {
        let state = lock(&self.state);
        if !state.contexts.contains(&context) {
            return Err(RegistryError::InvalidContext);
        }
        Ok(state
            .config
            .readers
            .iter()
            .map(|reader| reader.name.clone())
            .collect())
    }

    pub fn connect(
        &self,
        context: ContextHandle,
        name: Option<&str>,
    ) -> Result<CardHandle, RegistryError> {
        let reader = {
            let mut state = lock(&self.state);
            if !state.contexts.contains(&context) {
                return Err(RegistryError::InvalidContext);
            }
            let reader = state
                .config
                .reader(name)
                .cloned()
                .ok_or(RegistryError::UnknownReader)?;
            let now = Instant::now();
            if endpoint_in_connect_backoff(
                &mut state.connect_failures,
                (reader.host, reader.port),
                now,
            ) {
                return Err(RegistryError::Io(io::Error::new(
                    io::ErrorKind::ConnectionRefused,
                    "recent connection failure backoff is active",
                )));
            }
            reader
        };

        // Connect without holding the registry lock. Other contexts remain usable while
        // a remote endpoint is slow to accept a TCP connection.
        let endpoint = (reader.host, reader.port);
        let proxy = match Proxy::connect(reader.host, reader.port, self.timeout) {
            Ok(proxy) => proxy,
            Err(error) => {
                lock(&self.state)
                    .connect_failures
                    .insert(endpoint, Instant::now());
                return Err(error.into());
            }
        };
        let card = Arc::new(Card {
            context,
            reader: reader.name,
            proxy: Mutex::new(proxy),
        });

        let mut state = lock(&self.state);
        state.connect_failures.remove(&endpoint);
        if !state.contexts.contains(&context) {
            return Err(RegistryError::InvalidContext);
        }
        let handle = loop {
            let candidate = CardHandle(state.next_card);
            if state.next_card == 0 || state.next_card == usize::MAX {
                return Err(RegistryError::HandleSpaceExhausted);
            }
            state.next_card += 1;
            if !state.cards.contains_key(&candidate) {
                break candidate;
            }
        };
        state.cards.insert(handle, card);
        Ok(handle)
    }

    pub fn disconnect(&self, handle: CardHandle) -> Result<(), RegistryError> {
        lock(&self.state)
            .cards
            .remove(&handle)
            .map(drop)
            .ok_or(RegistryError::InvalidCard)
    }

    pub fn reader_name(&self, handle: CardHandle) -> Result<String, RegistryError> {
        lock(&self.state)
            .cards
            .get(&handle)
            .map(|card| card.reader.clone())
            .ok_or(RegistryError::InvalidCard)
    }

    pub fn transmit(&self, handle: CardHandle, apdu: &[u8]) -> Result<Vec<u8>, RegistryError> {
        let card = lock(&self.state)
            .cards
            .get(&handle)
            .cloned()
            .ok_or(RegistryError::InvalidCard)?;
        let result = lock(&card.proxy).transmit(apdu).map_err(RegistryError::Io);
        result
    }
}

fn lock<T>(mutex: &Mutex<T>) -> MutexGuard<'_, T> {
    mutex
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner)
}

#[cfg(test)]
mod tests {
    use super::{endpoint_in_connect_backoff, CONNECT_BACKOFF};
    use std::{
        collections::BTreeMap,
        net::Ipv4Addr,
        time::{Duration, Instant},
    };

    #[test]
    fn endpoint_backoff_expires_at_one_second_without_wall_clock_waits() {
        let endpoint = (Ipv4Addr::LOCALHOST, 6900);
        let failed_at = Instant::now();
        let mut failures = BTreeMap::from([(endpoint, failed_at)]);

        assert!(endpoint_in_connect_backoff(
            &mut failures,
            endpoint,
            failed_at + CONNECT_BACKOFF - Duration::from_nanos(1),
        ));
        assert!(!endpoint_in_connect_backoff(
            &mut failures,
            endpoint,
            failed_at + CONNECT_BACKOFF,
        ));
        assert!(failures.is_empty());
    }
}
