fn main() {
    // Compile the HyperPack C engine as a static library
    cc::Build::new()
        .file("../../src/hyperpack_lib.c")
        .opt_level(2)
        .flag_if_supported("-w") // suppress warnings
        .compile("hyperpacklib");

    // Link platform-specific system libraries
    if cfg!(target_os = "windows") {
        // pthreads & libm are bundled in MinGW's libgcc/msvcrt on Windows
        // The cc crate links against MSVCRT automatically
    } else if cfg!(target_os = "macos") {
        // libm is part of libSystem on macOS — no explicit link needed
        println!("cargo:rustc-link-lib=pthread");
    } else {
        // Linux and other POSIX
        println!("cargo:rustc-link-lib=pthread");
        println!("cargo:rustc-link-lib=m");
    }

    println!("cargo:rerun-if-changed=../../src/hyperpack.c");
    println!("cargo:rerun-if-changed=../../src/hyperpack_lib.c");

    tauri_build::build()
}
