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

    // lui rB, hi ... addiu rB, rB, lo  for one of the operands
    let mut base = None;
    for &reg in &operands {
        let lui = words[..addu_idx]
            .iter()
            .rev()
            .find(|(_, w)| op(*w) == 0x0F && rt(*w) == reg);
        let addiu = words[..addu_idx]
            .iter()
            .rev()
            .find(|(_, w)| op(*w) == 0x09 && rt(*w) == reg && rs(*w) == reg);
        if let (Some((_, l)), Some((_, a))) = (lui, addiu) {
            base = Some(((l & 0xFFFF) << 16).wrapping_add(simm(*a) as u32));
            break;
        }
    }
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

    #[test]
    fn ignores_jr_ra_and_unknown_shapes() {
        let f = 0x0880_0000u32;
        let s = seg(f, &[0x03E0_0008, 0x0000_0000, 0x0060_0008, 0x0000_0000]);
        assert!(discover_jump_table_xrefs(&s, f, f + 0x10).is_empty());
    }
}
