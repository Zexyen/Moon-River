//! [moonriver] Direct extraction of the target Switch title from the player's own Nintendo Switch
//! NSP & XCI files using their own `prod.keys`.
//!
//! This repository contains NO keys, NO game code, and NO game assets.

pub mod container;
pub mod crypto;
pub mod keys;
pub mod nca;
pub mod storage;

use crate::game::{DISPLAY_NAME, EXEFS, TITLE_BASE_ID, TITLE_MASK, TITLE_UPDATE_ID, UPDATE_LABEL};
use container::open_package;
use keys::{discover_prod_keys, parse_ticket, KeySet};
use nca::{ExeFs, Nca, RomFs};
use std::collections::HashMap;
use std::fs;
use std::path::{Path, PathBuf};

/// True if `path` has a `.nsp` or `.xci` extension (case-insensitive).
pub fn is_package_file(path: &Path) -> bool {
    path.extension()
        .and_then(|e| e.to_str())
        .map(|e| matches!(e.to_ascii_lowercase().as_str(), "nsp" | "xci"))
        .unwrap_or(false)
}

/// True if `path` has a compressed Switch package extension (`.nsz` / `.xcz`).
pub fn is_compressed_package(path: &Path) -> bool {
    path.extension()
        .and_then(|e| e.to_str())
        .map(|e| matches!(e.to_ascii_lowercase().as_str(), "nsz" | "xcz"))
        .unwrap_or(false)
}

/// Classify whether `--dump` (and optional `--update`) points to NSP/XCI package(s) or an extracted folder.
pub enum DumpSource {
    ExtractedDir,
    Packages(Vec<PathBuf>),
}

/// Resolve user-supplied dump + optional update paths into either an extracted folder or a list of `.nsp`/`.xci` files.
pub fn classify_dump(dump: &Path, update: Option<&Path>) -> Result<DumpSource, String> {
    let mut files: Vec<PathBuf> = Vec::new();
    let mut compressed: Vec<String> = Vec::new();

    if dump.is_file() {
        if is_compressed_package(dump) {
            return Err(format!(
                "{}: compressed NSZ/XCZ files are not supported directly. Decompress to NSP or XCI first.",
                dump.display()
            ));
        }
        if !is_package_file(dump) {
            return Err(format!(
                "{}: expected an extracted dump folder or a Switch .nsp / .xci file",
                dump.display()
            ));
        }
        files.push(dump.to_path_buf());
    } else if dump.is_dir() {
        // An extracted folder with exefs/ and romfs/ stays an extracted folder unless --update was also passed
        if update.is_none() && dump.join("exefs").is_dir() && dump.join("romfs").is_dir() {
            return Ok(DumpSource::ExtractedDir);
        }
        let Ok(rd) = fs::read_dir(dump) else {
            return Ok(DumpSource::ExtractedDir);
        };
        for e in rd.flatten() {
            let p = e.path();
            if p.is_file() {
                if is_package_file(&p) {
                    files.push(p);
                } else if is_compressed_package(&p) {
                    compressed.push(e.file_name().to_string_lossy().into_owned());
                }
            }
        }
        files.sort();
        if files.is_empty() {
            if !compressed.is_empty() {
                return Err(format!(
                    "{}: compressed NSZ/XCZ files ({}) are not supported directly. Decompress them to NSP or XCI first.",
                    dump.display(),
                    compressed.join(", ")
                ));
            }
            if update.is_none() {
                return Ok(DumpSource::ExtractedDir);
            }
        }
    } else if update.is_none() {
        return Ok(DumpSource::ExtractedDir);
    } else {
        return Err(format!("{}: file or folder not found", dump.display()));
    }

    if let Some(upd) = update {
        if !upd.exists() {
            return Err(format!("--update {}: file or folder not found", upd.display()));
        }
        if upd.is_file() {
            if is_compressed_package(upd) {
                return Err(format!(
                    "{}: compressed NSZ/XCZ files are not supported directly. Decompress to NSP or XCI first.",
                    upd.display()
                ));
            }
            if !is_package_file(upd) {
                return Err(format!("--update {}: expected a .nsp or .xci file", upd.display()));
            }
            if !files.contains(&upd.to_path_buf()) {
                files.push(upd.to_path_buf());
            }
        } else if upd.is_dir() {
            if let Ok(rd) = fs::read_dir(upd) {
                for e in rd.flatten() {
                    let p = e.path();
                    if p.is_file() && is_package_file(&p) && !files.contains(&p) {
                        files.push(p);
                    }
                }
            }
        }
    }

    if files.is_empty() {
        return Err(format!("{}: no .nsp or .xci files found", dump.display()));
    }
    Ok(DumpSource::Packages(files))
}

/// Resolved target Program 0 (base + update, or a standalone updated cartridge base NCA).
pub struct TitleSet {
    pub keys_path: PathBuf,
    pub prog0_base: Option<Nca>,
    pub prog0_update: Nca,
}

fn nso_build_id_bytes(data: &[u8]) -> Option<String> {
    if data.len() >= 0x54 && &data[0..4] == b"NSO0" {
        Some(crate::util::hex(&data[0x40..0x54]))
    } else {
        None
    }
}

fn nca_matches_main_bid(nca: &Nca, want_bid: Option<&str>) -> bool {
    let Some(want) = want_bid else { return false };
    nca.open_exefs()
        .ok()
        .and_then(|exefs| exefs.read_file("main").ok().flatten())
        .and_then(|main_bytes| nso_build_id_bytes(&main_bytes))
        .as_deref()
        == Some(want)
}

impl TitleSet {
    /// Open one or more `.nsp` / `.xci` files, decrypt NCA headers using `prod.keys`, and resolve
    /// the target game's Program 0 (base + update).
    pub fn open(files: &[PathBuf], explicit_keys: Option<&Path>, repo: Option<&Path>) -> Result<Self, String> {
        if files.is_empty() {
            return Err("no NSP or XCI files provided".into());
        }

        // Collect hint directories (parent dirs of all provided package files)
        let mut hint_dirs: Vec<PathBuf> = Vec::new();
        for f in files {
            if let Some(p) = f.parent() {
                if !p.as_os_str().is_empty() && !hint_dirs.contains(&p.to_path_buf()) {
                    hint_dirs.push(p.to_path_buf());
                }
            }
        }
        let hint_refs: Vec<&Path> = hint_dirs.iter().map(PathBuf::as_path).collect();

        let keys_path = match explicit_keys {
            Some(p) => {
                if !p.is_file() {
                    return Err(format!("--keys {}: file not found", p.display()));
                }
                p.to_path_buf()
            }
            None => discover_prod_keys(&hint_refs, repo).ok_or_else(|| {
                "prod.keys not found: pass --keys <path/to/prod.keys>, or place your console's prod.keys beside your NSP/XCI files or in ~/.switch/prod.keys".to_string()
            })?,
        };

        let mut keys = KeySet::load_prod_keys(&keys_path)?;
        // Also check for title.keys beside the package files
        for d in &hint_dirs {
            let tk = d.join("title.keys");
            if tk.is_file() {
                if let Ok(t) = fs::read_to_string(&tk) {
                    keys.load_title_keys_str(&t);
                }
            }
        }

        // Open every container and collect tickets + NCA slices
        let mut raw_entries = Vec::new();
        for f in files {
            raw_entries.extend(open_package(f)?);
        }

        // Pass 1: parse all .tik tickets (prefer common over personalized if both exist for a rights ID)
        let mut tickets: HashMap<[u8; 16], ([u8; 16], bool)> = HashMap::new();
        for e in &raw_entries {
            if e.name.to_ascii_lowercase().ends_with(".tik") && e.slice.len <= 1024 * 1024 {
                if let Ok(bytes) = e.slice.read_vec_at(0, e.slice.len as usize) {
                    if let Ok(t) = parse_ticket(&bytes) {
                        tickets
                            .entry(t.rights_id)
                            .and_modify(|cur| {
                                if cur.1 && !t.is_personalized {
                                    *cur = (t.encrypted_title_key, false);
                                }
                            })
                            .or_insert((t.encrypted_title_key, t.is_personalized));
                    }
                }
            }
        }

        // Pass 2: decrypt headers of all .nca entries and filter to Program NCAs
        let mut game_programs: Vec<(Nca, PathBuf)> = Vec::new();
        let mut other_titles: Vec<u64> = Vec::new();
        let mut first_nca_err: Option<String> = None;

        for e in raw_entries {
            let lower = e.name.to_ascii_lowercase();
            if !lower.ends_with(".nca") {
                continue;
            }
            match Nca::open(e.name, e.slice, &keys, &tickets) {
                Ok(nca) => {
                    if !nca.is_program() {
                        continue;
                    }
                    if (nca.title_id & TITLE_MASK) == TITLE_BASE_ID {
                        game_programs.push((nca, e.source_path));
                    } else if !other_titles.contains(&nca.title_id) {
                        other_titles.push(nca.title_id);
                    }
                }
                Err(err) => {
                    if first_nca_err.is_none() {
                        first_nca_err = Some(err);
                    }
                }
            }
        }

        if game_programs.is_empty() {
            if !other_titles.is_empty() {
                let ids: Vec<String> = other_titles.iter().map(|t| format!("{:016X}", t)).collect();
                return Err(format!(
                    "no {} ({:016X}) program found in the selected NSP/XCI file(s); found title ID(s): {}",
                    DISPLAY_NAME,
                    TITLE_BASE_ID,
                    ids.join(", ")
                ));
            }
            if let Some(err) = first_nca_err {
                return Err(err);
            }
            return Err("no Switch Program NCAs found in the selected NSP/XCI file(s)".into());
        }

        // Separate base and update NCAs for Program 0
        let mut prog0_bases: Vec<(Nca, PathBuf)> = Vec::new();
        let mut prog0_updates: Vec<(Nca, PathBuf)> = Vec::new();

        for (nca, src) in game_programs {
            if nca.content_index != 0 && (nca.title_id & 0xFF) != 0 {
                continue;
            }
            if nca.has_bktr_patch() || (nca.title_id & 0x800) != 0 {
                prog0_updates.push((nca, src));
            } else {
                prog0_bases.push((nca, src));
            }
        }

        let want_bid = EXEFS.first().and_then(|e| e.3);

        // If no separate update NCA was provided, check if a base NCA already holds the target version
        // (e.g. a revised cartridge revision with Update 1.2 baked into the base Program NCA).
        if prog0_updates.is_empty() {
            if let Some(idx) = prog0_bases.iter().position(|(n, _)| nca_matches_main_bid(n, want_bid)) {
                let (prog0_update, _) = prog0_bases.remove(idx);
                return Ok(Self {
                    keys_path,
                    prog0_base: None,
                    prog0_update,
                });
            }
            return Err(format!(
                "found {} base game ({:016X}), but {} ({:016X}) is missing. \
                 Moon River requires {}. \
                 Select both your base game NSP/XCI and {} NSP (or put both in the same folder).",
                DISPLAY_NAME, TITLE_BASE_ID, UPDATE_LABEL, TITLE_UPDATE_ID, UPDATE_LABEL, UPDATE_LABEL
            ));
        }

        // Pick the best Program 0 update (preferring the one whose ExeFS `main` matches the supported build ID)
        let best_upd_idx = prog0_updates
            .iter()
            .position(|(n, _)| nca_matches_main_bid(n, want_bid))
            .unwrap_or_else(|| {
                // Fall back to highest sdk_version (so step_verify reports the exact build ID mismatch)
                prog0_updates
                    .iter()
                    .enumerate()
                    .max_by_key(|(_, (n, _))| n.sdk_version)
                    .map(|(i, _)| i)
                    .unwrap_or(0)
            });
        let (prog0_update, _) = prog0_updates.remove(best_upd_idx);

        // If Program 0 update uses BKTR, we need the Base Program 0 NCA
        let prog0_base = prog0_bases.into_iter().next().map(|(n, _)| n);
        if prog0_update.has_bktr_patch() && prog0_base.is_none() {
            return Err(format!(
                "found {} update ({:016X}), but the base game ({:016X}) is missing. \
                 Select both your base game NSP/XCI and {} NSP (or put both in the same folder).",
                DISPLAY_NAME, TITLE_UPDATE_ID, TITLE_BASE_ID, UPDATE_LABEL
            ));
        }

        Ok(Self {
            keys_path,
            prog0_base,
            prog0_update,
        })
    }

    pub fn open_exefs(&self) -> Result<ExeFs, String> {
        self.prog0_update.open_exefs()
    }

    pub fn open_romfs(&self) -> Result<RomFs, String> {
        self.prog0_update.open_romfs(self.prog0_base.as_ref())
    }

    /// Extract Program 0 ExeFS to `dst_exefs`.
    pub fn extract_exefs(&self, dst_exefs: &Path) -> Result<usize, String> {
        let exefs = self.open_exefs()?;
        exefs.extract_all(dst_exefs)
    }

    /// Stream Program 0 (merged base + update) RomFS to `dst_romfs`.
    pub fn extract_romfs(
        &self,
        dst_romfs: &Path,
        on_progress: &mut dyn FnMut(usize, usize, u64, u64),
    ) -> Result<(usize, usize), String> {
        let romfs = self.open_romfs()?;
        romfs.extract_all(dst_romfs, on_progress)
    }
}

/// CLI subcommand: `mrbuild extract <file_or_dir>... [--update <file>] [--keys <prod.keys>] --out <dir>`
pub fn main_extract(args: &[String], repo: &Path) -> Result<(), String> {
    let mut inputs: Vec<PathBuf> = Vec::new();
    let mut keys: Option<PathBuf> = None;
    let mut out: Option<PathBuf> = None;

    let mut it = args.iter();
    while let Some(a) = it.next() {
        let (k, inline) = match a.split_once('=') {
            Some((k, v)) if k.starts_with("--") => (k, Some(v.to_string())),
            _ => (a.as_str(), None),
        };
        let mut val = || {
            inline
                .clone()
                .or_else(|| it.next().cloned())
                .ok_or_else(|| format!("extract: {} requires a value", k))
        };
        match k {
            "--keys" => keys = Some(PathBuf::from(val()?)),
            "--out" => out = Some(PathBuf::from(val()?)),
            "--update" | "--dump" => inputs.push(PathBuf::from(val()?)),
            "--repo" => {
                let _ = val()?;
            }
            s if s.starts_with("--") => return Err(format!("extract: unknown option {}", s)),
            _ => inputs.push(PathBuf::from(a)),
        }
    }

    if inputs.is_empty() {
        return Err("usage: mrbuild extract <base.nsp|.xci|folder> [<update.nsp>] [--keys <prod.keys>] --out <folder>".into());
    }
    let out_dir = out.ok_or("extract: --out <folder> is required")?;

    // Expand directories into package files
    let mut pkg_files: Vec<PathBuf> = Vec::new();
    for p in &inputs {
        match classify_dump(p, None)? {
            DumpSource::Packages(v) => {
                for f in v {
                    if !pkg_files.contains(&f) {
                        pkg_files.push(f);
                    }
                }
            }
            DumpSource::ExtractedDir => {
                return Err(format!("{}: no .nsp or .xci files found", p.display()));
            }
        }
    }

    let ts = TitleSet::open(&pkg_files, keys.as_deref(), Some(repo))?;
    println!("keys: {}", ts.keys_path.display());
    println!("extracting {} -> {}", DISPLAY_NAME, out_dir.display());
    let n_exe = ts.extract_exefs(&out_dir.join("exefs"))?;
    println!("  exefs: {} files", n_exe);

    let mut last_pct = usize::MAX;
    let (total_r, new_r) = ts.extract_romfs(&out_dir.join("romfs"), &mut |done, total, bytes, total_bytes| {
        let pct = if total_bytes > 0 { (bytes * 100 / total_bytes) as usize } else { 100 };
        if done == total || pct / 5 != last_pct / 5 {
            last_pct = pct;
            println!(
                "  romfs: [{}/{}] {:.2} / {:.2} GB ({}%)",
                done,
                total,
                bytes as f64 / 1e9,
                total_bytes as f64 / 1e9,
                pct
            );
        }
    })?;
    println!("  romfs: {} files ({} written)", total_r, new_r);
    println!("done -> {}", out_dir.display());
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::switch::container::{build_pfs0, build_xci};
    use crate::switch::crypto::{aes128_ctr_apply, aes128_ecb_encrypt, aes128_ecb_encrypt_block, aes128_xts_encrypt, make_upper_ctr};
    use crate::switch::nca::build_romfs;
    use crate::switch::storage::build_bktr_table;

    /// Helper: build a synthetic NCA3 encrypted with our synthetic test keys (NO real Nintendo keys!).
    struct SyntheticKeys {
        header_key: [u8; 32],
        kak_app: [u8; 16],
        titlekek: [u8; 16],
        prod_keys_text: String,
    }

    impl SyntheticKeys {
        fn make() -> Self {
            let header_key = [0x31u8; 32];
            let kak_app = [0x42u8; 16];
            let titlekek = [0x53u8; 16];
            let prod_keys_text = format!(
                "header_key = {}\nkey_area_key_application_03 = {}\ntitlekek_03 = {}\n",
                crate::util::hex(&header_key),
                crate::util::hex(&kak_app),
                crate::util::hex(&titlekek),
            );
            Self {
                header_key,
                kak_app,
                titlekek,
                prod_keys_text,
            }
        }
    }

    fn align_up(v: usize, a: usize) -> usize {
        (v + a - 1) & !(a - 1)
    }

    fn make_fake_nso(build_id_hex: &str, payload: &[u8]) -> Vec<u8> {
        let mut out = vec![0u8; 0x100];
        out[0..4].copy_from_slice(b"NSO0");
        for i in 0..20 {
            out[0x40 + i] = u8::from_str_radix(&build_id_hex[i * 2..i * 2 + 2], 16).unwrap();
        }
        out.extend_from_slice(payload);
        out
    }

    fn make_ticket(rights_id: &[u8; 16], enc_title_key: &[u8; 16]) -> Vec<u8> {
        let mut tik = vec![0u8; 0x2C0];
        tik[0..4].copy_from_slice(&0x010004u32.to_le_bytes()); // RSA-2048 SHA256
        tik[0x180..0x190].copy_from_slice(enc_title_key);
        tik[0x281] = 0; // common
        tik[0x2A0..0x2B0].copy_from_slice(rights_id);
        tik
    }

    fn finalize_nca(
        sk: &SyntheticKeys,
        title_id: u64,
        content_index: u32,
        rights_id: Option<[u8; 16]>,
        ctr_key: [u8; 16],
        sections: &[(usize, u64, u64, [u8; 0x200])],
        body: &[u8],
    ) -> Vec<u8> {
        let mut hdr = vec![0u8; 0xC00];
        hdr[0x200..0x204].copy_from_slice(b"NCA3");
        hdr[0x205] = 0; // Program
        hdr[0x206] = 4; // key_gen -> rev 3
        hdr[0x207] = 0; // kaek = application
        let total_size = (0xC00 + body.len()) as u64;
        hdr[0x208..0x210].copy_from_slice(&total_size.to_le_bytes());
        hdr[0x210..0x218].copy_from_slice(&title_id.to_le_bytes());
        hdr[0x218..0x21C].copy_from_slice(&content_index.to_le_bytes());
        hdr[0x21C..0x220].copy_from_slice(&0x0003_0600u32.to_le_bytes()); // SDK 3.6.0
        hdr[0x220] = 4;

        if let Some(rid) = rights_id {
            hdr[0x230..0x240].copy_from_slice(&rid);
        } else {
            let mut key_area = [0u8; 0x40];
            key_area[0x20..0x30].copy_from_slice(&ctr_key);
            aes128_ecb_encrypt(&sk.kak_app, &mut key_area);
            hdr[0x300..0x340].copy_from_slice(&key_area);
        }

        for &(idx, start, size, ref fsh) in sections {
            let m_start = (start / 0x200) as u32;
            let m_end = ((start + size) / 0x200) as u32;
            let e = &mut hdr[0x240 + idx * 16..0x240 + (idx + 1) * 16];
            e[0..4].copy_from_slice(&m_start.to_le_bytes());
            e[4..8].copy_from_slice(&m_end.to_le_bytes());
            e[8..12].copy_from_slice(&1u32.to_le_bytes());
            hdr[0x400 + idx * 0x200..0x600 + idx * 0x200].copy_from_slice(fsh);
        }

        aes128_xts_encrypt(&sk.header_key, 0, 0x200, &mut hdr);
        let mut out = hdr;
        out.extend_from_slice(body);
        out
    }

    fn make_pfs0_hash_header(pfs0_offset: u64, pfs0_size: u64) -> [u8; 0xF8] {
        let mut h = [0u8; 0xF8];
        h[0x24..0x28].copy_from_slice(&2u32.to_le_bytes()); // layer count = 2
        h[0x38..0x40].copy_from_slice(&pfs0_offset.to_le_bytes());
        h[0x40..0x48].copy_from_slice(&pfs0_size.to_le_bytes());
        h
    }

    fn make_ivfc_header(romfs_offset: u64, romfs_size: u64) -> [u8; 0xF8] {
        let mut h = [0u8; 0xF8];
        h[0..4].copy_from_slice(b"IVFC");
        h[0x0C..0x10].copy_from_slice(&7u32.to_le_bytes()); // max_layers = 7 -> level 5 at index 5
        let lv5 = 0x10 + 5 * 24;
        h[lv5..lv5 + 8].copy_from_slice(&romfs_offset.to_le_bytes());
        h[lv5 + 8..lv5 + 16].copy_from_slice(&romfs_size.to_le_bytes());
        h
    }

    #[test]
    fn end_to_end_synthetic_xci_and_nsp_with_bktr_and_tickets() {
        let sk = SyntheticKeys::make();
        let dir = std::env::temp_dir().join(format!("mrbuild_switch_e2e_{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        let keys_path = dir.join("prod.keys");
        fs::write(&keys_path, &sk.prod_keys_text).unwrap();

        let main_bid = EXEFS[0].3.unwrap();

        // ---- 1. Build Base Program 0 NCA (standard crypto, RomFS with data006.cpk)
        let base_key = [0xA1u8; 16];
        let base_payload: Vec<u8> = (0..0x2000).map(|i| ((i * 7 + 3) & 0xFF) as u8).collect();
        let base_romfs = build_romfs(&[("data006.cpk", &base_payload)]);
        let base_exefs = build_pfs0(&[("main", &make_fake_nso("0000000000000000000000000000000000000000", b"base-v100"))]);

        let s0_start = 0xC00u64;
        let s0_size = align_up(base_exefs.len(), 0x200) as u64;
        let mut s0_data = base_exefs.clone();
        s0_data.resize(s0_size as usize, 0);
        aes128_ctr_apply(&base_key, make_upper_ctr(0x10000001, 0), s0_start, &mut s0_data);

        let s1_start = s0_start + s0_size;
        let s1_size = align_up(base_romfs.len(), 0x200) as u64;
        let mut s1_data = base_romfs.clone();
        s1_data.resize(s1_size as usize, 0);
        aes128_ctr_apply(&base_key, make_upper_ctr(0x10000002, 0), s1_start, &mut s1_data);

        let mut base_fsh0 = [0u8; 0x200];
        base_fsh0[2] = 1; // PartitionFS
        base_fsh0[3] = 2; // HierarchicalSha256
        base_fsh0[4] = 3; // AesCtr
        base_fsh0[0x08..0x100].copy_from_slice(&make_pfs0_hash_header(0, base_exefs.len() as u64));
        base_fsh0[0x144..0x148].copy_from_slice(&0x10000001u32.to_le_bytes());

        let mut base_fsh1 = [0u8; 0x200];
        base_fsh1[2] = 0; // RomFS
        base_fsh1[3] = 3; // IVFC
        base_fsh1[4] = 3; // AesCtr
        base_fsh1[0x08..0x100].copy_from_slice(&make_ivfc_header(0, base_romfs.len() as u64));
        base_fsh1[0x144..0x148].copy_from_slice(&0x10000002u32.to_le_bytes());

        let mut base_body = s0_data;
        base_body.extend_from_slice(&s1_data);
        let base_nca = finalize_nca(
            &sk,
            TITLE_BASE_ID,
            0,
            None,
            base_key,
            &[
                (0, s0_start, s0_size, base_fsh0),
                (1, s1_start, s1_size, base_fsh1),
            ],
            &base_body,
        );

        // ---- 2. Build Update 1.2 Program 0 NCA (titlekey crypto via .tik, BKTR patch RomFS!)
        let upd_title_key = [0xB2u8; 16];
        let enc_title_key = aes128_ecb_encrypt_block(&sk.titlekek, &upd_title_key);
        let mut rights_id = [0u8; 16];
        rights_id[0..8].copy_from_slice(&TITLE_UPDATE_ID.to_be_bytes());
        rights_id[15] = 0x03; // master key rev 3
        let upd_tik = make_ticket(&rights_id, &enc_title_key);

        let main_nso = make_fake_nso(main_bid, b"bayo2-v120-main");
        let upd_exefs = build_pfs0(&[("main", &main_nso), ("main.npdm", b"META-test")]);

        // Merged RomFS: data006.cpk slice from base + sound/WwiseInfo.wai from update
        let merged_romfs = build_romfs(&[
            ("data006.cpk", &base_payload[0x200..0x1200]),
            ("sound/WwiseInfo.wai", b"synthetic-wwise-info-from-update"),
        ]);
        let split_at = merged_romfs.len() / 2;
        let patch_prefix_size = align_up(split_at, 0x200);
        let mut bktr_patch_img = vec![0u8; patch_prefix_size];
        bktr_patch_img[..split_at].copy_from_slice(&merged_romfs[..split_at]);

        let patch_tail_off = bktr_patch_img.len() as u64;
        let tail_len = merged_romfs.len() - split_at;
        bktr_patch_img.extend_from_slice(&merged_romfs[split_at..]);
        let patch_data_end = align_up(bktr_patch_img.len(), 0x4000) as u64;
        bktr_patch_img.resize(patch_data_end as usize, 0);

        let mut reloc_entries: Vec<Vec<u8>> = Vec::new();
        let mut e0 = vec![0u8; 20];
        e0[0..8].copy_from_slice(&0u64.to_le_bytes());
        e0[8..16].copy_from_slice(&0u64.to_le_bytes());
        e0[16..20].copy_from_slice(&1u32.to_le_bytes()); // from patch
        reloc_entries.push(e0);
        let mut e1 = vec![0u8; 20];
        e1[0..8].copy_from_slice(&(split_at as u64).to_le_bytes());
        e1[8..16].copy_from_slice(&patch_tail_off.to_le_bytes());
        e1[16..20].copy_from_slice(&1u32.to_le_bytes()); // from patch
        reloc_entries.push(e1);
        let reloc_table = build_bktr_table(merged_romfs.len() as u64, 20, &reloc_entries);

        let mut sub_entries: Vec<Vec<u8>> = Vec::new();
        let mut se0 = vec![0u8; 16];
        se0[0..8].copy_from_slice(&0u64.to_le_bytes());
        se0[12..16].copy_from_slice(&1u32.to_le_bytes());
        sub_entries.push(se0);
        let mut se1 = vec![0u8; 16];
        se1[0..8].copy_from_slice(&patch_tail_off.to_le_bytes());
        se1[12..16].copy_from_slice(&2u32.to_le_bytes());
        sub_entries.push(se1);
        let sub_table = build_bktr_table(patch_data_end, 16, &sub_entries);

        let reloc_off = patch_data_end;
        let sub_off = reloc_off + reloc_table.len() as u64;
        let upd_s0_start = 0xC00u64;
        let upd_s0_size = align_up(upd_exefs.len(), 0x200) as u64;
        let mut upd_s0 = upd_exefs.clone();
        upd_s0.resize(upd_s0_size as usize, 0);
        aes128_ctr_apply(&upd_title_key, make_upper_ctr(0x20000001, 0), upd_s0_start, &mut upd_s0);

        let upd_s1_start = upd_s0_start + upd_s0_size;
        aes128_ctr_apply(
            &upd_title_key,
            make_upper_ctr(0x20000002, 1),
            upd_s1_start,
            &mut bktr_patch_img[..patch_tail_off as usize],
        );
        aes128_ctr_apply(
            &upd_title_key,
            make_upper_ctr(0x20000002, 2),
            upd_s1_start + patch_tail_off,
            &mut bktr_patch_img[patch_tail_off as usize..patch_tail_off as usize + tail_len],
        );

        let mut enc_reloc = reloc_table.clone();
        aes128_ctr_apply(&upd_title_key, make_upper_ctr(0x20000002, 0), upd_s1_start + reloc_off, &mut enc_reloc);
        let mut enc_sub = sub_table.clone();
        aes128_ctr_apply(&upd_title_key, make_upper_ctr(0x20000002, 0), upd_s1_start + sub_off, &mut enc_sub);

        let mut upd_s1 = bktr_patch_img;
        upd_s1.extend_from_slice(&enc_reloc);
        upd_s1.extend_from_slice(&enc_sub);
        let upd_s1_size = align_up(upd_s1.len(), 0x200) as u64;
        upd_s1.resize(upd_s1_size as usize, 0);

        let mut upd_fsh0 = [0u8; 0x200];
        upd_fsh0[2] = 1;
        upd_fsh0[3] = 2;
        upd_fsh0[4] = 3;
        upd_fsh0[0x08..0x100].copy_from_slice(&make_pfs0_hash_header(0, upd_exefs.len() as u64));
        upd_fsh0[0x144..0x148].copy_from_slice(&0x20000001u32.to_le_bytes());

        let mut upd_fsh1 = [0u8; 0x200];
        upd_fsh1[2] = 0;
        upd_fsh1[3] = 3;
        upd_fsh1[4] = 4; // AesCtrEx (BKTR)
        upd_fsh1[0x08..0x100].copy_from_slice(&make_ivfc_header(0, merged_romfs.len() as u64));
        upd_fsh1[0x100..0x108].copy_from_slice(&reloc_off.to_le_bytes());
        upd_fsh1[0x108..0x110].copy_from_slice(&(reloc_table.len() as u64).to_le_bytes());
        upd_fsh1[0x110..0x114].copy_from_slice(b"BKTR");
        upd_fsh1[0x114..0x118].copy_from_slice(&1u32.to_le_bytes());
        upd_fsh1[0x120..0x128].copy_from_slice(&sub_off.to_le_bytes());
        upd_fsh1[0x128..0x130].copy_from_slice(&(sub_table.len() as u64).to_le_bytes());
        upd_fsh1[0x130..0x134].copy_from_slice(b"BKTR");
        upd_fsh1[0x134..0x138].copy_from_slice(&1u32.to_le_bytes());
        upd_fsh1[0x144..0x148].copy_from_slice(&0x20000002u32.to_le_bytes());

        let mut upd_body = upd_s0;
        upd_body.extend_from_slice(&upd_s1);
        let upd_nca = finalize_nca(
            &sk,
            TITLE_UPDATE_ID,
            0,
            Some(rights_id),
            upd_title_key,
            &[
                (0, upd_s0_start, upd_s0_size, upd_fsh0),
                (1, upd_s1_start, upd_s1_size, upd_fsh1),
            ],
            &upd_body,
        );

        // ---- 3. Test Scenario A: Base XCI (with card key area) + Update NSP (with .tik)
        let base_xci_path = dir.join("base.xci");
        let upd_nsp_path = dir.join("update_120.nsp");
        fs::write(&base_xci_path, build_xci(&[("base.nca", &base_nca)], true)).unwrap();
        fs::write(&upd_nsp_path, build_pfs0(&[("upd0.nca", &upd_nca), ("upd.tik", &upd_tik)])).unwrap();

        let ts = TitleSet::open(&[base_xci_path.clone(), upd_nsp_path.clone()], Some(&keys_path), None).unwrap();

        let out_a = dir.join("out_a");
        ts.extract_exefs(&out_a.join("exefs")).unwrap();
        let (total_r, wrote_r) = ts.extract_romfs(&out_a.join("romfs"), &mut |_, _, _, _| {}).unwrap();
        assert_eq!((total_r, wrote_r), (2, 2));
        // Second run must resume/skip both files (0 written)
        let (total_r2, wrote_r2) = ts.extract_romfs(&out_a.join("romfs"), &mut |_, _, _, _| {}).unwrap();
        assert_eq!((total_r2, wrote_r2), (2, 0));

        assert_eq!(fs::read(out_a.join("exefs/main")).unwrap(), main_nso);
        assert_eq!(fs::read(out_a.join("exefs/main.npdm")).unwrap(), b"META-test");
        assert_eq!(fs::read(out_a.join("romfs/data006.cpk")).unwrap(), &base_payload[0x200..0x1200]);
        assert_eq!(
            fs::read(out_a.join("romfs/sound/WwiseInfo.wai")).unwrap(),
            b"synthetic-wwise-info-from-update"
        );

        // ---- 4. Test Scenario B: Single Merged Super-XCI containing Base + Update
        let super_xci = dir.join("merged.xci");
        fs::write(
            &super_xci,
            build_xci(&[("base.nca", &base_nca), ("upd0.nca", &upd_nca), ("upd.tik", &upd_tik)], false),
        )
        .unwrap();
        let ts_super = TitleSet::open(&[super_xci], Some(&keys_path), None).unwrap();
        let romfs = ts_super.open_romfs().unwrap();
        let ((cnt, _, _), _) = romfs.manifest();
        assert_eq!(cnt, 2);

        // ---- 5. Test Scenario C: Revised cartridge (base-only NCA already carrying v1.2 build ID)
        let rev_exefs = build_pfs0(&[("main", &main_nso), ("main.npdm", b"META-rev")]);
        let rev_s0_start = 0xC00u64;
        let rev_s0_size = align_up(rev_exefs.len(), 0x200) as u64;
        let mut rev_s0 = rev_exefs.clone();
        rev_s0.resize(rev_s0_size as usize, 0);
        aes128_ctr_apply(&base_key, make_upper_ctr(0x30000001, 0), rev_s0_start, &mut rev_s0);
        let rev_s1_start = rev_s0_start + rev_s0_size;
        let rev_s1_size = align_up(merged_romfs.len(), 0x200) as u64;
        let mut rev_s1 = merged_romfs.clone();
        rev_s1.resize(rev_s1_size as usize, 0);
        aes128_ctr_apply(&base_key, make_upper_ctr(0x30000002, 0), rev_s1_start, &mut rev_s1);
        let mut rev_fsh0 = [0u8; 0x200];
        rev_fsh0[2] = 1;
        rev_fsh0[3] = 2;
        rev_fsh0[4] = 3;
        rev_fsh0[0x08..0x100].copy_from_slice(&make_pfs0_hash_header(0, rev_exefs.len() as u64));
        rev_fsh0[0x144..0x148].copy_from_slice(&0x30000001u32.to_le_bytes());
        let mut rev_fsh1 = [0u8; 0x200];
        rev_fsh1[2] = 0;
        rev_fsh1[3] = 3;
        rev_fsh1[4] = 3;
        rev_fsh1[0x08..0x100].copy_from_slice(&make_ivfc_header(0, merged_romfs.len() as u64));
        rev_fsh1[0x144..0x148].copy_from_slice(&0x30000002u32.to_le_bytes());
        let mut rev_body = rev_s0;
        rev_body.extend_from_slice(&rev_s1);
        let rev_nca = finalize_nca(
            &sk,
            TITLE_BASE_ID,
            0,
            None,
            base_key,
            &[(0, rev_s0_start, rev_s0_size, rev_fsh0), (1, rev_s1_start, rev_s1_size, rev_fsh1)],
            &rev_body,
        );
        let rev_xci = dir.join("rev_cart.xci");
        fs::write(&rev_xci, build_xci(&[("rev.nca", &rev_nca)], false)).unwrap();
        let ts_rev = TitleSet::open(&[rev_xci], Some(&keys_path), None).unwrap();
        assert_eq!(ts_rev.open_exefs().unwrap().read_file("main.npdm").unwrap().as_deref(), Some(&b"META-rev"[..]));

        // ---- 6. Test Scenario D: Error paths (base-only old version, update-only missing base)
        let base_only = TitleSet::open(&[base_xci_path], Some(&keys_path), None);
        assert!(base_only.err().unwrap().contains(UPDATE_LABEL));
        let upd_only = TitleSet::open(&[upd_nsp_path], Some(&keys_path), None);
        assert!(upd_only.err().unwrap().contains("base game"));

        fs::remove_dir_all(&dir).unwrap();
    }
}
