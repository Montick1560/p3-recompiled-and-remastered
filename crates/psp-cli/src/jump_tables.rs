//! Pattern-based jump-table discovery.
//!
//! The decoder turns `jr rX` into a `JumpTable` (a C++ switch over the case
//! labels) when DATA xrefs point from a table into the function body. Ghidra
//! supplies those xrefs for most switches, but misses some — e.g. the tables
//! in Patapon 3's code overlays, whose cases then become RECOMP_LOOKUP misses
//! (noops). This pass recognises the standard MIPS switch idiom
//!
//! ```text
//!   sltiu  at, rI, N          ; bound (optional)
//!   lui    rB, hi             ; table base ...
//!   sll    rT, rI, 2
//!   addiu  rB, rB, lo         ; ... = (hi << 16) + sext(lo)
//!   addu   rX, rT, rB
//!   lw     rX, 0(rX)
//!   jr     rX
//! ```
//!
//! The base is taken from the latest writes of the `addu` operand (`lui` then
//! `addiu`), not from any pair in the window.
//!
//! reads the table from the loaded image and returns synthetic
//! `(table_entry_addr, case_target)` DATA xrefs for cases inside the function.

/// How far back from the `jr` to look for the idiom's instructions.
const LOOKBACK: u32 = 16;
/// Upper bound on cases read when no `sltiu` bound is found.
const MAX_CASES: u32 = 256;

fn read_word(segment_bytes: &[(u32, Vec<u8>)], va: u32) -> Option<u32> {
    segment_bytes.iter().find_map(|(base, bytes)| {
        let off = va.checked_sub(*base)? as usize;
        let b = bytes.get(off..off + 4)?;
        Some(u32::from_le_bytes([b[0], b[1], b[2], b[3]]))
    })
}

fn rs(w: u32) -> u32 { (w >> 21) & 31 }
fn rt(w: u32) -> u32 { (w >> 16) & 31 }
fn rd(w: u32) -> u32 { (w >> 11) & 31 }
fn op(w: u32) -> u32 { w >> 26 }
fn simm(w: u32) -> i32 { (w & 0xFFFF) as u16 as i16 as i32 }

/// True when `w` writes general register `reg` (the shapes a switch prologue holds).
fn writes(w: u32, reg: u32) -> bool {
    if reg == 0 {
        return false;
    }
    match op(w) {
        0x00 | 0x1C => rd(w) == reg,               // SPECIAL / SPECIAL2 (Allegrex)
        0x1F => rt(w) == reg,                      // SPECIAL3 (ext, ins, seb, seh)
        0x08..=0x0F | 0x20..=0x27 => rt(w) == reg, // ALU immediates, lui, loads
        0x03 => reg == 31,                         // jal
        _ => false,
    }
}

/// The value `reg` holds just before `words[idx]` when it was built by `lui reg, hi` and then
/// `addiu reg, reg, lo`, each the latest write to `reg`; None for any other shape.
fn lui_addiu_value(words: &[(u32, u32)], idx: usize, reg: u32) -> Option<u32> {
    let last_write = |end: usize| words[..end].iter().rposition(|(_, w)| writes(*w, reg));
    let ai = last_write(idx)?;
    let a = words[ai].1;
    if op(a) != 0x09 || rs(a) != reg {
        return None;
    }
    let l = words[last_write(ai)?].1;
    if op(l) != 0x0F {
        return None;
    }
    Some(((l & 0xFFFF) << 16).wrapping_add(simm(a) as u32))
}

/// Synthetic DATA xrefs for every recognised jump table in `[start, end)`.
pub fn discover_jump_table_xrefs(
    segment_bytes: &[(u32, Vec<u8>)],
    start: u32,
    end: u32,
) -> Vec<(u32, u32)> {
    let mut out = Vec::new();
    let mut va = start;
    while va + 4 <= end {
        let Some(w) = read_word(segment_bytes, va) else { break };
        // jr rX (SPECIAL funct 0x08), rX != ra
        if op(w) == 0 && (w & 0x3F) == 0x08 && rs(w) != 31 {
            if let Some(xrefs) = table_at_jr(segment_bytes, va, rs(w), start, end) {
                out.extend(xrefs);
            }
        }
        va += 4;
    }
    out
}

fn table_at_jr(
    segment_bytes: &[(u32, Vec<u8>)],
    jr_va: u32,
    target_reg: u32,
    start: u32,
    end: u32,
) -> Option<Vec<(u32, u32)>> {
    let lo_bound = jr_va.saturating_sub(4 * LOOKBACK).max(start);
    let words: Vec<(u32, u32)> = (lo_bound..jr_va)
        .step_by(4)
        .filter_map(|a| Some((a, read_word(segment_bytes, a)?)))
        .collect();

    // lw rX, 0(rP)
    let (lw_idx, lw) = words
        .iter()
        .enumerate()
        .rev()
        .find(|(_, (_, w))| op(*w) == 0x23 && rt(*w) == target_reg)?;
    let ptr_reg = rs(lw.1);
    let lw_off = simm(lw.1);

    // addu rP, rA, rB  (one operand is the table base)
    let (addu_idx, addu) = words[..lw_idx]
        .iter()
        .enumerate()
        .rev()
        .find(|(_, (_, w))| op(*w) == 0 && (*w & 0x3F) == 0x21 && rd(*w) == ptr_reg)?;
    let operands = [rs(addu.1), rt(addu.1)];

    // lui rB, hi; addiu rB, rB, lo: the base is the value the operand holds at the addu (a
    // stray lui/addiu of the other operand in the window is not it, FUN_08ae4ba0).
    let base = operands.iter().find_map(|&reg| lui_addiu_value(&words, addu_idx, reg));
    let table = base?.wrapping_add(lw_off as u32);

    // sltiu at, rI, N  (bound), searched a little further back
    let bound = {
        let far = jr_va.saturating_sub(4 * 2 * LOOKBACK).max(start);
        (far..jr_va)
            .step_by(4)
            .filter_map(|a| read_word(segment_bytes, a))
            .filter(|w| op(*w) == 0x0B)
            .last()
            .map(|w| w & 0xFFFF)
            .filter(|&n| n > 0 && n <= MAX_CASES)
    };

    let mut xrefs = Vec::new();
    for i in 0..bound.unwrap_or(MAX_CASES) {
        let entry = table.wrapping_add(4 * i);
        let Some(target) = read_word(segment_bytes, entry) else { break };
        if target > start && target < end {
            xrefs.push((entry, target));
        } else if bound.is_none() {
            break; // unbounded scan stops at the first non-case word
        }
    }
    (xrefs.len() >= 2).then_some(xrefs)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn seg(base: u32, words: &[u32]) -> Vec<(u32, Vec<u8>)> {
        vec![(base, words.iter().flat_map(|w| w.to_le_bytes()).collect())]
    }

    /// The switch prologue of Patapon 3 OL_Azito FUN_08abce50, rebased so
    /// the table sits right after the function body.
    #[test]
    fn finds_the_azito_switch_table() {
        let f = 0x0880_0000u32;
        let mut words = vec![
            0x2CA1_0003, // sltiu at, a1, 3
            0x1020_0010, // beq at, zero, default
            0x0000_0000,
            0x3C04_0880, // lui a0, 0x0880
            0x0005_1880, // sll v1, a1, 2
            0x2484_0040, // addiu a0, a0, 0x40   -> table 0x08800040
            0x0064_1821, // addu v1, v1, a0
            0x8C63_0000, // lw v1, 0(v1)
            0x0060_0008, // jr v1
            0x0000_0000,
            0x0000_0000, // case A @ 0x28
            0x0000_0000, // case B @ 0x2C
            0x0000_0000, // case C @ 0x30
            0x03E0_0008, // jr ra
            0x0000_0000,
            0x0000_0000,
        ];
        words.extend([f + 0x28, f + 0x2C, f + 0x30, 0xDEAD_BEEF]); // table @0x40
        let s = seg(f, &words);
        let x = discover_jump_table_xrefs(&s, f, f + 0x40);
        assert_eq!(x, vec![(f + 0x40, f + 0x28), (f + 0x44, f + 0x2C), (f + 0x48, f + 0x30)]);
    }

    /// The switch of Patapon 3 DxD OL_Title FUN_08ae4ba0 (0x08AE4EA4..), rebased: an unrelated
    /// `lui v1` and the index's own `addiu v1, v1, -5` sit in the window, the base is a0's.
    /// Taking v1's pair gave base 0x001FFFFB and no table (the Gigante King freeze, 2026-10-17).
    #[test]
    fn base_comes_from_the_latest_writes_not_any_lui_addiu_pair() {
        let f = 0x0880_0000u32;
        let mut words = vec![
            0x8E84_0338, // 0x00 lw a0, 824(s4)
            0x3C03_0020, // 0x04 lui v1, 0x0020
            0x0083_1824, // 0x08 and v1, a0, v1
            0x5060_0118, // 0x0C beqzl v1, ...
            0x8FBF_001C, // 0x10 lw ra, 28(sp)
            0x9643_0078, // 0x14 lhu v1, 120(s2)
            0x2463_FFFB, // 0x18 addiu v1, v1, -5
            0x2C61_000C, // 0x1C sltiu at, v1, 12
            0x1020_0112, // 0x20 beq at, zero, default
            0x0000_0000, // 0x24
            0x3C04_0880, // 0x28 lui a0, 0x0880
            0x0003_1880, // 0x2C sll v1, v1, 2
            0x2484_0060, // 0x30 addiu a0, a0, 0x60   -> table 0x08800060
            0x0064_1821, // 0x34 addu v1, v1, a0
            0x8C63_0000, // 0x38 lw v1, 0(v1)
            0x0060_0008, // 0x3C jr v1
            0x0000_0000, // 0x40
            0x0000_0000, // 0x44 case A
            0x0000_0000, // 0x48 case B
            0x03E0_0008, // 0x4C jr ra (default)
            0x0000_0000, // 0x50
            0x0000_0000, // 0x54
            0x0000_0000, // 0x58
            0x0000_0000, // 0x5C
        ];
        let (a, b, d) = (f + 0x44, f + 0x48, f + 0x4C);
        let table = [a, d, b, a, d, b, a, d, b, a, d, b];
        words.extend(table);
        let s = seg(f, &words);
        let x = discover_jump_table_xrefs(&s, f, f + 0x60);
        let want: Vec<(u32, u32)> =
            table.iter().enumerate().map(|(i, &t)| (f + 0x60 + 4 * i as u32, t)).collect();
        assert_eq!(x, want);
    }

    #[test]
    fn ignores_jr_ra_and_unknown_shapes() {
        let f = 0x0880_0000u32;
        let s = seg(f, &[0x03E0_0008, 0x0000_0000, 0x0060_0008, 0x0000_0000]);
        assert!(discover_jump_table_xrefs(&s, f, f + 0x10).is_empty());
    }
}
