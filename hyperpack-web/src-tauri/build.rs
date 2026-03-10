fn main() {
    let mut build = cc::Build::new();
    build
        .file("../../src/hyperpack_lib.c")
        .opt_level(2)
        .flag_if_supported("-w"); // suppress warnings

    // On Windows with the MSVC Rust toolchain, hyperpack.c uses POSIX headers
    // (dirent.h, pthread.h, unistd.h) that cl.exe does not provide.
    // MinGW-w64 gcc must be on PATH (added by CI; for local builds, install MSYS2
    // or choco install mingw). We set the compiler explicitly here so that other
    // cc-compiled crates (vswhom-sys, etc.) are unaffected.
    if cfg!(target_os = "windows") {
        build.compiler("gcc");
    }

    build.compile("hyperpacklib");

    // Link platform-specific system libraries
    if cfg!(target_os = "macos") {
        // libm is part of libSystem on macOS; pthreads bundled in libSystem too
        // — no explicit link needed.
    } else if cfg!(target_os = "windows") {
        // pthreads and libm are bundled in MinGW's libgcc/msvcrt — no explicit link.
    } else {
        // Linux / other POSIX
        println!("cargo:rustc-link-lib=pthread");
        println!("cargo:rustc-link-lib=m");
    }

    println!("cargo:rerun-if-changed=../../src/hyperpack.c");
    println!("cargo:rerun-if-changed=../../src/hyperpack_lib.c");

    tauri_build::build()
}
