{
  description = "Nix Packages Indexer (and LSP server).";
  inputs = {
    nixpkgs.url = "nixpkgs/nixos-25.11";
  };
  outputs =
    { self, nixpkgs }:
    let
      system = "x86_64-linux";
      pkgs = import nixpkgs { inherit system; };
    in
    {
      packages.${system} = rec {
        nidx = pkgs.stdenv.mkDerivation {
          pname = "nidx";
          version = "0.1.0";
          src = ./.;

          nativeBuildInputs = [ pkgs.cmake ];
          buildInputs = [
            pkgs.sqlite
            pkgs.sqlitecpp
          ];
          cmakeFlags = [
            "-DCMAKE_BUILD_TYPE=Release"
          ];

          meta = with pkgs.lib; {
            description = "Nix Packages Indexer (and LSP server).";
            homepage = "https://github.com/bitflaw/nidx";
            license = licenses.gpl3Plus;
            maintainers = [ ];
            platforms = platforms.linux;
          };
        };
        default = nidx;
      };

      devShells.${system}.default = pkgs.mkShell {
        nativeBuildInputs = with pkgs; [
          cmake
          clang-tools
        ];
        buildInputs = [
          pkgs.sqlite
          pkgs.sqlitecpp
        ];
        shellHook = ''
          export SHELL="${pkgs.bashInteractive}/bin/bash"
        '';
      };
    };
}
