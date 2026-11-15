set -e
cd ${A:-$HOME/pixelios-artifacts}/gpu
meson setup build/mesa src/mesa --prefix=${A:-$HOME/pixelios-artifacts}/gpu/install-host --buildtype=debugoptimized \
  -Dclang-libdir=$CLANG_LIBDIR \
  -Dgallium-drivers=panfrost -Dvulkan-drivers= -Dplatforms= -Degl=enabled -Dgbm=disabled -Dglx=disabled \
  -Dgles1=disabled -Dgles2=enabled -Dopengl=true -Dglvnd=disabled -Dllvm=enabled -Dshared-llvm=enabled \
  -Dmesa-clc=enabled -Dinstall-mesa-clc=true -Dprecomp-compiler=enabled -Dinstall-precomp-compiler=true \
  -Dtools=drm-shim,panfrost -Dbuild-tests=false -Dvalgrind=disabled -Dlibunwind=disabled -Dlmsensors=disabled \
  -Dgallium-rusticl=false -Dgallium-va=disabled -Dvideo-codecs= -Dteflon=false -Dperfetto=false -Dshader-cache=disabled
