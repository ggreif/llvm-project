# LLDB for Rust and Motoko

This fork of the https://github.com/rust-lang/llvm-project/tree/rustc/10.0-2020-05-05 repository
- adds the Rust language plugin (patches from Tom Tromey https://github.com/vadimcn/lldb/commits/rust-lldb)
- adds a Motoko Plugin

## Building

Below steps describe how to build the custom `lldb` on macOS and Linux.

We assume the `nix` package manager.

Prerequisites:
 - `nix-env -i ninja-1.8.2`
 - `nix-env -i cmake-3.12.1`
 - `nix-env -i libedit-20180525-3.1`
 - `nix-env -i ncurses-6.1-20181027`

### macOS

Here we assume the command-line tools are installed (mostly because we need a signed `lldb-server`,
but I also had trouble using the `clang-wrapper` from `nixpkgs`).

Being at the top directory of the repository, do

```
cmake -G Ninja -DLLVM_TARGETS_TO_BUILD=X86 \
-DLLVM_ENABLE_PROJECTS="clang;libcxx;libcxxabi;lldb" \
-DLLDB_USE_SYSTEM_DEBUGSERVER=ON \
-DBUILD_SHARED_LIBS=ON \
-DLLVM_ENABLE_LIBCXX=ON -DLLVM_BUILD_EXTERNAL_COMPILER_RT=OFF \
-DLIBXML2_INCLUDE_DIR=/nix/store/83ccaw4fa3jgsmr28s8b0jhk4y0cjb91-libxml2-2.9.10-dev/include/libxml2 \
-DLIBXML2_LIBRARY=/nix/store/v9pb2nfz6y2jb44fk0zl6fk1cdrgfxs2-libxml2-2.9.10/lib \
-DLLDB_ENABLE_CURSES=OFF \
 llvm
```
Then
```
ninja bin/lldb
```

Improvements that need to be pursued
 - The `libxml2(-dev)` paths are to some random present artifacts, `nix-env -iA nixpkgs.libxml2.dev` doesn't help
 - I have trouble enabling curses and libedit on the Mac.
 - out-of-repo builds
 
 ### Linux
 
 ```
cmake -G Ninja -DLLDB_ENABLE_CURSES=ON \
 -DLLVM_ENABLE_PROJECTS='clang;lldb' -DLLVM_TARGETS_TO_BUILD=X86 \
 -DBUILD_SHARED_LIBS=ON \
 -DCURSES_INCLUDE_DIRS=/nix/store/hxf06ad4ihxpk8459vnkz965rf2vqnpp-ncurses-6.2-dev/include \
 -DCURSES_LIBRARIES=$HOME/.nix-profile/lib \
 -DPANEL_LIBRARIES=$HOME/.nix-profile/lib \
 -DLibEdit_LIBRARIES=$HOME/.nix-profile/lib \
 -DLibEdit_INCLUDE_DIRS=/nix/store/v4832sga5jdrhifqnqv8npa2w969snwh-libedit-20191231-3.1-dev/include \
  llvm
```
Then
```
ninja bin/lldb
```

Improvements that need to be pursued
 - we need a proper nix-expression so that we can cleanly pass the include paths too
 - for now you'll have to edit manually
 - out-of-repo builds
