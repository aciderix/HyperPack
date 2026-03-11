# Security Policy

## Supported Versions

| Version | Supported |
|---------|-----------|
| Latest release | ✅ |
| Older releases | ❌ |

## Reporting a Vulnerability

If you discover a security vulnerability in HyperPack, please report it responsibly:

1. **Do not** open a public issue
2. Email the maintainer directly (see GitHub profile)
3. Include a description of the vulnerability and steps to reproduce
4. Allow reasonable time for a fix before public disclosure

## Scope

HyperPack processes untrusted input files (compressed archives). Security-relevant areas include:

- Buffer overflows in decompression routines
- Integer overflows in size calculations
- Malformed HPK files causing crashes or infinite loops
- Memory exhaustion via crafted inputs

## Web App

The web app runs entirely client-side via WebAssembly. No files are uploaded to any server. The WASM sandbox provides an additional layer of isolation.
