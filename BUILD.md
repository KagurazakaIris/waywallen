# Building Waywallen

End-to-end build instructions for developers.

## System dependencies

| Dependency | Version | Notes |
|------------|---------|-------|
| Rust | stable | |
| Clang | 22+ | [LLVM-22.1.4-Linux-X64](https://github.com/llvm/llvm-project/releases/download/llvmorg-22.1.4/LLVM-22.1.4-Linux-X64.tar.xz) |
| CMake | 3.28+ | |
| Vulkan SDK | ≥ 1.1 |  |
| Qt6 | ≥ 6.10 | Quick, DBus, Protobuf |
| mpv | - |  |

## Build, install, run

CMake drives everything — Cargo is invoked transparently via [Corrosion](https://github.com/corrosion-rs/corrosion), pinned in `cmake/FetchCorrosion.cmake`.

```bash
cmake --preset clang-release -DCMAKE_INSTALL_PREFIX=install
cmake --build   build/clang-release
cmake --install build/clang-release
```

This produces under `install/`:

```
install/bin/
    waywallen                          # daemon (Rust)
    waywallen-display-layer-shell      # wlroots adapter (Rust)
    waywallen-ui                       # Qt/QML UI
    waywallen-image-renderer           # C++ plugin
    waywallen-video-renderer           # C++ plugin (FFmpeg + Vulkan compute)
install/share/waywallen/
    renderers/*.toml
    sources/*.lua
install/share/{applications,metainfo,icons/...}/
```

The CMake build type maps to a Cargo profile: `Debug` → `cargo --profile dev`, `Release` / `RelWithDebInfo` → `cargo --release`.

To skip components: `-DWAYWALLEN_BUILD_DAEMON=OFF`, `-DWAYWALLEN_BUILD_UI=OFF`, `-DWAYWALLEN_BUILD_PLUGINS=OFF`.

## Launching

```bash
cd install
export QML_IMPORT_PATH=./lib/qt6/qml
export LD_LIBRARY_PATH="$PWD/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
./bin/waywallen --ui ./bin/waywallen-ui --plugin ./share/waywallen
```

## Packaging

CPack is wired up in `cmake/CPackConfig.cmake`. After a successful configure:

```bash
# TGZ (works everywhere)
cmake --build build/clang-release --target package
# or, equivalently:
cpack --preset clang-release

# DEB (requires dpkg-shlibdeps)
cpack --preset clang-release-deb

# RPM (requires rpmbuild)
cpack --preset clang-release-rpm
```

Packages stage into `/usr` (`CPACK_PACKAGING_INSTALL_PREFIX`) regardless of the dev-time `CMAKE_INSTALL_PREFIX`. Runtime dependencies are auto-derived (`CPACK_DEBIAN_PACKAGE_SHLIBDEPS=ON`, `CPACK_RPM_PACKAGE_AUTOREQ=ON`).

The protocol XMLs (`protocol/*.xml`) and `proto/control.proto` are build-time codegen inputs and are not shipped in the package. Read them from the source tree if you need to implement a third-party client.
