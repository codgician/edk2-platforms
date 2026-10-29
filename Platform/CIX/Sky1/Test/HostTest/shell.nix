{
  pkgs ? import <nixpkgs> { },
}:
pkgs.mkShell.override { stdenv = pkgs.clangStdenv; } {
  nativeBuildInputs = with pkgs; [
    cmake
    ninja
    pkg-config
  ];
  # compiler-rt provides libFuzzer and the sanitizer runtimes.
  buildInputs = with pkgs; [
    cmocka
    llvmPackages.compiler-rt
  ];
  # Sanitizer builds conflict with nixpkgs' fortify/stack hardening flags.
  hardeningDisable = [ "all" ];
}
