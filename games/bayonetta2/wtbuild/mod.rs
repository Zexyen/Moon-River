//! Bayonetta 2 (Nintendo Switch, version 1.2 / update v131072): what wtbuild knows about this game only.
//! The dump it accepts, the addresses the lifter patches or sends to host functions (../runtime/game_shim_eh.c),
//! and the runtime sources only this game needs. tools/wtbuild/src/main.rs includes this file as `crate::game`.

use crate::lifter::Image;

#[path = "post_lift.rs"]
mod post_lift_impl;
pub use post_lift_impl::post_lift;

/// the folder under games/
pub const NAME: &str = "bayonetta2";

/// this game's own runtime sources (../runtime), built into the host runtime after the shared ones
pub const RUNTIME_SOURCES: &[&str] = &[];

/// data the game library exports to the runtime besides the tables every game has (../runtime/game_shim_eh.c)
pub const EXPORTS: &[&str] = &["game_fsr_w", "game_fsr_h", "game_fsr_phases", "game_mv_pairs"];

// ---- Bayonetta 2 version 1.2 (update v131072)
pub const VERSION: &str = "Bayonetta 2 version 1.2 (update v131072)";
/// the dump's exefs files: name, size, sha256, NSO build id. The first is main
pub const EXEFS: &[(&str, u64, &str, Option<&str>)] = &[
    (
        "main",
        13166635,
        "95b2d654ec9d9fe391f1a5ad991bcb5c0d1ed97105dce46e4360dce1d8a3356e",
        Some("f19b2e01e1794f933a9720c1404bb6f600000000"),
    ),
    (
        "sdk",
        4181288,
        "2b8824ba37e7ea3e0b7cc9853e5f81b643334374ce3a92efb2a49ae040f77d09",
        Some("8ceabb1750539c3874cc9fab45cab335f784cc38"),
    ),
    (
        "subsdk0",
        6072185,
        "348d4bf6ca468fdabd455ed41bebb6b37b221c5c694cf2a7b4e81fc55feae779",
        Some("a82aa14c5d5a359ad04dc4eab9f5d5901315ee5e"),
    ),
    (
        "rtld",
        7099,
        "c2d9a6e220bf93bc8196b3ea58080f1cd88cf6820508bd9b10e76dc5bb79b92a",
        Some("4044eca5abc5055b1b0bb8b9babe1c9b524a4579"),
    ),
    (
        "main.npdm",
        1476,
        "4b9cbd4e3baf419381d70560cacea255b9c2e5726a82ba779f893e8a22aac702",
        None,
    ),
];
/// the files of EXEFS a dump must have
pub const REQUIRED: &[&str] = &["main", "sdk", "main.npdm"];
/// a few RomFS files checked one by one: path, size, sha256
pub const ROMFS_FILES: &[(&str, u64, &str)] = &[
    (
        "data006.cpk",
        24378176,
        "a6aaa3c1f036eba30011e758ae2b42cdcbb0045ad54ad6bea953ebbcc3e11ac7",
    ),
    (
        "data005.cpk",
        183508728,
        "ce8fb1b0a7cf2f698554f4f82ac00e8dc04e6afbfb82d6ecc8d296ef0dfa39ac",
    ),
    (
        "sound/WwiseInfo.wai",
        113996,
        "45461675e561ae8fa903467a953a53841b5c366b56e1b8b0b6ccdc044c4dd43f",
    ),
];
/// the whole RomFS: file count, bytes, sha256 of its manifest
pub const ROMFS_MANIFEST: (usize, u64, &str) = (
    1643,
    13480389777,
    "4d5e288d43f669353e483f279e325984544906c253d932863b649f8f681c340d",
);

/// this main is Bayonetta 2 (and not the small testdata stand-in ELF)
pub fn is_game(img: &[u8]) -> bool {
    img.len() > 0x1000000
}

/// game functions whose direct calls (BL) go to a host function instead: (address, name) calls h_<name>(Ctx *c)
pub const HOST_OVERRIDES: &[(i64, &str)] = &[];

/// instruction patches of main, checked against the words the binary has (im.patches), and its frame counters
pub fn patches(im: &mut Image) -> Result<(), String> {
    if !im.game {
        return Ok(());
    }
    im.fps_counters = counters();
    Ok(())
}

/// ../lifter/fps_counters.txt: the store address of every site of main whose add counts the game time due
pub fn counters() -> std::collections::HashSet<i64> {
    include_str!("../lifter/fps_counters.txt")
        .lines()
        .filter_map(|l| l.split_whitespace().next())
        .filter(|w| !w.starts_with('#'))
        .map(|w| i64::from_str_radix(w, 16).expect("fps_counters.txt: a hex store address"))
        .collect()
}
