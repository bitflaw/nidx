{
  description = "Nix Packages Indexer";
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
      devShells.${system}.default = pkgs.mkShell {
        buildInputs = with pkgs; [
          cmake
          clang-tools

          sqlite
          sqlitecpp
        ];
        shellHook = ''
          export SHELL="${pkgs.bashInteractive}/bin/bash"
        '';
      };
    };
}
