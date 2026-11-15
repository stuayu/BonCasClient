# Rust migration

`boncasclient-core` ports reader configuration, BonCasLink TCP framing, and the initial context/card registry. INI text can be parsed from Unicode strings or legacy profile bytes (UTF-8/UTF-16 BOM and Windows-31J). Its registry supports the legacy single-reader name fallback, `set_config` endpoint override, and one-second per-endpoint connect-failure backoff. A failed or empty response retires that TCP connection so later APDUs cannot consume a stale frame.

The crate builds as a Rust library only; the current WinSCard DLL exports remain in the C++ project. The next migration step is the Windows ABI layer and `SCard*` exports. Preserve the C++ local-reader-first behavior at that boundary, then use the registry fallback for unknown names in legacy single-reader mode. Keep the C++ DLL as the reference implementation while that boundary is built.
