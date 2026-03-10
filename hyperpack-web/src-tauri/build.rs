fn main() {
    // Compile the HyperPack C engine as a static library
    cc::Build::new()
        .file("../../src/hyperpack_lib.c")
        .opt_level(2)
        .flag_if_supported("-w") // suppress warnings (they show in native build already)
        .flag_if_supported("-lpthread")
        .compile("hyperpacklib");

    // Link pthreads on non-Windows
    #[cfg(not(target_os = "windows"))]
    println!("cargo:rustc-link-lib=pthread");

    println!("cargo:rustc-link-lib=m");
    println!("cargo:rerun-if-changed=../../src/hyperpack.c");
    println!("cargo:rerun-if-changed=../../src/hyperpack_lib.c");

    tauri_build::build()
}
