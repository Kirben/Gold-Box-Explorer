# Build Gold Box Explorer

Install CMake 3.21 or newer, a C++17 compiler, and wxWidgets 3.2 development files. On Linux and macOS, CMake also needs iconv.

From the repository root, run:

```sh
cd portable
cmake --preset linux-release
cmake --build --preset linux-release
```

Use `windows-release` on Windows or `macos-release` on macOS in both commands. If CMake cannot locate wxWidgets, pass `-DCMAKE_PREFIX_PATH=<wxWidgets-install-prefix>` when configuring.

To cross-build Windows x64 from Linux or WSL, install `x86_64-w64-mingw32-g++` and build wxWidgets 3.2 statically for that target. From `portable`, run:

```sh
export WX_WINDOWS_PREFIX=/path/to/mingw-wxwidgets
cmake --preset windows-mingw-release
cmake --build --preset windows-mingw-release
```
