//! Post-lift C patches for Bayonetta 2 v1.2 (update v131072):
//!   1. Cutscene deleted-character safety (Findings 146 & 150)
//!   2. Action prompt ring angle for printed-letter layouts (Finding 148)
//!   3. Frame rate above 60 fps up to 120 fps (Findings 107, 113, 114, 129, 134, 136, 139, 144)
//!   4. Wide screens 21:9 / 32:9 (Hor+ width, post-effect buffers, centered 2D projection)
//!   5. Menu confirm/cancel swap for hardcoded Yes/No dialogs (Muspelheim & Gates of Hell teleporters, Chapter 1 telephone)
//!
//! All runtime helper bodies live in games/bayonetta2/runtime/game_shim_eh.c (compiled once with
//! system headers into game_shim_eh.o); this module only inserts one-line calls into the lifted
//! c_*.c files and appends their prototypes to build/gen_eh/funcs.h, so the player's freestanding
//! compile of c_*.c needs no extra system headers.

use std::collections::{BTreeMap, HashMap, HashSet};
use std::fs;
use std::path::Path;

const FUNCS_H_PROTOS: &str = r#"
/* [bayonetta2 post_lift] runtime helpers in games/bayonetta2/runtime/game_shim_eh.c */
void recomps_evfix_deleted(Ctx *c, uint64_t entry);
void recomps_evfix_slot(Ctx *c, uint64_t slot);
void recomps_evfix_test(Ctx *c);
void recomps_prompt_ring(uint64_t at, uint32_t mask);
int recomps_uncap_on(void);
float recomps_uncap_min(void);
float recomps_uncap_f(uint64_t a);
void recomps_uncap_limit(void);
void recomps_uncap_tick(Ctx *c, float step);
extern int recomps_fps_tick;
uint32_t recomps_fps_ci(Ctx *c, uint64_t key, uint64_t field, uint32_t wb, int k, int sign);
int recomps_fps_cf(Ctx *c, uint64_t key, uint64_t field, float w, int sign);
float recomps_fps_fs(Ctx *c, uint64_t key, uint64_t field);
float recomps_fps_lmul(Ctx *c, int slot, uint64_t key, uint64_t field, float a);
float recomps_fps_lsum(Ctx *c, int slot);
void recomps_fps_reload(Ctx *c, int C, int X);
float recomps_fps_smk(Ctx *c, float k, uint64_t key, uint64_t field);
int recomps_cine_fix_on(void);
void recomps_cine_shot_end(Ctx *c);
void recomps_cine_last_hold(Ctx *c);
int recomps_cine_end_early(Ctx *c);
int recomps_slowmo_fix(void);
void recomps_slowmo_timer(Ctx *c);
int recomps_slowmo_align(Ctx *c);
int recomps_movie_fix_on(void);
void recomps_movie_timeout_step(Ctx *c);
unsigned recomps_uw_width(void);
void recomps_ach_tick(Ctx *c);
void recomps_ach_live_eval(Ctx *c);
"#;

const FPS_TICKS_TXT: &str = include_str!("../lifter/fps_ticks.txt");
const FPS_SMOOTH_TXT: &str = include_str!("../lifter/fps_smooth.txt");

// ---- tiny zero-dependency matchers for the exact C statements the lifter emits ----

fn label_of(line: &str) -> Option<&str> {
    let rest = line.strip_prefix("L_")?;
    let colon = rest.find(':')?;
    let hex = &rest[..colon];
    if !hex.is_empty() && hex.bytes().all(|b| b.is_ascii_hexdigit()) {
        Some(hex)
    } else {
        None
    }
}

fn body_of(line: &str) -> Option<&str> {
    let rest = line.strip_prefix("L_")?;
    let colon = rest.find(": ")?;
    Some(&rest[colon + 2..])
}

fn parse_u32_prefix(s: &str) -> Option<(u32, &str)> {
    let n = s.bytes().take_while(|b| b.is_ascii_digit()).count();
    if n == 0 {
        return None;
    }
    Some((s[..n].parse().ok()?, &s[n..]))
}

fn parse_hex_u64_prefix(s: &str) -> Option<(u64, &str)> {
    let s = s.strip_prefix("0x")?;
    let n = s.bytes().take_while(|b| b.is_ascii_hexdigit()).count();
    if n == 0 {
        return None;
    }
    Some((u64::from_str_radix(&s[..n], 16).ok()?, &s[n..]))
}

fn is_call(line: &str) -> bool {
    if line.contains("call_indirect") {
        return true;
    }
    let bytes = line.as_bytes();
    let mut i = 0;
    while let Some(pos) = line[i..].find("f_") {
        let p = i + pos;
        let boundary = p == 0 || !bytes[p - 1].is_ascii_alphanumeric() && bytes[p - 1] != b'_';
        if boundary {
            let after = &line[p + 2..];
            let hex = after.bytes().take_while(|b| b.is_ascii_hexdigit()).count();
            if hex > 0 && after[hex..].starts_with("(c)") {
                return true;
            }
        }
        i = p + 2;
    }
    false
}

fn is_stop(line: &str) -> bool {
    is_call(line) || line.contains("return") || line.contains("goto ")
}

/// Match: `{ float t = fnan32(c->v[A].f[0] - c->v[B].f[0]); vzero(&c->v[S]); c->v[S].f[0] = t; }`
fn match_fsub(line: &str) -> Option<(u32, u32, u32)> {
    let b = body_of(line)?;
    let r = b.strip_prefix("{ float t = fnan32(c->v[")?;
    let (va, r) = parse_u32_prefix(r)?;
    let r = r.strip_prefix("].f[0] - c->v[")?;
    let (vb, r) = parse_u32_prefix(r)?;
    let r = r.strip_prefix("].f[0]); vzero(&c->v[")?;
    let (vs, r) = parse_u32_prefix(r)?;
    let r = r.strip_prefix("]); c->v[")?;
    let (vs2, r) = parse_u32_prefix(r)?;
    if vs == vs2 && r == "].f[0] = t; }" {
        Some((va, vb, vs))
    } else {
        None
    }
}

/// Match: `fcmp(c, (double)c->v[S].f[0], (double)0);` or `fcmpe(...)`
fn match_fcmp0(line: &str, s: u32) -> bool {
    let Some(b) = body_of(line) else { return false };
    b == format!("fcmp(c, (double)c->v[{}].f[0], (double)0);", s)
        || b == format!("fcmpe(c, (double)c->v[{}].f[0], (double)0);", s)
}

/// Match: `{  uint64_t a = c->x[BASE] + OFFULL; wrv(a, &c->v[S], 4);  }` -> (base, off)
fn match_stv(line: &str, s: u32) -> Option<(u32, u64)> {
    let b = body_of(line)?;
    let r = b.strip_prefix("{  uint64_t a = c->x[")?;
    let (base, r) = parse_u32_prefix(r)?;
    let r = r.strip_prefix("] + ")?;
    let n = r.bytes().take_while(|c| c.is_ascii_digit()).count();
    let off: u64 = r[..n].parse().ok()?;
    let r = &r[n..];
    if r == format!("ULL; wrv(a, &c->v[{}], 4);  }}", s) {
        Some((base, off))
    } else {
        None
    }
}

/// Match: `{  uint64_t a = c->x[BASE] + OFFULL; c->v[R] = rdv(a, 4);  }`
fn match_ldv(line: &str, base: u32, off: u64, reg: u32) -> bool {
    body_of(line) == Some(format!("{{  uint64_t a = c->x[{}] + {}ULL; c->v[{}] = rdv(a, 4);  }}", base, off, reg).as_str())
}

/// Match: `{  uint64_t a = c->x[BASE] + OFFULL; c->x[R] = (uint64_t)(rd32(a));  }`
fn match_ldx(line: &str, base: u32, off: u64, reg: u32) -> bool {
    body_of(line) == Some(format!("{{  uint64_t a = c->x[{}] + {}ULL; c->x[{}] = (uint64_t)(rd32(a));  }}", base, off, reg).as_str())
}

/// Match: `if (cond(c, 8)) goto L_...;` or `cond(c, 12)`
fn match_br_gt(line: &str) -> bool {
    let Some(b) = body_of(line) else { return false };
    (b.starts_with("if (cond(c, 8)) goto L_") || b.starts_with("if (cond(c, 12)) goto L_")) && b.ends_with(';')
}

/// Match: `c->x[R] = (uint32_t)(0x...ULL);` or `c->x[R] = (uint32_t)(0U | 0x...ULL);`
fn match_movw(line: &str) -> Option<(u32, u64)> {
    let b = body_of(line)?;
    let r = b.strip_prefix("c->x[")?;
    let (reg, r) = parse_u32_prefix(r)?;
    let r = r.strip_prefix("] = (uint32_t)(")?;
    let r = r.strip_prefix("0U | ").unwrap_or(r);
    let (val, r) = parse_hex_u64_prefix(r)?;
    if r == "ULL);" {
        Some((reg, val))
    } else {
        None
    }
}

/// Match: `c->x[R] = (uint32_t)(((uint32_t)c->x[R] & ~0xffffULL) | 0x...ULL);`
fn match_movk(line: &str, reg: u32) -> Option<u64> {
    let b = body_of(line)?;
    let pfx = format!("c->x[{}] = (uint32_t)(((uint32_t)c->x[{}] & ~0xffffULL) | ", reg, reg);
    let r = b.strip_prefix(&pfx)?;
    let (val, r) = parse_hex_u64_prefix(r)?;
    if r == "ULL);" {
        Some(val)
    } else {
        None
    }
}

/// Match: `{  uint64_t a = c->x[BASE] + OFFULL; wr32(a, (uint32_t)c->x[R]);  }`
fn match_stw(line: &str, base: u32, off: u64, reg: u32) -> bool {
    body_of(line) == Some(format!("{{  uint64_t a = c->x[{}] + {}ULL; wr32(a, (uint32_t)c->x[{}]);  }}", base, off, reg).as_str())
}

/// Match store field `(base, off)` of `wr32` or `wrv(..., 4)`
fn store_field(line: &str) -> Result<(u32, u64), String> {
    let err = || format!("post_lift: not a store to c->x[n] + imm: {}", line.trim());
    let idx = line.find("{  uint64_t a = c->x[").ok_or_else(err)?;
    let r = &line[idx + "{  uint64_t a = c->x[".len()..];
    let (base, r) = parse_u32_prefix(r).ok_or_else(err)?;
    let r = r.strip_prefix("] + ").ok_or_else(err)?;
    let n = r.bytes().take_while(|b| b.is_ascii_digit()).count();
    if n == 0 {
        return Err(err());
    }
    let off: u64 = r[..n].parse().map_err(|_| err())?;
    let r = &r[n..];
    if !r.starts_with("ULL; wr32(a, (uint32_t)c->x[") && !r.starts_with("ULL; wrv(a, &c->v[") {
        return Err(err());
    }
    Ok((base, off))
}

fn assigns_x(line: &str, reg: u32) -> bool {
    body_of(line).is_some_and(|b| b.starts_with(&format!("c->x[{}] = ", reg)))
}

fn match_vdef(line: &str, reg: u32) -> bool {
    line.contains(&format!("c->v[{}].f[0] = t;", reg))
        || line.contains(&format!("c->v[{}] = rdv", reg))
        || line.contains(&format!("vzero(&c->v[{}]);", reg))
        || line.contains(&format!("c->v[{}] = t;", reg))
        || line.contains(&format!("c->v[{}] = t1;", reg))
        || line.contains(&format!("c->v[{}] = t2;", reg))
}

fn check_base(lines: &[String], a: usize, b: usize, base: u32, what: &str) -> Result<(), String> {
    for line in &lines[a..b] {
        if assigns_x(line, base) || is_call(line) {
            return Err(format!("post_lift: {}: x{} changes before store: {}", what, base, line.trim()));
        }
    }
    Ok(())
}

/// Finding 136: find all float timer reload sites in one file's pristine lines.
fn find_reloads(lines: &[String]) -> Vec<(usize, u32, u32)> {
    let mut jumped: HashSet<String> = HashSet::new();
    for l in lines {
        if !l.trim_start().starts_with("case 0x") {
            let mut rest = l.as_str();
            while let Some(p) = rest.find("goto L_") {
                let after = &rest[p + 7..];
                let n = after.bytes().take_while(|b| b.is_ascii_hexdigit()).count();
                if n > 0 && after[n..].starts_with(';') {
                    jumped.insert(after[..n].to_string());
                }
                rest = &after[n..];
            }
        }
    }
    let mut out = Vec::new();
    let mut fstart = 0usize;
    for i in 0..lines.len() {
        let l = &lines[i];
        if l.starts_with("void f_") || l.starts_with("static void f_") {
            fstart = i;
        }
        let Some((va, _vb, vs)) = match_fsub(l) else { continue };
        let (mut k, mut cmp_ok, mut st_ok, mut base, mut off) = (i + 1, false, false, 0u32, 0u64);
        while k < lines.len().min(i + 3) && !(cmp_ok && st_ok) {
            if match_fcmp0(&lines[k], vs) {
                cmp_ok = true;
            } else if let Some((b, o)) = match_stv(&lines[k], vs) {
                base = b;
                off = o;
                st_ok = true;
            } else {
                break;
            }
            k += 1;
        }
        if !(cmp_ok && st_ok) || k >= lines.len() || !match_br_gt(&lines[k]) {
            continue;
        }
        let lo = fstart.max(i.saturating_sub(10));
        if !(lo..i).any(|j| match_ldv(&lines[j], base, off, va)) {
            continue;
        }
        let mut found: Option<(u32, f32)> = None;
        let v_pat = format!("c->v[{}]", vs);
        for j in (k + 1)..lines.len().min(k + 9) {
            let Some(lab) = label_of(&lines[j]) else { break };
            if jumped.contains(lab) || is_stop(&lines[j]) || lines[j].contains(&v_pat) {
                break;
            }
            if found.is_none() {
                if let Some((reg, mut val)) = match_movw(&lines[j]) {
                    if j + 1 < lines.len() {
                        if let Some(lo16) = match_movk(&lines[j + 1], reg) {
                            val = (val & 0xffff_0000) | lo16;
                        }
                    }
                    let p = f32::from_bits((val & 0xffff_ffff) as u32);
                    found = Some((reg, p));
                    continue;
                }
            }
            if let Some((reg, p)) = found {
                if match_stw(&lines[j], base, off, reg) {
                    if (0.01..=100000.0).contains(&p) {
                        out.push((j, reg, vs));
                    }
                    break;
                }
                if assigns_x(&lines[j], reg) && match_movk(&lines[j], reg).is_none() {
                    break;
                }
            }
        }
    }
    out
}

fn func_range(lines: &[String], fn_hex: &str) -> Result<(usize, usize), String> {
    let pat = format!("void f_{}(", fn_hex);
    let start = lines
        .iter()
        .position(|l| l.starts_with(&pat))
        .ok_or_else(|| format!("post_lift: function f_{} not found", fn_hex))?;
    let end = (start..lines.len())
        .find(|&i| lines[i].starts_with('}'))
        .ok_or_else(|| format!("post_lift: closing brace for f_{} not found", fn_hex))?;
    Ok((start, end))
}

fn replace_once(lines: &mut [String], prefix: &str, repl_prefix: &str) -> Result<(), String> {
    let hits: Vec<usize> = lines.iter().enumerate().filter(|(_, l)| l.starts_with(prefix)).map(|(i, _)| i).collect();
    if hits.len() != 1 {
        return Err(format!("post_lift: expected 1 line starting with {:?}, found {}", prefix, hits.len()));
    }
    let i = hits[0];
    let rest = lines[i][prefix.len()..].to_string();
    lines[i] = format!("{}{}", repl_prefix, rest);
    Ok(())
}

/// Patch integer counter (+/- 1 or 2) feeding store at `lines[i]`.
fn patch_int(lines: &mut [String], lo: usize, i: usize, key: u64) -> Result<(), String> {
    let what = lines[i].split(':').next().unwrap_or("?").to_string();
    let store_s = lines[i].as_str();
    let wr_pos = store_s.find("wr32(a, (uint32_t)c->x[").ok_or_else(|| format!("post_lift: {}: expected wr32 store", what))?;
    let (mut reg, _) = parse_u32_prefix(&store_s[wr_pos + "wr32(a, (uint32_t)c->x[".len()..])
        .ok_or_else(|| format!("post_lift: {}: bad store reg", what))?;
    let (base, off) = store_field(store_s)?;

    let k_lo = lo.max(i.saturating_sub(14));
    for k in (k_lo..i).rev() {
        if !assigns_x(&lines[k], reg) {
            continue;
        }
        // Check MOV: `= (uint32_t)(0U | (uint32_t)c->x[M]);` or `= (uint64_t)(0ULL | c->x[M]);`
        if let Some(b) = body_of(&lines[k]) {
            if let Some(mv) = b.strip_prefix(&format!("c->x[{}] = (uint32_t)(0U | (uint32_t)c->x[", reg))
                .or_else(|| b.strip_prefix(&format!("c->x[{}] = (uint32_t)(0U | c->x[", reg)))
                .or_else(|| b.strip_prefix(&format!("c->x[{}] = (uint64_t)(0ULL | c->x[", reg)))
            {
                if let Some((mreg, tail)) = parse_u32_prefix(mv) {
                    if tail == "]);" {
                        reg = mreg;
                        continue;
                    }
                }
            }
        }
        // Match one of the 4 integer counter op forms
        let line = lines[k].clone();
        let matched: Option<(u32, u32, i32, &str)> = (|| {
            for imm in [1u32, 2u32] {
                if let Some(p) = line.find("(uint32_t)((uint32_t)c->x[") {
                    let r = &line[p + "(uint32_t)((uint32_t)c->x[".len()..];
                    if let Some((src, t)) = parse_u32_prefix(r) {
                        if t.starts_with(&format!("] + {}ULL)", imm)) {
                            return Some((src, imm, 1, "add"));
                        }
                        if t.starts_with(&format!("] - {}ULL)", imm)) {
                            return Some((src, imm, -1, "sub"));
                        }
                    }
                }
                if let Some(p) = line.find("adds32(c, (uint32_t)c->x[") {
                    let r = &line[p + "adds32(c, (uint32_t)c->x[".len()..];
                    if let Some((src, t)) = parse_u32_prefix(r) {
                        if t.starts_with(&format!("], ~(uint32_t){}ULL, 1)", imm)) {
                            return Some((src, imm, -1, "subs"));
                        }
                        if t.starts_with(&format!("], (uint32_t){}ULL, 0)", imm)) {
                            return Some((src, imm, 1, "adds"));
                        }
                    }
                }
            }
            None
        })();
        let Some((src, imm, sign, form)) = matched else {
            return Err(format!("post_lift: {}: no +/-1 on line defining x{}: {}", what, reg, line.trim()));
        };
        if reg == base {
            return Err(format!("post_lift: {}: counter is its own base register", what));
        }
        check_base(lines, k + 1, i, base, &what)?;
        let load_lo = lo.max(k.saturating_sub(12));
        if !(load_lo..k).any(|j| match_ldx(&lines[j], base, off, src)) {
            return Err(format!("post_lift: {}: stepped value not loaded from stored field", what));
        }
        let call = format!(
            "recomps_fps_ci(c, 0x{:x}ULL, c->x[{}] + {}ULL, (uint32_t)c->x[{}], {}, {})",
            key, base, off, src, imm, sign
        );
        let new_line = match form {
            "add" => line.replacen(&format!("+ {}ULL)", imm), &format!("+ (uint64_t){})", call), 1),
            "sub" => line.replacen(&format!("- {}ULL)", imm), &format!("- (uint64_t){})", call), 1),
            "subs" => line.replacen(&format!("~(uint32_t){}ULL, 1)", imm), &format!("~(uint32_t){}, 1)", call), 1),
            "adds" => line.replacen(&format!("(uint32_t){}ULL, 0)", imm), &format!("(uint32_t){}, 0)", call), 1),
            _ => unreachable!(),
        };
        if new_line == line {
            return Err(format!("post_lift: {}: replacement had no effect", what));
        }
        lines[k] = new_line;
        return Ok(());
    }
    Err(format!("post_lift: {}: no definition of x{} found", what, reg))
}

/// Extract `(r1, op_char, r2)` from `fnan32(c->v[R1].f[0] +/- c->v[R2].f[0])`
fn parse_faddsub_line(line: &str) -> Option<(u32, char, u32)> {
    let p = line.find("fnan32(c->v[")?;
    let r = &line[p + "fnan32(c->v[".len()..];
    let (r1, r) = parse_u32_prefix(r)?;
    let (op, r) = if let Some(rest) = r.strip_prefix("].f[0] + c->v[") {
        ('+', rest)
    } else if let Some(rest) = r.strip_prefix("].f[0] - c->v[") {
        ('-', rest)
    } else {
        return None;
    };
    let (r2, r) = parse_u32_prefix(r)?;
    if r.starts_with("].f[0])") {
        Some((r1, op, r2))
    } else {
        None
    }
}

fn replace_in_fnan32(line: &str, old: &str, rep: &str) -> Result<String, String> {
    let p = line.find("fnan32(").ok_or_else(|| format!("post_lift: missing fnan32 in {}", line))?;
    let (a, b) = line.split_at(p + "fnan32(".len());
    let b2 = b.replacen(old, rep, 1);
    if b2 == b {
        return Err(format!("post_lift: {:?} not found in fnan32 of {}", old, line));
    }
    Ok(format!("{}{}", a, b2))
}

fn patch_f1(
    lines: &mut [String],
    lo: usize,
    hi: usize,
    mut i: usize,
    key: u64,
    op_mode: bool,
    lin_slot: Option<usize>,
    anystep: bool,
) -> Result<(), String> {
    let what = lines[i].split(':').next().unwrap_or("?").to_string();
    let j = if op_mode {
        let p = lines[i].find("vzero(&c->v[").ok_or_else(|| format!("post_lift: {}: no vzero", what))?;
        let (d, _) = parse_u32_prefix(&lines[i][p + "vzero(&c->v[".len()..]).ok_or_else(|| format!("post_lift: {}: bad vzero", what))?;
        let store_pat = format!("wrv(a, &c->v[{}], 4)", d);
        let st = ((i + 1)..hi.min(i + 8))
            .find(|&k| lines[k].contains(&store_pat))
            .ok_or_else(|| format!("post_lift: {}: store for v{} not found", what, d))?;
        let op_idx = i;
        i = st;
        op_idx
    } else {
        let p = lines[i].find("wrv(a, &c->v[").ok_or_else(|| format!("post_lift: {}: no wrv", what))?;
        let (v, _) = parse_u32_prefix(&lines[i][p + "wrv(a, &c->v[".len()..]).ok_or_else(|| format!("post_lift: {}: bad wrv", what))?;
        ((lo + 1)..i)
            .rev()
            .find(|&k| match_vdef(&lines[k], v))
            .ok_or_else(|| format!("post_lift: {}: no v{} def before store", what, v))?
    };

    let (base, off) = store_field(&lines[i])?;
    let (r1, op, r2) = parse_faddsub_line(&lines[j]).ok_or_else(|| format!("post_lift: {}: no float add: {}", what, lines[j].trim()))?;

    if anystep {
        if op != '+' {
            return Err(format!("post_lift: {}: f1step expects +", what));
        }
        for (step, x) in [(r2, r1), (r1, r2)] {
            let mut loaded = false;
            let k_lo = lo.max(j.saturating_sub(24));
            for k in (k_lo..j).rev() {
                if match_ldv(&lines[k], base, off, x) {
                    loaded = true;
                    break;
                }
                if match_vdef(&lines[k], x) || (x < 8 && is_call(&lines[k])) || assigns_x(&lines[k], base) {
                    break;
                }
            }
            if !loaded {
                continue;
            }
            check_base(lines, j, i, base, &what)?;
            let old = format!("c->v[{}].f[0]", step);
            let rep = format!(
                "({} * (float)recomps_fps_cf(c, 0x{:x}ULL, c->x[{}] + {}ULL, c->v[{}].f[0], 1))",
                old, key, base, off, x
            );
            lines[j] = replace_in_fnan32(&lines[j], &old, &rep)?;
            return Ok(());
        }
        return Err(format!("post_lift: {}: f1step field load not found", what));
    }

    for (r, other) in [(r1, r2), (r2, r1)] {
        for k in ((lo + 1)..j).rev() {
            if r < 8 && is_call(&lines[k]) {
                break;
            }
            if !match_vdef(&lines[k], r) {
                continue;
            }
            let c1_sign = if lines[k].contains("fnan32((float)1.0)") {
                1i32
            } else if lines[k].contains("fnan32((float)-1.0)") {
                -1i32
            } else {
                break;
            };
            if op == '-' && r == r1 {
                return Err(format!("post_lift: {}: 1.0 - x is not a counter", what));
            }
            let sign = c1_sign * if op == '-' && r == r2 { -1 } else { 1 };
            let field = format!("c->x[{}] + {}ULL", base, off);
            let old = format!("c->v[{}].f[0]", r);
            let rep = if let Some(slot) = lin_slot {
                let mul = ((lo + 1)..j)
                    .rev()
                    .find(|&q| match_vdef(&lines[q], other))
                    .ok_or_else(|| format!("post_lift: {}: f1lin mul not found", what))?;
                let mp = format!("fnan32(c->v[{}].f[0] * c->v[", other);
                let idx = lines[mul].find(&mp).ok_or_else(|| format!("post_lift: {}: bad f1lin mul: {}", what, lines[mul].trim()))?;
                let (fa_reg, _) = parse_u32_prefix(&lines[mul][idx + mp.len()..]).ok_or_else(|| format!("post_lift: {}: bad fa reg", what))?;
                if fa_reg == other {
                    return Err(format!("post_lift: {}: f1lin self multiply", what));
                }
                check_base(lines, mul, i, base, &what)?;
                let load_lo = lo.max(mul.saturating_sub(12));
                if !(load_lo..mul).any(|q| match_ldv(&lines[q], base, off, other)) {
                    return Err(format!("post_lift: {}: f1lin field load not found", what));
                }
                let fa = format!("c->v[{}].f[0]", fa_reg);
                let lmul = format!("* recomps_fps_lmul(c, {}, 0x{:x}ULL, {}, {})", slot, key, field, fa);
                lines[mul] = replace_in_fnan32(&lines[mul], &format!("* {}", fa), &lmul)?;
                format!("({} * recomps_fps_lsum(c, {}))", old, slot)
            } else {
                check_base(lines, j, i, base, &what)?;
                let load_lo = lo.max(j.saturating_sub(12));
                if !(load_lo..j).any(|q| match_ldv(&lines[q], base, off, other)) {
                    return Err(format!("post_lift: {}: float counter field load not found", what));
                }
                format!("({} * (float)recomps_fps_cf(c, 0x{:x}ULL, {}, c->v[{}].f[0], {}))", old, key, field, other, sign)
            };
            lines[j] = replace_in_fnan32(&lines[j], &old, &rep)?;
            return Ok(());
        }
    }
    Err(format!("post_lift: {}: float add has no constant 1.0 operand: {}", what, lines[j].trim()))
}

fn parse_reg_tok(tok: &str, prefix: char) -> Option<u32> {
    let rest = tok.strip_prefix(prefix)?;
    if rest.is_empty() || !rest.bytes().all(|b| b.is_ascii_digit()) {
        return None;
    }
    rest.parse().ok()
}

/// Patch a continuous float step (`fs` store-labeled or `fsop` op-labeled) using `recomps_fps_fs`.
/// Scales any constant or loaded step (e.g. `±1/60.0f` second-based timers, `±1.0f` animation phases,
/// or per-frame rotation/fade deltas) by the continuous 60 Hz frame step so high-FPS motion stays smooth
/// and runs at true 60 FPS speed.
fn patch_fs(
    lines: &mut [String],
    lo: usize,
    hi: usize,
    mut i: usize,
    key: u64,
    op_mode: bool,
    step_reg: Option<u32>,
    field_reg: Option<u32>,
) -> Result<(), String> {
    let what = lines[i].split(':').next().unwrap_or("?").to_string();
    let (j, field_expr) = if let Some(freg) = field_reg {
        if !op_mode {
            return Err(format!("post_lift: {}: field register x{} requires fsop", what, freg));
        }
        (i, format!("c->x[{}]", freg))
    } else {
        let j = if op_mode {
            let p = lines[i].find("vzero(&c->v[").ok_or_else(|| format!("post_lift: {}: no vzero", what))?;
            let (d, _) = parse_u32_prefix(&lines[i][p + "vzero(&c->v[".len()..])
                .ok_or_else(|| format!("post_lift: {}: bad vzero", what))?;
            let store_pat = format!("wrv(a, &c->v[{}], 4)", d);
            let st = ((i + 1)..hi.min(i + 16))
                .find(|&k| lines[k].contains(&store_pat))
                .ok_or_else(|| format!("post_lift: {}: store for v{} not found", what, d))?;
            let op_idx = i;
            i = st;
            op_idx
        } else {
            let p = lines[i].find("wrv(a, &c->v[").ok_or_else(|| format!("post_lift: {}: no wrv", what))?;
            let (v, _) = parse_u32_prefix(&lines[i][p + "wrv(a, &c->v[".len()..])
                .ok_or_else(|| format!("post_lift: {}: bad wrv", what))?;
            ((lo + 1)..i)
                .rev()
                .find(|&k| match_vdef(&lines[k], v))
                .ok_or_else(|| format!("post_lift: {}: no v{} def before store", what, v))?
        };
        let (base, off) = store_field(&lines[i])?;
        check_base(lines, j, i, base, &what)?;
        (j, format!("c->x[{}] + {}ULL", base, off))
    };

    let (r1, op, r2) = parse_faddsub_line(&lines[j])
        .ok_or_else(|| format!("post_lift: {}: no float add/sub: {}", what, lines[j].trim()))?;

    let step = if let Some(sr) = step_reg {
        if sr != r1 && sr != r2 {
            return Err(format!("post_lift: {}: step reg v{} not an operand of {}", what, sr, lines[j].trim()));
        }
        if op == '-' && sr == r1 {
            return Err(format!("post_lift: {}: step - x is not a counter", what));
        }
        sr
    } else {
        let (base, off) = store_field(&lines[i])?;
        let mut found = None;
        for (st, x) in [(r2, r1), (r1, r2)] {
            if op == '-' && st == r1 {
                continue;
            }
            let k_lo = lo.max(j.saturating_sub(24));
            for k in (k_lo..j).rev() {
                if match_ldv(&lines[k], base, off, x) {
                    found = Some(st);
                    break;
                }
                if match_vdef(&lines[k], x) || (x < 8 && is_call(&lines[k])) || assigns_x(&lines[k], base) {
                    break;
                }
            }
            if found.is_some() {
                break;
            }
        }
        found.ok_or_else(|| format!("post_lift: {}: fs field load not found", what))?
    };

    let old = format!("c->v[{}].f[0]", step);
    let rep = format!("({} * recomps_fps_fs(c, 0x{:x}ULL, {}))", old, key, field_expr);
    lines[j] = replace_in_fnan32(&lines[j], &old, &rep)?;
    Ok(())
}

pub fn post_lift(gen_dir: &Path, log: &mut dyn FnMut(String)) -> Result<(), String> {
    let mut c_files: Vec<String> = fs::read_dir(gen_dir)
        .map_err(|e| format!("{}: {}", gen_dir.display(), e))?
        .flatten()
        .map(|e| e.file_name().to_string_lossy().into_owned())
        .filter(|n| n.starts_with("c_") && n.ends_with(".c"))
        .collect();
    c_files.sort();

    // Load all files' contents once in memory? There are 386 files (~150 MB total).
    // Even better: single scan builds function->file and label->file indexes, plus runs reload scan
    // on each file, keeping only modified files in memory!
    let mut modified: BTreeMap<String, Vec<String>> = BTreeMap::new();
    let mut fn_to_file: HashMap<String, String> = HashMap::new();
    let mut uw_label_to_file: HashMap<String, String> = HashMap::new();

    let uw_set: HashSet<&str> = std::iter::once(UW_WIDTH_LAB)
        .chain(UW_BUFFERS_LABS.iter().copied())
        .chain(UW_HALF_LABS.iter().copied())
        .chain(UW_QUARTER_LABS.iter().copied())
        .chain(std::iter::once(UW_CENTRE_LAB))
        .collect();

    let mut nrl = 0usize;
    let mut rl_files = 0usize;

    for fname in &c_files {
        let text = fs::read_to_string(gen_dir.join(fname)).map_err(|e| format!("{}: {}", fname, e))?;
        let lines: Vec<String> = text.split('\n').map(|s| s.trim_end_matches('\r').to_string()).collect();
        for l in &lines {
            if let Some(rest) = l.strip_prefix("void f_") {
                if let Some(p) = rest.find("(Ctx *c)") {
                    fn_to_file.insert(rest[..p].to_string(), fname.clone());
                }
            } else if let Some(lab) = label_of(l) {
                if uw_set.contains(lab) {
                    uw_label_to_file.insert(lab.to_string(), fname.clone());
                }
            }
        }
        let reloads = find_reloads(&lines);
        if !reloads.is_empty() {
            rl_files += 1;
            let mut mlines = lines;
            for (j, w, sreg) in reloads {
                let lab = mlines[j].split(':').next().unwrap().to_string();
                let body = mlines[j][lab.len() + 1..].trim_start().to_string();
                mlines[j] = format!("{}: recomps_fps_reload(c, {}, {}); {}", lab, w, sreg, body);
                nrl += 1;
            }
            modified.insert(fname.clone(), mlines);
        }
    }

    let load_file = |fname: &str, modified: &mut BTreeMap<String, Vec<String>>| -> Result<(), String> {
        if !modified.contains_key(fname) {
            let text = fs::read_to_string(gen_dir.join(fname)).map_err(|e| format!("{}: {}", fname, e))?;
            let lines: Vec<String> = text.split('\n').map(|s| s.trim_end_matches('\r').to_string()).collect();
            modified.insert(fname.to_string(), lines);
        }
        Ok(())
    };

    let file_of = |fn_hex: &str| -> Result<String, String> {
        fn_to_file
            .get(fn_hex)
            .cloned()
            .ok_or_else(|| format!("post_lift: function f_{} not found in lifted C", fn_hex))
    };

    // ---- 1. Cutscene deleted-character fix (Findings 146 & 150) ----
    for (fn_hex, call) in [
        ("71009a6d1c", "  recomps_evfix_deleted(c, c->x[0]);   /* [event_fix] */"),
        ("7100e27e1c", "  recomps_evfix_slot(c, c->x[0]);   /* [event_fix] */"),
        ("7100e19d80", "  recomps_evfix_test(c);   /* [event_fix] */"),
    ] {
        let fname = file_of(fn_hex)?;
        load_file(&fname, &mut modified)?;
        let ls = modified.get_mut(&fname).unwrap();
        let (lo, _) = func_range(ls, fn_hex)?;
        let expected = format!("  c->fn = 0x{}ULL;", fn_hex);
        if ls.get(lo + 1).map(|s| s.as_str()) != Some(expected.as_str()) {
            return Err(format!("post_lift: f_{} entry mismatch in {}", fn_hex, fname));
        }
        ls.insert(lo + 2, call.to_string());
    }

    // ---- 2. Action prompt ring angle (Finding 148) ----
    for (fn_hex, label, store, mask) in [
        ("7100c018b0", "7100c01b64", "{  uint64_t a = c->x[8] + 444ULL; wrv(a, &c->v[8], 4);  }", "rd32(c->x[20] + 20ULL)"),
        ("7100c018b0", "7100c01d0c", "{  uint64_t a = c->x[8] + 444ULL; wrv(a, &c->v[0], 4);  }", "rd32(c->x[20] + 20ULL)"),
        ("7100c02bf0", "7100c02d24", "{  uint64_t a = c->x[8] + 444ULL; wrv(a, &c->v[8], 4);  }", "rd32(c->x[20] + 20ULL)"),
        ("7100c02ecc", "7100c02fc4", "{  uint64_t a = c->x[8] + 444ULL; wr32(a, (uint32_t)c->x[9]);  }", "0x40"),
        ("7100c02ecc", "7100c03060", "{  uint64_t a = c->x[8] + 444ULL; wr32(a, 0U);  }", "0x10"),
    ] {
        let fname = file_of(fn_hex)?;
        load_file(&fname, &mut modified)?;
        let ls = modified.get_mut(&fname).unwrap();
        let exact = format!("L_{}: {}", label, store);
        let hits: Vec<usize> = ls.iter().enumerate().filter(|(_, l)| *l == &exact).map(|(i, _)| i).collect();
        if hits.len() != 1 {
            return Err(format!("post_lift: prompt_fix expected 1 match for L_{} in {}, found {}", label, fname, hits.len()));
        }
        ls[hits[0]] = format!("{} recomps_prompt_ring(c->x[8] + 444ULL, {});   /* [prompt_fix] */", exact, mask);
    }

    // ---- 3. Frame rate above 60 fps ----
    // 3a. Clock (0x7100dd1d60)
    {
        let fname = file_of("7100dd1d60")?;
        load_file(&fname, &mut modified)?;
        let ls = modified.get_mut(&fname).unwrap();
        replace_once(ls, "L_7100dd1d60: {", "if (recomps_uncap_on()) c->x[1] = 1;\nL_7100dd1d60: {")?;
        replace_once(
            ls,
            "L_7100dd1e00: { float t = fnan32((float)1.0);",
            "L_7100dd1e00: { float t = fnan32(recomps_uncap_on() ? recomps_uncap_min() : (float)1.0);",
        )?;
        replace_once(
            ls,
            "L_7100dd1e14: c->x[8] = (uint32_t)(0U | 0x3f800000ULL);",
            "L_7100dd1e14: if (recomps_uncap_on()) { float m = recomps_uncap_min(); uint32_t b; memcpy(&b, &m, 4); c->x[8] = b; } else c->x[8] = (uint32_t)(0U | 0x3f800000ULL);",
        )?;
        replace_once(
            ls,
            "L_7100dd1e30: {  uint64_t a = c->x[19] + 208ULL; wr64(a, c->x[20]);  }",
            "L_7100dd1e30: {  uint64_t a = c->x[19] + 208ULL; wr64(a, c->x[20]);  }\nif (recomps_uncap_on()) recomps_uncap_tick(c, recomps_uncap_f(c->x[19] + 200));\nrecomps_ach_tick(c);",
        )?;
    }
    // 3b. Present (0x7100ed6ca4) & 30 fps flag (0x7100dc4f4c)
    {
        let fname = file_of("7100ed6ca4")?;
        load_file(&fname, &mut modified)?;
        let ls = modified.get_mut(&fname).unwrap();
        replace_once(ls, "L_7100ed6ca4: {", "if (recomps_uncap_on()) { c->x[2] = 0; recomps_uncap_limit(); }\nL_7100ed6ca4: {")?;
    }
    {
        let fname = file_of("7100dc4f4c")?;
        load_file(&fname, &mut modified)?;
        let ls = modified.get_mut(&fname).unwrap();
        replace_once(ls, "L_7100dc4f60: {", "if (recomps_uncap_on()) c->x[8] = 0;\nL_7100dc4f60: {")?;
    }
    // 3c. Cinematic shot hold (Finding 129)
    {
        let fname = file_of("7100e322e8")?;
        load_file(&fname, &mut modified)?;
        let ls = modified.get_mut(&fname).unwrap();
        for (lab, hook) in [
            ("L_7100e3242c: fcmp", "if (recomps_cine_fix_on() && c->v[0].f[0] >= c->v[1].f[0]) { recomps_cine_shot_end(c); goto L_7100e3244c; }"),
            ("L_7100e32408: fcmp", "if (recomps_cine_fix_on() && c->v[1].f[0] >= c->v[0].f[0]) recomps_cine_last_hold(c);"),
            ("L_7100e327d0: c->x[20] =", "if (recomps_cine_fix_on() && recomps_cine_end_early(c)) goto L_7100e32854;"),
        ] {
            replace_once(ls, lab, &format!("{}\n{}", hook, lab))?;
        }
    }
    // 3d. Time channel 0 slowmo (Finding 144)
    {
        let fname = file_of("7100b55938")?;
        load_file(&fname, &mut modified)?;
        let ls = modified.get_mut(&fname).unwrap();
        for (lab, hook) in [
            ("L_7100b55978: {", "if (recomps_slowmo_fix() >= 2) recomps_slowmo_timer(c);"),
            ("L_7100b55b3c: {", "if (recomps_slowmo_fix()) { if (recomps_slowmo_align(c)) goto L_7100b55ba8; goto L_7100b55b40; }"),
        ] {
            replace_once(ls, lab, &format!("{}\n{}", hook, lab))?;
        }
    }
    // 3e. Movie timeout above 60 fps (B2recomp issue #63 / PR #65)
    {
        let fname = file_of("7100b75dc0")?;
        load_file(&fname, &mut modified)?;
        let ls = modified.get_mut(&fname).unwrap();
        replace_once(
            ls,
            "L_7100b76288: {",
            "if (recomps_movie_fix_on()) recomps_movie_timeout_step(c);\nL_7100b76288: {",
        )?;
    }
    // 3f. Frame counters (fps_ticks.txt)
    let mut nticks = 0usize;
    let mut lin_slots = 0usize;
    for raw in FPS_TICKS_TXT.lines() {
        let s = raw.trim();
        if s.is_empty() || s.starts_with('#') {
            continue;
        }
        let cols: Vec<&str> = s.split_whitespace().collect();
        if cols.len() < 3 {
            continue;
        }
        let (fn_hex, lab, kind) = (cols[0], cols[1], cols[2]);
        let fname = file_of(fn_hex)?;
        load_file(&fname, &mut modified)?;
        let ls = modified.get_mut(&fname).unwrap();
        let (lo, hi) = func_range(ls, fn_hex)?;
        let lab_pfx = format!("{}:", lab);
        let i = (lo..hi)
            .find(|&k| ls[k].starts_with(&lab_pfx))
            .ok_or_else(|| format!("post_lift: {} not found in f_{} ({})", lab, fn_hex, fname))?;
        let key = u64::from_str_radix(lab.strip_prefix("L_").unwrap_or(lab), 16)
            .map_err(|_| format!("post_lift: bad label {}", lab))?;
        match kind {
            "int" => patch_int(ls, lo, i, key)?,
            "f1" => patch_f1(ls, lo, hi, i, key, false, None, false)?,
            "f1op" => patch_f1(ls, lo, hi, i, key, true, None, false)?,
            "f1step" => patch_f1(ls, lo, hi, i, key, false, None, true)?,
            "f1lin" => {
                patch_f1(ls, lo, hi, i, key, false, Some(lin_slots), false)?;
                lin_slots += 1;
            }
            "fs" | "fsop" => {
                let op_mode = kind == "fsop";
                let mut step_reg = None;
                let mut field_reg = None;
                for &tok in cols.iter().skip(3).take(2) {
                    if step_reg.is_none() {
                        if let Some(v) = parse_reg_tok(tok, 'v') {
                            step_reg = Some(v);
                            continue;
                        }
                    }
                    if field_reg.is_none() {
                        if let Some(x) = parse_reg_tok(tok, 'x') {
                            field_reg = Some(x);
                            continue;
                        }
                    }
                    break;
                }
                patch_fs(ls, lo, hi, i, key, op_mode, step_reg, field_reg)?;
            }
            _ => return Err(format!("post_lift: {} {}: unknown kind {}", fn_hex, lab, kind)),
        }
        nticks += 1;
    }
    if lin_slots > 4 {
        return Err("post_lift: more f1lin sites than fps_lin slots".into());
    }
    // 3g. Smoothings (fps_smooth.txt)
    let mut nsmooth = 0usize;
    for raw in FPS_SMOOTH_TXT.lines() {
        let s = raw.trim();
        if s.is_empty() || s.starts_with('#') {
            continue;
        }
        let cols: Vec<&str> = s.split_whitespace().collect();
        if cols.len() < 5 {
            continue;
        }
        let (fn_hex, stl, mul, kreg) = (cols[0], cols[1], cols[2], cols[3]);
        let fname = file_of(fn_hex)?;
        load_file(&fname, &mut modified)?;
        let ls = modified.get_mut(&fname).unwrap();
        let (lo, hi) = func_range(ls, fn_hex)?;
        let mi = (lo..hi)
            .find(|&k| ls[k].starts_with(&format!("{}:", mul)))
            .ok_or_else(|| format!("post_lift: smooth mul {} not found in f_{}", mul, fn_hex))?;
        let si = (lo..hi)
            .find(|&k| ls[k].starts_with(&format!("{}:", stl)))
            .ok_or_else(|| format!("post_lift: smooth store {} not found in f_{}", stl, fn_hex))?;
        let (base, off) = store_field(&ls[si])?;
        let kr: u32 = kreg.strip_prefix('v').ok_or_else(|| format!("post_lift: bad kreg {}", kreg))?
            .parse().map_err(|_| format!("post_lift: bad kreg {}", kreg))?;
        check_base(ls, mi, si, base, mul)?;
        let old_k = format!("c->v[{}].f[0]", kr);
        let rep = format!(
            "recomps_fps_smk(c, {}, 0x{}ULL, c->x[{}] + {}ULL)",
            old_k,
            stl.strip_prefix("L_").unwrap_or(stl),
            base,
            off
        );
        ls[mi] = replace_in_fnan32(&ls[mi], &old_k, &rep)?;
        nsmooth += 1;
    }

    // ---- 4. Wide screens (21:9 / 32:9) ----
    let uw_labels: Vec<&str> = std::iter::once(UW_WIDTH_LAB)
        .chain(UW_BUFFERS_LABS.iter().copied())
        .chain(UW_HALF_LABS.iter().copied())
        .chain(UW_QUARTER_LABS.iter().copied())
        .chain(std::iter::once(UW_CENTRE_LAB))
        .collect();
    let mut nuw = 0usize;
    for &lab in &uw_labels {
        let fname = uw_label_to_file
            .get(lab)
            .cloned()
            .ok_or_else(|| format!("post_lift: ultrawide site L_{} not found", lab))?;
        load_file(&fname, &mut modified)?;
        let ls = modified.get_mut(&fname).unwrap();
        let pfx = format!("L_{}:", lab);
        let i = ls
            .iter()
            .position(|l| l.starts_with(&pfx))
            .ok_or_else(|| format!("post_lift: L_{} vanished from {}", lab, fname))?;
        ls[i] = patch_uw_line(lab, &ls[i])?;
        nuw += 1;
    }

    // ---- 5. Menu confirm/cancel swap for hardcoded dialogs (swap_confirm) ----
    let mut nswap = 0usize;
    for &(fn_hex, lab, bit, cmp_op, mask_addr) in SWAP_CONFIRM_SITES {
        let fname = file_of(fn_hex)?;
        load_file(&fname, &mut modified)?;
        let ls = modified.get_mut(&fname).unwrap();
        let (lo, hi) = func_range(ls, fn_hex)?;
        let pfx = format!("L_{}:", lab);
        let i = (lo..hi)
            .find(|&k| ls[k].starts_with(&pfx))
            .ok_or_else(|| format!("post_lift: swap_confirm site L_{} not found in f_{} ({})", lab, fn_hex, fname))?;
        ls[i] = patch_swap_confirm_line(lab, bit, cmp_op, mask_addr, &ls[i])?;
        nswap += 1;
    }

    // ---- 6. Recomp Achievements live unlock hooks ----
    let mut nach = 0usize;
    for &(fn_hex, lab, store) in ACH_LIVE_SITES {
        let fname = file_of(fn_hex)?;
        load_file(&fname, &mut modified)?;
        let ls = modified.get_mut(&fname).unwrap();
        let (lo, hi) = func_range(ls, fn_hex)?;
        let pfx = format!("L_{}:", lab);
        let i = (lo..hi)
            .find(|&k| ls[k].starts_with(&pfx))
            .ok_or_else(|| format!("post_lift: ach_live site L_{} not found in f_{} ({})", lab, fn_hex, fname))?;
        ls[i] = patch_ach_live_line(lab, store, &ls[i])?;
        nach += 1;
    }

    // Write modified c_*.c files
    for (fname, lines) in &modified {
        let out = lines.join("\n");
        fs::write(gen_dir.join(fname), out).map_err(|e| format!("{}: {}", fname, e))?;
    }

    // Add prototypes to funcs.h
    let funcs_h_path = gen_dir.join("funcs.h");
    let mut funcs_h = fs::read_to_string(&funcs_h_path).map_err(|e| format!("funcs.h: {}", e))?;
    if !funcs_h.contains("/* [bayonetta2 post_lift]") {
        funcs_h.push_str(FUNCS_H_PROTOS);
        fs::write(&funcs_h_path, funcs_h).map_err(|e| format!("funcs.h: {}", e))?;
    }

    log(format!(
        "bayonetta2 post_lift: {} files patched (event_fix 3 hooks, prompt_fix 5 stores, fps: {} counters + {} reloads in {} files + {} smoothings + clock/present/cine/slowmo/movie, ultrawide {} sites, swap_confirm {} sites, ach_live {} sites)",
        modified.len(),
        nticks,
        nrl,
        rl_files,
        nsmooth,
        nuw,
        nswap,
        nach,
    ));
    Ok(())
}

/// Live achievement evaluation sites (after save_obj state mutations):
///   - f_7100d943b4: chapter record clear / Play History commit (writes slot + 68 = 3)
///   - f_7100d9045c: story Bewitchment unlock (sets bit in save_obj + 432)
///   - f_7100d8fa50: weapon unlock (sets bit in save_obj + 0xa46c)
///   - f_7100d96c68: alternate weapon unlock (sets bit in save_obj + 0xa46c)
const ACH_LIVE_SITES: &[(&str, &str, &str)] = &[
    ("7100d943b4", "7100d9440c", "{  uint64_t a = c->x[8] + 68ULL; wr32(a, (uint32_t)c->x[9]);  }"),
    ("7100d9045c", "7100d90494", "{  uint64_t a = c->x[8] + 0ULL; wr32(a, (uint32_t)c->x[9]);  }"),
    ("7100d8fa50", "7100d8fa70", "{  uint64_t a = c->x[8] + ((uint64_t)c->x[10]); wr32(a, (uint32_t)c->x[11]);  }"),
    ("7100d96c68", "7100d96ce0", "{  uint64_t a = c->x[12] + ((uint64_t)c->x[11]); wr32(a, (uint32_t)c->x[10]);  }"),
];

fn patch_ach_live_line(lab: &str, store: &str, old: &str) -> Result<String, String> {
    let orig = format!("L_{}: {}", lab, store);
    let patched = format!("{} recomps_ach_live_eval(c);   /* [ach_live] */", orig);
    if old == orig || old == patched {
        Ok(patched)
    } else {
        Err(format!("post_lift: unexpected ach_live line at L_{}: {}", lab, old))
    }
}

/// Hardcoded Pro A (bit 4) / Pro B (bit 5) checks in UI dialogs that bypassed
/// Bayonetta 2's global confirm/cancel bitmasks (0x71018e4810 / 0x71018e4814):
///   - f_71007c649c: Muspelheim & Gates of Hell teleporter Yes/No dialog
///   - f_7100b7a29c: Chapter 1 Gates of Hell telephone Yes/No dialog
///   - f_7100b7a624: Chapter 1 Gates of Hell telephone notice dialog
const SWAP_CONFIRM_SITES: &[(&str, &str, u32, &str, u64)] = &[
    ("71007c649c", "71007c65a0", 5, "!=", 0x71018e4814), // portal dialog cancel
    ("71007c649c", "71007c65f8", 4, "==", 0x71018e4810), // portal dialog confirm
    ("7100b7a29c", "7100b7a46c", 5, "!=", 0x71018e4814), // telephone dialog cancel
    ("7100b7a29c", "7100b7a4d0", 4, "==", 0x71018e4810), // telephone dialog confirm
    ("7100b7a624", "7100b7a768", 4, "==", 0x71018e4810), // telephone notice confirm
];

fn patch_swap_confirm_line(
    lab: &str,
    bit: u32,
    cmp_op: &str,
    mask_addr: u64,
    old: &str,
) -> Result<String, String> {
    let orig_pfx = format!("L_{}: if (((c->x[8] >> {}) & 1) {} 0) ", lab, bit, cmp_op);
    let patched_pfx = format!("L_{}: if (((uint32_t)c->x[8] & rd32(0x{:x}ULL)) {} 0) ", lab, mask_addr, cmp_op);
    let rest = old
        .strip_prefix(&orig_pfx)
        .or_else(|| old.strip_prefix(&patched_pfx))
        .ok_or_else(|| format!("post_lift: unexpected swap_confirm line at L_{}: {}", lab, old))?;
    let goto_stmt = rest.strip_suffix("   /* [swap_confirm] */").unwrap_or(rest);
    if !goto_stmt.starts_with("goto L_") || !goto_stmt.ends_with(';') {
        return Err(format!("post_lift: unexpected swap_confirm branch at L_{}: {}", lab, old));
    }
    Ok(format!("{}{}   /* [swap_confirm] */", patched_pfx, goto_stmt))
}

const UW_WIDTH_LAB: &str = "7100dc1e40";
const UW_BUFFERS_LABS: &[&str] = &["710097b9fc", "710097ba0c", "710097cba0", "710097d29c", "7100ed19d8"];
const UW_HALF_LABS: &[&str] = &["710097cbe8", "710097d2d0", "710097d2fc", "710097d328"];
const UW_QUARTER_LABS: &[&str] = &[
    "710097d354", "710097d380", "710097d3a8", "710097d3d0", "710097d3f8",
    "710097d420", "710097d448", "710097d498", "710097d4c0",
];
const UW_CENTRE_LAB: &str = "710097985c";

fn patch_uw_line(lab: &str, old: &str) -> Result<String, String> {
    if lab == UW_WIDTH_LAB {
        if !old.contains("c->x[20]") || !old.contains("rd32(a)") {
            return Err(format!("post_lift: unexpected ultrawide width line at {}: {}", lab, old));
        }
        Ok(format!(
            "L_{}: {{  uint64_t a = c->x[20] + 0ULL; c->x[8] = (uint64_t)(rd32(a));  }} if (recomps_uw_width()) c->x[8] = recomps_uw_width();   /* [uw_patch] */",
            lab
        ))
    } else if lab == UW_CENTRE_LAB {
        if !old.contains("c->v[3] = t") {
            return Err(format!("post_lift: unexpected ultrawide centre line at {}: {}", lab, old));
        }
        Ok(format!(
            "L_{}: {{ V128 t = c->v[0];  c->v[3] = t; }} if (recomps_uw_width()) {{ float off = (c->v[1].f[0] - 1280.0f) * 0.5f; if (off > 0) {{ c->v[0].f[0] = -off; c->v[1].f[0] -= off; }} }}   /* [uw_patch] */",
            lab
        ))
    } else {
        let b = body_of(old).ok_or_else(|| format!("post_lift: bad uw line: {}", old))?;
        let r = b.strip_prefix("c->x[").ok_or_else(|| format!("post_lift: unexpected uw line at {}: {}", lab, old))?;
        let (reg, _) = parse_u32_prefix(r).ok_or_else(|| format!("post_lift: bad uw reg at {}", lab))?;
        if UW_HALF_LABS.contains(&lab) {
            Ok(format!("L_{}: c->x[{}] = (uint32_t)(recomps_uw_width() ? recomps_uw_width() / 2 : 0x280U);   /* [uw_patch] */", lab, reg))
        } else if UW_QUARTER_LABS.contains(&lab) {
            Ok(format!("L_{}: c->x[{}] = (uint32_t)(recomps_uw_width() ? recomps_uw_width() / 4 : 0x140U);   /* [uw_patch] */", lab, reg))
        } else if UW_BUFFERS_LABS.contains(&lab) {
            Ok(format!("L_{}: c->x[{}] = (uint32_t)(recomps_uw_width() ? recomps_uw_width() : 0x500U);   /* [uw_patch] */", lab, reg))
        } else {
            Err(format!("post_lift: unknown ultrawide label {}", lab))
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn uw_patch_all_20_sites_and_idempotent() {
        let width_in = "L_7100dc1e40: {  uint64_t a = c->x[20] + 0ULL; c->x[8] = (uint64_t)(rd32(a));  }";
        let width_out = patch_uw_line(UW_WIDTH_LAB, width_in).unwrap();
        assert!(width_out.contains("if (recomps_uw_width()) c->x[8] = recomps_uw_width();"));
        assert_eq!(patch_uw_line(UW_WIDTH_LAB, &width_out).unwrap(), width_out);

        let centre_in = "L_710097985c: { V128 t = c->v[0];  c->v[3] = t; }";
        let centre_out = patch_uw_line(UW_CENTRE_LAB, centre_in).unwrap();
        assert!(centre_out.contains("float off = (c->v[1].f[0] - 1280.0f) * 0.5f;"));
        assert_eq!(patch_uw_line(UW_CENTRE_LAB, &centre_out).unwrap(), centre_out);

        for &lab in UW_BUFFERS_LABS {
            let line = format!("L_{}: c->x[9] = (uint32_t)(0x500ULL);", lab);
            let out = patch_uw_line(lab, &line).unwrap();
            assert!(out.contains("recomps_uw_width() ? recomps_uw_width() : 0x500U"));
            assert_eq!(patch_uw_line(lab, &out).unwrap(), out);
        }
        for &lab in UW_HALF_LABS {
            let line = format!("L_{}: c->x[10] = (uint32_t)(0x280ULL);", lab);
            let out = patch_uw_line(lab, &line).unwrap();
            assert!(out.contains("recomps_uw_width() ? recomps_uw_width() / 2 : 0x280U"));
            assert_eq!(patch_uw_line(lab, &out).unwrap(), out);
        }
        for &lab in UW_QUARTER_LABS {
            let line = format!("L_{}: c->x[11] = (uint32_t)(0x140ULL);", lab);
            let out = patch_uw_line(lab, &line).unwrap();
            assert!(out.contains("recomps_uw_width() ? recomps_uw_width() / 4 : 0x140U"));
            assert_eq!(patch_uw_line(lab, &out).unwrap(), out);
        }
        assert_eq!(
            1 + UW_BUFFERS_LABS.len() + UW_HALF_LABS.len() + UW_QUARTER_LABS.len() + 1,
            20
        );
    }

    #[test]
    fn uw_patch_rejects_unexpected_lines() {
        assert!(patch_uw_line(UW_WIDTH_LAB, "L_7100dc1e40: c->x[8] = 0;").is_err());
        assert!(patch_uw_line(UW_CENTRE_LAB, "L_710097985c: c->v[0] = c->v[1];").is_err());
        assert!(patch_uw_line("710097b9fc", "L_710097b9fc: ;").is_err());
    }

    #[test]
    fn swap_confirm_patch_all_5_sites_and_idempotent() {
        let cases = [
            (
                "71007c65a0",
                5,
                "!=",
                0x71018e4814u64,
                "L_71007c65a0: if (((c->x[8] >> 5) & 1) != 0) goto L_71007c65d4;",
                "L_71007c65a0: if (((uint32_t)c->x[8] & rd32(0x71018e4814ULL)) != 0) goto L_71007c65d4;   /* [swap_confirm] */",
            ),
            (
                "71007c65f8",
                4,
                "==",
                0x71018e4810u64,
                "L_71007c65f8: if (((c->x[8] >> 4) & 1) == 0) goto L_71007c6678;",
                "L_71007c65f8: if (((uint32_t)c->x[8] & rd32(0x71018e4810ULL)) == 0) goto L_71007c6678;   /* [swap_confirm] */",
            ),
            (
                "7100b7a46c",
                5,
                "!=",
                0x71018e4814u64,
                "L_7100b7a46c: if (((c->x[8] >> 5) & 1) != 0) goto L_7100b7a4ac;",
                "L_7100b7a46c: if (((uint32_t)c->x[8] & rd32(0x71018e4814ULL)) != 0) goto L_7100b7a4ac;   /* [swap_confirm] */",
            ),
            (
                "7100b7a4d0",
                4,
                "==",
                0x71018e4810u64,
                "L_7100b7a4d0: if (((c->x[8] >> 4) & 1) == 0) goto L_7100b7a5ac;",
                "L_7100b7a4d0: if (((uint32_t)c->x[8] & rd32(0x71018e4810ULL)) == 0) goto L_7100b7a5ac;   /* [swap_confirm] */",
            ),
            (
                "7100b7a768",
                4,
                "==",
                0x71018e4810u64,
                "L_7100b7a768: if (((c->x[8] >> 4) & 1) == 0) goto L_7100b7aa04;",
                "L_7100b7a768: if (((uint32_t)c->x[8] & rd32(0x71018e4810ULL)) == 0) goto L_7100b7aa04;   /* [swap_confirm] */",
            ),
        ];
        assert_eq!(cases.len(), SWAP_CONFIRM_SITES.len());
        for (idx, (lab, bit, cmp_op, mask_addr, input, expected)) in cases.iter().enumerate() {
            let (_, s_lab, s_bit, s_cmp, s_mask) = SWAP_CONFIRM_SITES[idx];
            assert_eq!((s_lab, s_bit, s_cmp, s_mask), (*lab, *bit, *cmp_op, *mask_addr));
            let out = patch_swap_confirm_line(lab, *bit, cmp_op, *mask_addr, input).unwrap();
            assert_eq!(out, *expected);
            assert_eq!(patch_swap_confirm_line(lab, *bit, cmp_op, *mask_addr, &out).unwrap(), out);
        }
    }

    #[test]
    fn swap_confirm_patch_rejects_unexpected_lines() {
        assert!(patch_swap_confirm_line(
            "71007c65a0",
            5,
            "!=",
            0x71018e4814,
            "L_71007c65a0: if (((c->x[8] >> 4) & 1) != 0) goto L_71007c65d4;"
        )
        .is_err());
        assert!(patch_swap_confirm_line(
            "71007c65f8",
            4,
            "==",
            0x71018e4810,
            "L_71007c65f8: if (((c->x[8] >> 4) & 1) != 0) goto L_71007c6678;"
        )
        .is_err());
        assert!(patch_swap_confirm_line(
            "7100b7a768",
            4,
            "==",
            0x71018e4810,
            "L_7100b7a768: if (((c->x[8] >> 4) & 1) == 0) return;"
        )
        .is_err());
    }

    #[test]
    fn fs_patch_store_and_op_modes() {
        // Store-labeled -1/60 timer (ComboRow f_7100c8fa98 pattern)
        let mut lines: Vec<String> = vec![
            "void f_7100c8fa98(Ctx *c) {",
            "L_7100c8fb64: {  uint64_t a = c->x[19] + 400ULL; c->v[0] = rdv(a, 4);  }",
            "L_7100c8fb78: c->x[8] = (uint64_t)(0x7101462000ULL);",
            "L_7100c8fb7c: {  uint64_t a = c->x[8] + 1956ULL; c->v[1] = rdv(a, 4);  }",
            "L_7100c8fb80: { float t = fnan32(c->v[0].f[0] + c->v[1].f[0]); vzero(&c->v[0]); c->v[0].f[0] = t; }",
            "L_7100c8fb84: fcmp(c, (double)c->v[0].f[0], (double)0);",
            "L_7100c8fb88: {  uint64_t a = c->x[19] + 400ULL; wrv(a, &c->v[0], 4);  }",
            "}",
        ].into_iter().map(String::from).collect();
        patch_fs(&mut lines, 0, 7, 6, 0x7100c8fb88, false, None, None).unwrap();
        assert!(lines[4].contains("(c->v[1].f[0] * recomps_fps_fs(c, 0x7100c8fb88ULL, c->x[19] + 400ULL))"));

        // Op-labeled wrap animation with csel before store (Chapter Select Card bobbing f_7100601964 pattern)
        let mut card: Vec<String> = vec![
            "void f_7100601964(Ctx *c) {",
            "L_710060198c: {  uint64_t a = c->x[19] + 1456ULL; c->v[0] = rdv(a, 4);  }",
            "L_7100601990: { float t = fnan32((float)1.0); vzero(&c->v[1]); c->v[1].f[0] = t; }",
            "L_7100601994: c->x[8] = (uint64_t)(0x7101462000ULL);",
            "L_7100601998: { float t = fnan32(c->v[0].f[0] + c->v[1].f[0]); vzero(&c->v[0]); c->v[0].f[0] = t; }",
            "L_710060199c: {  uint64_t a = c->x[8] + 2376ULL; c->v[1] = rdv(a, 4);  }",
            "L_71006019a0: c->x[8] = (uint64_t)(0x7101462000ULL);",
            "L_71006019a4: {  uint64_t a = c->x[8] + 1184ULL; c->v[2] = rdv(a, 4);  }",
            "L_71006019a8: c->x[8] = (uint64_t)(0x7101462000ULL);",
            "L_71006019ac: { float t = fnan32(c->v[0].f[0] + c->v[1].f[0]); vzero(&c->v[1]); c->v[1].f[0] = t; }",
            "L_71006019b0: fcmp(c, (double)c->v[0].f[0], (double)c->v[2].f[0]);",
            "L_71006019b4: { float t = fnan32(cond(c, 12) ? c->v[1].f[0] : c->v[0].f[0]); vzero(&c->v[0]); c->v[0].f[0] = t; }",
            "L_71006019b8: {  uint64_t a = c->x[8] + 1320ULL; c->v[1] = rdv(a, 4);  }",
            "L_71006019bc: c->x[8] = (uint64_t)(0x7101462000ULL);",
            "L_71006019c0: {  uint64_t a = c->x[19] + 1456ULL; wrv(a, &c->v[0], 4);  }",
            "}",
        ].into_iter().map(String::from).collect();
        patch_fs(&mut card, 0, 15, 4, 0x7100601998, true, None, None).unwrap();
        assert!(card[4].contains("(c->v[1].f[0] * recomps_fps_fs(c, 0x7100601998ULL, c->x[19] + 1456ULL))"));

        // Explicit step_reg + field_reg (Chapter Select Card rotation f_7100601da4 pattern)
        let mut rot: Vec<String> = vec![
            "void f_7100601da4(Ctx *c) {",
            "L_7100601ed0: { float t = fnan32(c->v[0].f[0] + c->v[1].f[0]); vzero(&c->v[9]); c->v[9].f[0] = t; }",
            "L_7100601edc: c->x[30] = 0x7100601ee0ULL; f_7100963a90(c);",
            "L_7100601ee0: {  uint64_t a = c->x[0] + 132ULL; wrv(a, &c->v[9], 4);  }",
            "}",
        ].into_iter().map(String::from).collect();
        patch_fs(&mut rot, 0, 4, 1, 0x7100601ed0, true, Some(1), Some(19)).unwrap();
        assert!(rot[1].contains("(c->v[1].f[0] * recomps_fps_fs(c, 0x7100601ed0ULL, c->x[19]))"));
    }

    #[test]
    fn ach_live_patch_all_4_sites_and_idempotent() {
        for &(_, lab, store) in ACH_LIVE_SITES {
            let input = format!("L_{}: {}", lab, store);
            let out = patch_ach_live_line(lab, store, &input).unwrap();
            assert!(out.contains("recomps_ach_live_eval(c);"));
            assert_eq!(patch_ach_live_line(lab, store, &out).unwrap(), out);
        }
        assert!(patch_ach_live_line("7100d9440c", "{ wrong }", "L_7100d9440c: return;").is_err());
    }

    #[test]
    fn movie_timeout_hook_inserts_before_label() {
        let mut lines: Vec<String> = vec![
            "void f_7100b75dc0(Ctx *c) {",
            "L_7100b76284: { float t = fnan32((float)-1.0); vzero(&c->v[1]); c->v[1].f[0] = t; }",
            "L_7100b76288: { float t = fnan32(c->v[0].f[0] + c->v[1].f[0]); vzero(&c->v[0]); c->v[0].f[0] = t; }",
            "}",
        ]
        .into_iter()
        .map(String::from)
        .collect();
        replace_once(
            &mut lines,
            "L_7100b76288: {",
            "if (recomps_movie_fix_on()) recomps_movie_timeout_step(c);\nL_7100b76288: {",
        )
        .unwrap();
        assert!(lines[2].starts_with("if (recomps_movie_fix_on()) recomps_movie_timeout_step(c);\nL_7100b76288: {"));
    }
}
