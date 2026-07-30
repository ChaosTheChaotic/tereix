{
  description = "A transpiler written for tereix";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    tereix-stdlib = {
      url = "github:ChaosTheChaotic/tereix-stdlib";
      flake = false;
    };
		tereix-legacy.url = "github:ChaosTheChaotic/tereix/legacy";
  };

  outputs =
    { self, nixpkgs, tereix-stdlib, tereix-legacy }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];
      forAllSystems = nixpkgs.lib.genAttrs systems;
    in
    {
      packages = forAllSystems (system: {
        default = nixpkgs.legacyPackages.${system}.callPackage ./package.nix {
          stdlibSrc = tereix-stdlib;
        };
      });

      devShells = forAllSystems (system: {
        default = nixpkgs.legacyPackages.${system}.mkShell {
          inputsFrom = [ self.packages.${system}.default ];
          packages = with nixpkgs.legacyPackages.${system}; [
            gdb
            clang-tools
            valgrind
						zstd
						cmocka
						tereix-legacy.packages.${system}.default
          ];
        };
      });
    };
}
