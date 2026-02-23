{
  description = "sash - tee with a live tail window";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = nixpkgs.legacyPackages.${system};
      in {
        packages.default = pkgs.stdenv.mkDerivation {
          pname = "sash";
          version = "0.1.0";
          src = pkgs.lib.cleanSourceWith {
            src = ./.;
            filter = path: type:
              let baseName = baseNameOf (toString path);
              in !(baseName == "build" || baseName == "result" || baseName == ".direnv");
          };
          nativeBuildInputs = [ pkgs.cmake ];
          meta = with pkgs.lib; {
            description = "tee with a live tail window";
            homepage = "https://github.com/bflyblue/sash";
            license = licenses.bsd2;
            platforms = platforms.unix;
            mainProgram = "sash";
          };
        };

        devShells.default = pkgs.mkShell {
          packages = [ pkgs.cmake pkgs.gcc ];
        };
      });
}
