#pragma once

namespace scrapeff {

/**
 * Configure Qt/OpenSSL TLS for outbound HTTPS similarly to typical Python setups:
 * merge CA bundles from SSL_CERT_FILE / NIX_SSL_CERT_FILE when set (e.g. nix-shell flake hook),
 * otherwise common OS CA bundle paths (parity with certifi-backed requests outside nix-shell).
 */
void applySslFromEnv();

} // namespace scrapeff
