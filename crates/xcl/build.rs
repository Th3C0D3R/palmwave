//! xcl.exe is linked without high-entropy ASLR. With it, Windows places a process's bottom-up
//! allocations (heaps, stacks, `VirtualAlloc(NULL)`) around 1–2.5 TiB, above 2^40; the title's
//! memory lands there too, and a retail game's XboxBvhBuilder refuses an acceleration structure
//! there ("DestAccelerationStructureData can not have more than 40b of address set", #585).
//! Without it, the same allocations start near 2 GiB, as console addresses do.

fn main() {
    println!("cargo:rerun-if-changed=build.rs");
    match std::env::var("CARGO_CFG_TARGET_ENV").as_deref() {
        Ok("msvc") => println!("cargo:rustc-link-arg-bin=xcl=/HIGHENTROPYVA:NO"),
        Ok("gnu") => println!("cargo:rustc-link-arg-bin=xcl=-Wl,--disable-high-entropy-va"),
        _ => {}
    }
}
