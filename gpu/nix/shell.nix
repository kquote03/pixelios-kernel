# PixOS lane G: host shell for the Mesa Panfrost build (G1) and its host tools (mesa_clc, vtn_bindgen2, panfrost_compile).
# LLVM/clang/libclc/SPIRV-LLVM-Translator are the same llvmPackages_21 set nixpkgs' own mesa uses (llvm 21.1.8).
# Nix keeps clang's libraries in their own output, so Mesa needs -Dclang-libdir=$CLANG_LIBDIR (patch 0002).
let
  pkgs = import <nixpkgs> {};
  llvm = pkgs.llvmPackages_21;
in
pkgs.mkShell {
  inputsFrom = [ pkgs.mesa ];
  packages = [
    pkgs.meson pkgs.ninja pkgs.pkg-config
    (pkgs.python3.withPackages (p: [ p.mako p.pyyaml p.packaging ]))
    pkgs.libdrm pkgs.libglvnd pkgs.glslang pkgs.bison pkgs.flex pkgs.gcc
    llvm.libllvm llvm.libclang llvm.clang-unwrapped llvm.libclc
    pkgs.spirv-llvm-translator pkgs.spirv-tools pkgs.spirv-headers
    pkgs.libpng pkgs.zlib pkgs.file
  ];
  CLANG_LIBDIR = "${pkgs.lib.getLib llvm.clang-unwrapped}/lib";
  shellHook = ''
    export PATH=${pkgs.lib.getDev llvm.libllvm}/bin:$PATH
  '';
}
