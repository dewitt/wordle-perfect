{
  description = "wordle-perfect — best-known Wordle decision tree solver";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = nixpkgs.legacyPackages.${system};
      in {
        devShells.default = pkgs.mkShell {
          name = "wordle-perfect";

          # NOTE: On macOS we use the system Apple Clang (via Xcode Command Line
          # Tools) rather than a Nix-provided LLVM because llvmPackages_18 in
          # nixpkgs-unstable currently fails to build compiler-rt against the
          # Apple SDK 26.4 bundled in this nixpkgs revision.  Apple Clang 21
          # (shipped with Xcode 16 / macOS 15) provides full C++23 and partial
          # C++26 support, which exceeds our requirements.  When a compatible
          # LLVM package becomes available in nixpkgs, the stdenv can be swapped
          # back to llvmPackages_N.stdenv for full hermeticity.
          packages = with pkgs; [
            cmake
            ninja
            sqlite
            pkg-config
            hyperfine
          ];

          shellHook = ''
            echo "wordle-perfect dev shell"
            echo "  compiler : $(clang++ --version | head -1)"
            echo "  cmake    : $(cmake --version | head -1)"
            export CMAKE_GENERATOR="Ninja"
          '';
        };
      });
}
