set -e
export PATH=$HOME/pixelios-artifacts/gpu/ios/bin:$PATH   # clang finds ld64 as "ld" (theos, wrapped); native gcc keeps its absolute binutils path
A=${A:-$HOME/pixelios-artifacts}
cd $A/gpu
rm -rf build/mesa-ios
meson setup build/mesa-ios src/mesa --cross-file ios/ios-arm64.cross --native-file ios/host-tools.native --wrap-mode=nofallback \
  --buildtype=release -Db_ndebug=true -Ddefault_library=static -Ddarwin-sweet=true \
  -Dplatforms= -Degl=disabled -Dglx=disabled -Dgbm=disabled \
  -Dgallium-drivers=panfrost -Dvulkan-drivers= -Dllvm=disabled \
  -Dmesa-clc=system -Dprecomp-compiler=system -Dpanfrost-rust=false \
  -Dopengl=true -Dgles1=disabled -Dgles2=enabled \
  -Dzstd=disabled -Dzlib=disabled -Dexpat=disabled -Dxmlconfig=disabled -Dshader-cache=disabled \
  -Dvalgrind=disabled -Dlibunwind=disabled -Dlmsensors=disabled -Dperfetto=false -Dbuild-tests=false \
  -Dgallium-rusticl=false -Dtools= -Dteflon=false -Dgallium-va=disabled -Dvideo-codecs= -Dhtml-docs=disabled -Ddisplay-info=disabled
