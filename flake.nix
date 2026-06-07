{
  description = "scrapeff — C++/Qt scrapers and gallery; optional Gaussian-splatting shell";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs {
          inherit system;
          config.allowUnfree = true;
        };

        # pip's PySide6 is built for manylinux and expects host libs; on Nix, list them explicitly.
        pysidePipRuntimes = with pkgs; [
          zstd
          glib
          pcre2
          expat
          fontconfig
          freetype
          harfbuzz
          libpng
          libjpeg
          libtiff
          dbus
          libxcb
          xcbutil
          xcbutilwm
          xcbutilimage
          xcbutilkeysyms
          xcbutilerrors
          libxkbcommon
          icu
          wayland
          sqlite
          brotli
          libdrm
          # QtNetwork (PySide6) links GSSAPI/Kerberos (libgssapi_krb5.so.2)
          krb5
          # QSslSocket / Qt TLS needs libssl at runtime
          openssl
        ];

        scrapeffShell =
          let
            qtp = pkgs.qt6;
          in
          pkgs.mkShell {
            packages = with pkgs; [
              cmake
              ninja
              gnumake
              gcc
              pkg-config
              openssl
              git
              qtp.qtbase
              cursor-cli
            ];

            CMAKE_PREFIX_PATH = pkgs.lib.makeSearchPath "" [ qtp.qtbase.dev ];

            shellHook = ''
              export SSL_CERT_FILE="${pkgs.cacert}/etc/ssl/certs/ca-bundle.crt"
              export NIX_SSL_CERT_FILE="''$SSL_CERT_FILE"
              # So plain `cmake ..` + Unix Makefiles finds make & compilers (same idea as apt install build-essential).
              export CMAKE_MAKE_PROGRAM="${pkgs.gnumake}/bin/make"
              export CC="${pkgs.gcc}/bin/gcc"
              export CXX="${pkgs.gcc}/bin/g++"
              echo "scrapeff (Qt 6) — default shell"
              echo "  cmake -S . -B build && cmake --build build -j"
              echo "  (inside build/: cmake .. && make — needs make + gcc; both are in this shell)"
              echo ""
            '';
          };

      in
      {
        devShells.default = scrapeffShell;
        devShells.scrapeff = scrapeffShell;
      });
}
