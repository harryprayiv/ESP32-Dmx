{
  description = "ESP32 DMX512 controller with a PureScript web console";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";

    # Keeps its own nixpkgs pin, which is the one its ESP-IDF build is tested against.
    esp-dev.url = "github:mirrexagon/nixpkgs-esp-dev";

    purescript-overlay = {
      url = "github:thomashoneyman/purescript-overlay";
      inputs.nixpkgs.follows = "nixpkgs";
    };
  };

  outputs =
    {
      nixpkgs,
      esp-dev,
      purescript-overlay,
      ...
    }:
    let
      system = "x86_64-linux";
      pkgs = import nixpkgs {
        inherit system;
        overlays = [ purescript-overlay.overlays.default ];
      };
    in
    {
      devShells.${system}.default = pkgs.mkShell {
        # ESP-IDF with the Xtensa and RISC-V toolchains. Its setup hook exports
        # IDF_PATH and puts idf.py on PATH.
        buildInputs = [ esp-dev.packages.${system}.esp-idf-full ];

        packages = [
          pkgs.purs
          pkgs.spago-unstable
          pkgs.esbuild
          pkgs.git
        ];
      };
    };
}