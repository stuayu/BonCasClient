use boncasclient_core::{ClientConfig, Proxy, Registry};
use std::{
    io::{Read, Write},
    net::{Ipv4Addr, TcpListener},
    sync::mpsc,
    thread,
    time::Duration,
};

#[test]
fn ini_parser_matches_profile_values_and_uses_first_sections_and_keys() {
    let config = ClientConfig::parse_ini(
        "[Server] trailing text\n\
         IP=\"192.0.2.10\"\n\
         Port=06900 ; note\n\
         Port=6901\n\
         [Server]\n\
         IP=192.0.2.11\n\
         [Local] comment\n\
         Enable=01\n\
         Enable=0\n\
         [Readers] trailing text\n\
         Primary=6901\n\
         [Readers]\n\
         Secondary=6902\n",
    );

    assert_eq!(config.server_host, Ipv4Addr::new(192, 0, 2, 10));
    assert_eq!(config.server_port, 6900);
    assert!(config.local_enabled);
    assert!(config.explicit_readers);
    assert_eq!(config.readers.len(), 1);
    assert_eq!(config.readers[0].name, "Primary");
}

#[test]
fn legacy_config_falls_back_for_unknown_reader_names() {
    let config = ClientConfig::parse_ini("[Server]\nIP=192.0.2.20\nPort=6900\n");
    let reader = config.reader(Some("reader name saved by Windows")).unwrap();

    assert_eq!(reader.name, "BonCasLink Client Card Reader");
    assert_eq!(reader.host, Ipv4Addr::new(192, 0, 2, 20));
}

#[test]
fn ini_byte_parser_reads_legacy_windows_31j_and_utf16() {
    let ini = "[Readers]\nカードリーダー=6900\n";
    let (legacy_bytes, _, had_errors) = encoding_rs::SHIFT_JIS.encode(ini);
    assert!(!had_errors);
    let legacy = ClientConfig::parse_ini_bytes(&legacy_bytes);
    assert_eq!(legacy.readers[0].name, "カードリーダー");

    let mut utf16 = vec![0xFF, 0xFE];
    for unit in ini.encode_utf16() {
        utf16.extend_from_slice(&unit.to_le_bytes());
    }
    let unicode = ClientConfig::parse_ini_bytes(&utf16);
    assert_eq!(unicode.readers[0].name, "カードリーダー");
}

#[test]
fn registry_set_config_overrides_the_legacy_endpoint() {
    let listener = TcpListener::bind((Ipv4Addr::LOCALHOST, 0)).unwrap();
    let port = listener.local_addr().unwrap().port();
    let server = thread::spawn(move || {
        let (mut stream, _) = listener.accept().unwrap();
        let mut request = [0u8; 2];
        stream.read_exact(&mut request).unwrap();
        assert_eq!(request, [1, 0xA4]);
        stream.write_all(&[2, 0x90, 0x00]).unwrap();
    });

    let registry = Registry::new(ClientConfig::default(), Duration::from_secs(1));
    registry.set_config(Ipv4Addr::LOCALHOST, port);
    let context = registry.establish_context().unwrap();
    let card = registry
        .connect(context, Some("reader name saved by Windows"))
        .unwrap();

    assert_eq!(registry.transmit(card, &[0xA4]).unwrap(), [0x90, 0x00]);
    registry.disconnect(card).unwrap();
    registry.release_context(context).unwrap();
    server.join().unwrap();
}

#[test]
fn proxy_retires_connection_after_an_empty_response() {
    let listener = TcpListener::bind((Ipv4Addr::LOCALHOST, 0)).unwrap();
    let address = listener.local_addr().unwrap();
    let server = thread::spawn(move || {
        let (mut stream, _) = listener.accept().unwrap();
        let mut request = [0u8; 2];
        stream.read_exact(&mut request).unwrap();
        stream.write_all(&[0]).unwrap();
    });
    let mut proxy =
        Proxy::connect(Ipv4Addr::LOCALHOST, address.port(), Duration::from_secs(1)).unwrap();

    assert_eq!(
        proxy.transmit(&[0xA4]).unwrap_err().kind(),
        std::io::ErrorKind::TimedOut
    );
    assert_eq!(
        proxy.transmit(&[0xA4]).unwrap_err().kind(),
        std::io::ErrorKind::NotConnected
    );
    server.join().unwrap();
}

#[test]
fn proxy_retires_connection_after_a_partial_response_timeout() {
    let listener = TcpListener::bind((Ipv4Addr::LOCALHOST, 0)).unwrap();
    let address = listener.local_addr().unwrap();
    let (client_done, wait_for_client) = mpsc::channel();
    let server = thread::spawn(move || {
        let (mut stream, _) = listener.accept().unwrap();
        let mut request = [0u8; 2];
        stream.read_exact(&mut request).unwrap();
        stream.write_all(&[3, 0x90]).unwrap();
        let _ = wait_for_client.recv_timeout(Duration::from_secs(5));
    });
    let mut proxy = Proxy::connect(
        Ipv4Addr::LOCALHOST,
        address.port(),
        Duration::from_millis(80),
    )
    .unwrap();

    let error = proxy.transmit(&[0xA4]).unwrap_err();
    assert!(matches!(
        error.kind(),
        std::io::ErrorKind::TimedOut | std::io::ErrorKind::WouldBlock
    ));
    client_done.send(()).unwrap();
    assert_eq!(
        proxy.transmit(&[0xA4]).unwrap_err().kind(),
        std::io::ErrorKind::NotConnected
    );
    server.join().unwrap();
}
