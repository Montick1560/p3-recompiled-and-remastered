//! PSP Allegrex MIPS32 instruction decoder.
//!
//! Translates raw `u32` instruction words into typed `MipsOp` IR values.
//! Handles all MIPS32 base instructions, Allegrex extensions, FPU (COP1), and
//! VFPU (COP2, stubbed for Phase 6). Correctly handles standard and branch-likely
//! delay slot semantics. Detects jump tables from DATA xrefs.

use byteorder::{LittleEndian, ReadBytesExt};
use psp_ir::{MipsOp, Reg};
use thiserror::Error;

mod allegrex;
mod delay_slot;
mod fpu;
mod mips32;
mod vfpu;
#[cfg(test)]
mod tests;

pub use delay_slot::{
    branch_gpr_reads, gpr_write_of, has_delay_slot_hazard, is_branch_likely, is_branch_or_jump,
};

/// Errors that can occur during instruction decoding.
#[derive(Debug, Error)]
pub enum DecodeError {
    /// Unknown or unimplemented instruction encoding.
    #[error("unknown instruction word 0x{0:08X} at vaddr 0x{1:08X}")]
    Unknown(u32, u32),
    /// Byte slice length is not a multiple of 4 (cannot read complete words).
    #[error("byte slice out of bounds or not word-aligned")]
    OutOfBounds,
}

// -------------------------------------------------------------------------
// Register field helpers
// -------------------------------------------------------------------------

/// Extract the `rs` (source 1) register field from an instruction word.
#[inline]
pub(crate) fn rs(word: u32) -> Reg {
    Reg::from_u8(((word >> 21) & 0x1F) as u8)
}

/// Extract the `rt` (source 2 / destination for I-type) register field.
#[inline]
pub(crate) fn rt(word: u32) -> Reg {
    Reg::from_u8(((word >> 16) & 0x1F) as u8)
}

/// Extract the `rd` (destination for R-type) register field.
#[inline]
pub(crate) fn rd(word: u32) -> Reg {
    Reg::from_u8(((word >> 11) & 0x1F) as u8)
}

// -------------------------------------------------------------------------
// Per-word decoder
// -------------------------------------------------------------------------

/// Compute the absolute branch target virtual address.
///
/// MIPS branch target = (branch_pc + 4) + sign_extend(offset) * 4
#[inline]
pub(crate) fn branch_target(vaddr: u32, offset: i16) -> u32 {
    vaddr.wrapping_add(4).wrapping_add((offset as i32).wrapping_mul(4) as u32)
}

/// Decode a single instruction word into a `MipsOp`.
///
/// Does not handle delay slot semantics — use `decode_function` for a
/// complete function with correct delay slot ordering.
pub fn decode_word(word: u32, vaddr: u32) -> Result<MipsOp, DecodeError> {
    let opcode = (word >> 26) & 0x3F;
    let rs_reg = rs(word);
    let rt_reg = rt(word);
    let imm16 = (word & 0xFFFF) as i16;
    let imm16u = (word & 0xFFFF) as u16;
    let offset = imm16;
    let btarget = branch_target(vaddr, offset);

    match opcode {
        0x00 => mips32::decode_special(word, vaddr),
        0x01 => mips32::decode_regimm(word, vaddr),
        0x02 => {
            let target = (vaddr & 0xF000_0000) | ((word & 0x03FF_FFFF) << 2);
            Ok(MipsOp::J { target })
        }
        0x03 => {
            let target = (vaddr & 0xF000_0000) | ((word & 0x03FF_FFFF) << 2);
            Ok(MipsOp::Jal { target })
        }
        0x04 => Ok(MipsOp::Beq { rs: rs_reg, rt: rt_reg, target: btarget, likely: false }),
        0x05 => Ok(MipsOp::Bne { rs: rs_reg, rt: rt_reg, target: btarget, likely: false }),
        0x06 => Ok(MipsOp::Blez { rs: rs_reg, target: btarget, likely: false }),
        0x07 => Ok(MipsOp::Bgtz { rs: rs_reg, target: btarget, likely: false }),
        0x08 => Ok(MipsOp::Addi { rt: rt_reg, rs: rs_reg, imm: imm16 }),
        0x09 => Ok(MipsOp::Addiu { rt: rt_reg, rs: rs_reg, imm: imm16 }),
        0x0A => Ok(MipsOp::Slti { rt: rt_reg, rs: rs_reg, imm: imm16 }),
        0x0B => Ok(MipsOp::Sltiu { rt: rt_reg, rs: rs_reg, imm: imm16u }),
        0x0C => Ok(MipsOp::Andi { rt: rt_reg, rs: rs_reg, imm: imm16u }),
        0x0D => Ok(MipsOp::Ori { rt: rt_reg, rs: rs_reg, imm: imm16u }),
        0x0E => Ok(MipsOp::Xori { rt: rt_reg, rs: rs_reg, imm: imm16u }),
        0x0F => Ok(MipsOp::Lui { rt: rt_reg, imm: imm16u }),
        0x10 => {
            // COP0: rs field acts as sub-opcode
            let rs_field = (word >> 21) & 0x1F;
            let rd_num = ((word >> 11) & 0x1F) as u8;
            match rs_field {
                0x00 => Ok(MipsOp::Mfc0 { rt: rt_reg, rd: rd_num }),
                0x04 => Ok(MipsOp::Mtc0 { rt: rt_reg, rd: rd_num }),
                _ => Err(DecodeError::Unknown(word, vaddr)),
            }
        }
        0x11 => fpu::decode_cop1(word, vaddr),
        0x12 => vfpu::decode_cop2(word, vaddr),
        // VFPU arithmetic (opcode 0x18-0x19, 0x1B)
        0x18 => vfpu::decode_vfpu0(word, vaddr),
        0x19 => vfpu::decode_vfpu1(word, vaddr),
        0x14 => Ok(MipsOp::Beq { rs: rs_reg, rt: rt_reg, target: btarget, likely: true }),
        0x15 => Ok(MipsOp::Bne { rs: rs_reg, rt: rt_reg, target: btarget, likely: true }),
        0x16 => Ok(MipsOp::Blez { rs: rs_reg, target: btarget, likely: true }),
        0x17 => Ok(MipsOp::Bgtz { rs: rs_reg, target: btarget, likely: true }),
        0x1B => vfpu::decode_vfpu3(word, vaddr),
        0x1C => allegrex::decode_allegrex_special2(word, vaddr),
        0x1F => allegrex::decode_special3(word, vaddr),
        0x20 => Ok(MipsOp::Lb { rt: rt_reg, rs: rs_reg, offset }),
        0x21 => Ok(MipsOp::Lh { rt: rt_reg, rs: rs_reg, offset }),
        0x22 => Ok(MipsOp::Lwl { rt: rt_reg, rs: rs_reg, offset }),
        0x23 => Ok(MipsOp::Lw { rt: rt_reg, rs: rs_reg, offset }),
        0x24 => Ok(MipsOp::Lbu { rt: rt_reg, rs: rs_reg, offset }),
        0x25 => Ok(MipsOp::Lhu { rt: rt_reg, rs: rs_reg, offset }),
        0x26 => Ok(MipsOp::Lwr { rt: rt_reg, rs: rs_reg, offset }),
        0x28 => Ok(MipsOp::Sb { rt: rt_reg, rs: rs_reg, offset }),
        0x29 => Ok(MipsOp::Sh { rt: rt_reg, rs: rs_reg, offset }),
        0x2A => Ok(MipsOp::Swl { rt: rt_reg, rs: rs_reg, offset }),
        0x2B => Ok(MipsOp::Sw { rt: rt_reg, rs: rs_reg, offset }),
        0x2E => Ok(MipsOp::Swr { rt: rt_reg, rs: rs_reg, offset }),
        0x2F => {
            let op = ((word >> 16) & 0x1F) as u8;
            Ok(MipsOp::Cache { op, rs: rs_reg, offset })
        }
        0x31 => {
            let ft = psp_ir::FpReg(((word >> 16) & 0x1F) as u8);
            Ok(MipsOp::Lwc1 { ft, rs: rs_reg, offset })
        }
        // VFPU memory and decode groups
        0x32 => vfpu::decode_lv_s(word, vaddr),
        0x34 => vfpu::decode_vfpu4(word, vaddr),
        0x35 => vfpu::decode_lvlr_q(word, vaddr),
        0x36 => vfpu::decode_lv_q(word, vaddr),
        0x37 => vfpu::decode_vfpu5(word, vaddr),
        0x39 => {
            let ft = psp_ir::FpReg(((word >> 16) & 0x1F) as u8);
            Ok(MipsOp::Swc1 { ft, rs: rs_reg, offset })
        }
        0x3A => vfpu::decode_sv_s(word, vaddr),
        0x3C => vfpu::decode_vfpu6(word, vaddr),
        0x3D => vfpu::decode_svlr_q(word, vaddr),
        0x3E => vfpu::decode_sv_q(word, vaddr),
        0x3F => Ok(MipsOp::VfpuFlush {}),
        _ => Err(DecodeError::Unknown(word, vaddr)),
    }
}

// -------------------------------------------------------------------------
// Function-level decoder with delay slot handling
// -------------------------------------------------------------------------

/// Decode a complete function from raw bytes into a sequence of `MipsOp` values.
///
/// Reads instruction words in little-endian order starting at `base_vaddr`.
/// Applies correct delay slot semantics:
/// - Standard branches: delay slot instruction emitted BEFORE the branch.
/// - Branch-likely: delay slot wrapped in `MipsOp::DelaySlot{}` AFTER the branch.
///
/// Jump table detection: for `Jr{rs}` where `rs != $ra`, scans `xrefs` for DATA
/// references into the function range. If 2+ targets found, emits `JumpTable`.
///
/// # Errors
///
/// Returns `DecodeError::OutOfBounds` if `bytes.len()` is not a multiple of 4.
/// Returns `DecodeError::Unknown` for any unrecognized instruction encoding.
pub fn decode_function(
    bytes: &[u8],
    base_vaddr: u32,
    xrefs: &[(u32, u32)],
) -> Result<Vec<MipsOp>, DecodeError> {
    if bytes.len() % 4 != 0 {
        return Err(DecodeError::OutOfBounds);
    }

    let word_count = bytes.len() / 4;
    let mut cursor = std::io::Cursor::new(bytes);
    let mut raw_ops: Vec<(u32, MipsOp)> = Vec::with_capacity(word_count);

    // First pass: decode all words without delay slot handling
    for i in 0..word_count {
        let vaddr = base_vaddr + (i as u32) * 4;
        let word = cursor.read_u32::<LittleEndian>().map_err(|_| DecodeError::OutOfBounds)?;
        let op = decode_word(word, vaddr)?;
        raw_ops.push((vaddr, op));
    }

    // Apply jump table detection: replace Jr{rs != $ra} with JumpTable if xrefs found
    let func_end = base_vaddr + (word_count as u32) * 4;
    let raw_ops: Vec<(u32, MipsOp)> = raw_ops
        .into_iter()
        .map(|(vaddr, op)| {
            let op = maybe_upgrade_jr_to_jump_table(op, vaddr, base_vaddr, func_end, xrefs);
            (vaddr, op)
        })
        .collect();

    // Second pass: apply delay slot reordering
    let mut result: Vec<MipsOp> = Vec::with_capacity(word_count);
    let mut skip_next = false;

    for i in 0..raw_ops.len() {
        if skip_next {
            skip_next = false;
            continue;
        }

        let (_, ref op) = raw_ops[i];

        if is_branch_or_jump(op) {
            if i + 1 < raw_ops.len() {
                let (_, ref ds_op) = raw_ops[i + 1];
                skip_next = true;

                if is_branch_likely(op) {
                    // Branch-likely: delay slot wrapped in DelaySlot{}, placed AFTER branch
                    result.push(op.clone());
                    result.push(MipsOp::DelaySlot { instr: Box::new(ds_op.clone()) });
                } else if has_delay_slot_hazard(op, ds_op) {
                    // Hazard: the delay slot WRITES a register the branch READS.
                    // Real MIPS evaluates the condition / captures the jump target
                    // BEFORE the delay slot executes, so the plain swap below would
                    // test the clobbered value (issue #12). Fuse the pair so the
                    // emitter can snapshot the pre-delay reads; the trailing Nop
                    // preserves the positional word-to-IR mapping that label and
                    // mid-entry emission rely on.
                    result.push(MipsOp::BranchHazardDelay {
                        branch: Box::new(op.clone()),
                        delay: Box::new(ds_op.clone()),
                    });
                    result.push(MipsOp::Nop {});
                } else {
                    // Standard branch: delay slot placed BEFORE branch (unconditionally executes)
                    result.push(ds_op.clone());
                    result.push(op.clone());
                }
            } else {
                // Branch at end of function with no delay slot (shouldn't happen in valid MIPS)
                result.push(op.clone());
            }
        } else {
            result.push(op.clone());
        }
    }

    Ok(result)
}

/// Upgrade a `Jr{rs}` to `JumpTable{...}` if xrefs provide sufficient case targets.
///
/// Only applies when rs != $ra (register 31). Requires 2+ DISTINCT xref targets that
/// land STRICTLY INSIDE the function body (`func_start < to < func_end`).
///
/// A real jump table targets multiple interior block labels (all `> func_start`). A
/// register-indirect *tail-jump* (e.g. a vtable dispatch `lw $t9,off($a0); jr $t9`)
/// attracts many DATA/RAW_SCAN xrefs that point AT the function's own entry — every
/// vtable slot holding `&fn` resolves to `to == func_start`. Including those produced a
/// degenerate `JumpTable{cases:[func_start; N]}`, whose emitted `default` dispatched the
/// function's own address and self-recursed forever. Excluding `to == func_start` (and
/// requiring ≥2 *distinct* interior targets) leaves such tail-jumps as plain `Jr`, which
/// emits a faithful dynamic `RECOMP_LOOKUP((uint32_t)reg)` dispatch.
fn maybe_upgrade_jr_to_jump_table(
    op: MipsOp,
    _instr_vaddr: u32,
    func_start: u32,
    func_end: u32,
    xrefs: &[(u32, u32)],
) -> MipsOp {
    match op {
        MipsOp::Jr { rs } if rs != Reg::Gpr(31) => {
            // Collect xref targets strictly inside the body (interior block labels);
            // exclude `to == func_start` (the function's own entry) which a vtable /
            // function-pointer tail-jump attracts but is not a jump-table case.
            let mut cases: Vec<u32> = xrefs
                .iter()
                .filter_map(|&(_from, to)| {
                    if to > func_start && to < func_end { Some(to) } else { None }
                })
                .collect();
            cases.sort_unstable();
            cases.dedup();

            if cases.len() >= 2 {
                MipsOp::JumpTable { index_reg: rs, cases }
            } else {
                MipsOp::Jr { rs }
            }
        }
        other => other,
    }
}
