fn main() {
    let mut build = cc::Build::new();
    build
        .file("../../src/hyperpack_lib.c")
        .opt_level(2)
        .flag_if_supported("-w"); // suppress warnings

    // hyperpack.c uses POSIX APIs (dirent.h, pthread.h, unistd.h).
    // On Windows we require the x86_64-pc-windows-gnu Rust toolchain so that
    // MinGW gcc + linker are used throughout and POSIX symbols resolve correctly.
    // The .compiler() call is a safety net; with the GNU toolchain cc already
    // picks up MinGW's gcc from PATH automatically.
    if cfg!(target_os = "windows") {
        build.compiler("gcc");
    }

    build.compile("hyperpacklib");

    // Link platform-specific system libraries
    if cfg!(target_os = "windows") {
        // With x86_64-pc-windows-gnu, link winpthread statically so that the
        // final binary does not depend on libwinpthread-1.dll at runtime.
        println!("cargo:rustc-link-lib=static=winpthread");
        println!("cargo:rustc-link-lib=z");
    } else if cfg!(target_os = "macos") {
        // libm is part of libSystem; pthreads are bundled too — no explicit link.
        println!("cargo:rustc-link-lib=pthread");
        println!("cargo:rustc-link-lib=z");
    } else {
        // Linux / other POSIX
        println!("cargo:rustc-link-lib=pthread");
        println!("cargo:rustc-link-lib=m");
        println!("cargo:rustc-link-lib=z");
    }

    println!("cargo:rerun-if-changed=../../src/hyperpack.c");
    println!("cargo:rerun-if-changed=../../src/hyperpack_lib.c");

    tauri_build::build()
}
