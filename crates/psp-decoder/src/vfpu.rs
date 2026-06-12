//! VFPU instruction decoder for the PSP Allegrex.
//!
//! Decodes all 14 VFPU top-level opcodes (0x12, 0x18, 0x19, 0x1B,
//! 0x32, 0x34-0x37, 0x3A, 0x3C-0x3F) into typed `MipsOp` variants.
//!
//! Reference: PPSSPP Core/MIPS/MIPSTables.cpp opcode dispatch tree,
//! vfpu-docs YAML encoding specification.

use psp_ir::{MipsOp, Reg, VfpuMatOp, VfpuUnaryOp};

use crate::{branch_target, DecodeError};

// -------------------------------------------------------------------------
// Field extraction helpers
// -------------------------------------------------------------------------

/// Extract VFPU VD register field (bits 6:0).
#[inline]
fn vd(word: u32) -> u8 {
    (word & 0x7F) as u8
}

/// Extract VFPU VS register field (bits 14:8).
#[inline]
fn vs(word: u32) -> u8 {
    ((word >> 8) & 0x7F) as u8
}

/// Extract VFPU VT register field (bits 22:16).
#[inline]
fn vt(word: u32) -> u8 {
    ((word >> 16) & 0x7F) as u8
}

/// Extract vector size from instruction encoding.
///
/// Size is encoded across bits 7 and 15:
/// `size = 1 + bit7 + (bit15 << 1)` giving 1=single, 2=pair,
/// 3=triple, 4=quad.
#[inline]
fn vec_size(word: u32) -> u8 {
    let b7 = (word >> 7) & 1;
    let b15 = (word >> 15) & 1;
    (1 + b7 + (b15 << 1)) as u8
}

/// Extract standard MIPS RT register field (bits 20:16).
#[inline]
fn mips_rt(word: u32) -> Reg {
    Reg::from_u8(((word >> 16) & 0x1F) as u8)
}

/// Extract standard MIPS RS register field (bits 25:21).
#[inline]
fn mips_rs(word: u32) -> Reg {
    Reg::from_u8(((word >> 21) & 0x1F) as u8)
}

// -------------------------------------------------------------------------
// COP2 (opcode 0x12)
// -------------------------------------------------------------------------

/// Decode a COP2 (VFPU, opcode=0x12) instruction word.
///
/// Dispatches on bits 25:21 (rs field) for control/move and branch
/// instructions.
pub(crate) fn decode_cop2(
    word: u32,
    vaddr: u32,
) -> Result<MipsOp, DecodeError> {
    let rs_field = (word >> 21) & 0x1F;

    match rs_field {
        0x00 => {
            // MFV: move from VFPU to GPR
            let rt = mips_rt(word);
            let vd_field = ((word >> 11) & 0x7F) as u8;
            Ok(MipsOp::VfpuMfv { rt, vd: vd_field })
        }
        0x04 => {
            // MTV: move to VFPU from GPR
            let rt = mips_rt(word);
            let vd_field = ((word >> 11) & 0x7F) as u8;
            Ok(MipsOp::VfpuMtv { rt, vd: vd_field })
        }
        0x03 => {
            // MFVC: move from VFPU control
            let rt = mips_rt(word);
            let imm = ((word >> 8) & 0xFF) as u8;
            Ok(MipsOp::VfpuMfvc { rt, imm })
        }
        0x07 => {
            // MTVC: move to VFPU control
            let rt = mips_rt(word);
            let imm = ((word >> 8) & 0xFF) as u8;
            Ok(MipsOp::VfpuMtvc { rt, imm })
        }
        0x08 => {
            // VFPU branch: bit 16 selects bvf(0)/bvt(1),
            // bit 17 selects likely
            let cc = ((word >> 18) & 7) as u8;
            let offset = (word & 0xFFFF) as i16;
            let target = branch_target(vaddr, offset);
            let is_true = (word >> 16) & 1 != 0;
            let likely = (word >> 17) & 1 != 0;
            if is_true {
                Ok(MipsOp::VfpuBvt { cc, target, likely })
            } else {
                Ok(MipsOp::VfpuBvf { cc, target, likely })
            }
        }
        _ => {
            // COP2 sub-ops with bit 24 set (single-precision VFPU ops
            // inside COP2) -- rare. Decode as unary if bit 25 is set,
            // otherwise unknown.
            if rs_field >= 0x10 {
                decode_cop2_vfpu_single(word, vaddr)
            } else {
                Ok(MipsOp::VfpuUnknown {
                    opcode: word,
                    pc: vaddr,
                })
            }
        }
    }
}

/// Decode COP2 single-precision VFPU operations (rs >= 0x10).
///
/// These are VFPU operations encoded inside the COP2 space.
fn decode_cop2_vfpu_single(
    word: u32,
    vaddr: u32,
) -> Result<MipsOp, DecodeError> {
    // Most of these map to VFPU4-style unary ops.
    // For now, route to VfpuUnknown to be filled as needed.
    Ok(MipsOp::VfpuUnknown {
        opcode: word,
        pc: vaddr,
    })
}

// -------------------------------------------------------------------------
// VFPU0 (opcode 0x18): vadd, vsub, vsbn, vdiv
// -------------------------------------------------------------------------

/// Decode VFPU0 group (opcode 0x18).
pub(crate) fn decode_vfpu0(
    word: u32,
    vaddr: u32,
) -> Result<MipsOp, DecodeError> {
    let sub = (word >> 23) & 7;
    let d = vd(word);
    let s = vs(word);
    let t = vt(word);
    let sz = vec_size(word);

    match sub {
        0 => Ok(MipsOp::VfpuAdd {
            vd: d,
            vs: s,
            vt: t,
            size: sz,
        }),
        1 => Ok(MipsOp::VfpuSub {
            vd: d,
            vs: s,
            vt: t,
            size: sz,
        }),
        // 6 => vsbn (subtract-negate, uncommon) -- treat as unknown
        7 => Ok(MipsOp::VfpuDiv {
            vd: d,
            vs: s,
            vt: t,
            size: sz,
        }),
        _ => Ok(MipsOp::VfpuUnknown {
            opcode: word,
            pc: vaddr,
        }),
    }
}

// -------------------------------------------------------------------------
// VFPU1 (opcode 0x19): vmul, vdot, vscl, vhdp, vcrs, vdet
// -------------------------------------------------------------------------

/// Decode VFPU1 group (opcode 0x19).
pub(crate) fn decode_vfpu1(
    word: u32,
    vaddr: u32,
) -> Result<MipsOp, DecodeError> {
    let sub = (word >> 23) & 7;
    let d = vd(word);
    let s = vs(word);
    let t = vt(word);
    let sz = vec_size(word);

    match sub {
        0 => Ok(MipsOp::VfpuMul {
            vd: d,
            vs: s,
            vt: t,
            size: sz,
        }),
        1 => Ok(MipsOp::VfpuDot {
            vd: d,
            vs: s,
            vt: t,
            size: sz,
        }),
        2 => Ok(MipsOp::VfpuScl {
            vd: d,
            vs: s,
            vt: t,
            size: sz,
        }),
        3 => Ok(MipsOp::VfpuHdp {
            vd: d,
            vs: s,
            vt: t,
            size: sz,
        }),
        5 => Ok(MipsOp::VfpuCrs {
            vd: d,
            vs: s,
            vt: t,
            size: sz,
        }),
        6 => Ok(MipsOp::VfpuDet {
            vd: d,
            vs: s,
            vt: t,
            size: sz,
        }),
        _ => Ok(MipsOp::VfpuUnknown {
            opcode: word,
            pc: vaddr,
        }),
    }
}

// -------------------------------------------------------------------------
// VFPU3 (opcode 0x1B): vcmp, vmin, vmax, vscmp, vsge, vslt
// -------------------------------------------------------------------------

/// Decode VFPU3 group (opcode 0x1B).
pub(crate) fn decode_vfpu3(
    word: u32,
    vaddr: u32,
) -> Result<MipsOp, DecodeError> {
    let sub = (word >> 23) & 7;
    let d = vd(word);
    let s = vs(word);
    let t = vt(word);
    let sz = vec_size(word);

    match sub {
        0 => {
            let cond = (word & 0xF) as u8;
            Ok(MipsOp::VfpuCmp {
                vs: s,
                vt: t,
                cond,
                size: sz,
            })
        }
        1 => Ok(MipsOp::VfpuVmin {
            vd: d,
            vs: s,
            vt: t,
            size: sz,
        }),
        2 => Ok(MipsOp::VfpuVmax {
            vd: d,
            vs: s,
            vt: t,
            size: sz,
        }),
        3 => Ok(MipsOp::VfpuScmp {
            vd: d,
            vs: s,
            vt: t,
            size: sz,
        }),
        4 => Ok(MipsOp::VfpuSge {
            vd: d,
            vs: s,
            vt: t,
            size: sz,
        }),
        5 => Ok(MipsOp::VfpuSlt {
            vd: d,
            vs: s,
            vt: t,
            size: sz,
        }),
        _ => Ok(MipsOp::VfpuUnknown {
            opcode: word,
            pc: vaddr,
        }),
    }
}

// -------------------------------------------------------------------------
// VFPU4Jump (opcode 0x34): unary/trig ops -- complex sub-dispatch
// -------------------------------------------------------------------------

/// Decode VFPU4 group (opcode 0x34).
///
/// This is the most complex decode function. Primary dispatch on bits
/// 25:23, then further sub-dispatch on bits 20:16 and 22:21.
pub(crate) fn decode_vfpu4(
    word: u32,
    vaddr: u32,
) -> Result<MipsOp, DecodeError> {
    let d = vd(word);
    let s = vs(word);
    let sz = vec_size(word);
    let primary = (word >> 23) & 7;

    match primary {
        0 => decode_vfpu4_type0(word, d, s, sz, vaddr),
        1 => decode_vfpu4_type1(word, d, s, sz, vaddr),
        2 => decode_vfpu4_type2(word, d, s, sz, vaddr),
        3 => decode_vfpu4_type3(word, d, s, sz, vaddr),
        4 => decode_vfpu4_type4(word, d, s, sz, vaddr),
        5 => decode_vfpu4_type5(word, d, s, sz, vaddr),
        6 => decode_vfpu4_type6(word, d, s, sz, vaddr),
        7 => decode_vfpu4_type7(word, d, s, sz, vaddr),
        _ => Ok(MipsOp::VfpuUnknown {
            opcode: word,
            pc: vaddr,
        }),
    }
}

/// VFPU4 type0: vmov, vabs, vneg, vidt, vsat0, vsat1, vzero, vone,
///               vrcp, vrsq, vsin, vcos, vexp2, vlog2, vsqrt, vasin,
///               vnrcp, vnsin, vrexp2
///
/// PPSSPP tableVFPU4 dispatches on bits 20:16 (5 bits, 32 entries).
/// Indices 0-7 are move/init ops; 16-28 are trig/math ops.
fn decode_vfpu4_type0(
    word: u32,
    d: u8,
    s: u8,
    sz: u8,
    vaddr: u32,
) -> Result<MipsOp, DecodeError> {
    let sub = (word >> 16) & 0x1F;
    let op = match sub {
        0 => VfpuUnaryOp::Mov,
        1 => VfpuUnaryOp::Abs,
        2 => VfpuUnaryOp::Neg,
        3 => VfpuUnaryOp::Idt,
        4 => VfpuUnaryOp::Sat0,
        5 => VfpuUnaryOp::Sat1,
        6 => VfpuUnaryOp::Vzero,
        7 => VfpuUnaryOp::Vone,
        // indices 8-15 are INVALID in PPSSPP
        16 => VfpuUnaryOp::Rcp,
        17 => VfpuUnaryOp::Rsq,
        18 => VfpuUnaryOp::Sin,
        19 => VfpuUnaryOp::Cos,
        20 => VfpuUnaryOp::Exp2,
        21 => VfpuUnaryOp::Log2,
        22 => VfpuUnaryOp::Sqrt,
        23 => VfpuUnaryOp::Asin,
        24 => VfpuUnaryOp::Nrcp,
        26 => VfpuUnaryOp::Nsin,
        28 => VfpuUnaryOp::Rexp2,
        _ => {
            return Ok(MipsOp::VfpuUnknown {
                opcode: word,
                pc: vaddr,
            });
        }
    };
    Ok(MipsOp::VfpuUnary {
        vd: d,
        vs: s,
        op,
        size: sz,
        imm: 0,
    })
}

/// VFPU4 type1: vrcp, vrsq, vsin, vcos, vexp2, vlog2, vsqrt, vasin,
///              vnrcp, vnsin, vrexp2
fn decode_vfpu4_type1(
    word: u32,
    d: u8,
    s: u8,
    sz: u8,
    vaddr: u32,
) -> Result<MipsOp, DecodeError> {
    let sub = (word >> 16) & 0x1F;
    let op = match sub {
        0 => VfpuUnaryOp::Rcp,
        1 => VfpuUnaryOp::Rsq,
        2 => VfpuUnaryOp::Sin,
        3 => VfpuUnaryOp::Cos,
        4 => VfpuUnaryOp::Exp2,
        5 => VfpuUnaryOp::Log2,
        6 => VfpuUnaryOp::Sqrt,
        7 => VfpuUnaryOp::Asin,
        16 => VfpuUnaryOp::Nrcp,
        17 => VfpuUnaryOp::Nsin,
        18 => VfpuUnaryOp::Rexp2,
        _ => {
            return Ok(MipsOp::VfpuUnknown {
                opcode: word,
                pc: vaddr,
            });
        }
    };
    Ok(MipsOp::VfpuUnary {
        vd: d,
        vs: s,
        op,
        size: sz,
        imm: 0,
    })
}

/// VFPU4 type2: vrnds, vrndi, vrndf1, vrndf2, vf2h, vh2f,
///              vsbz, vlgb, vuc2i, vc2i, vus2i, vs2i,
///              vi2uc, vi2c, vi2us, vi2s
fn decode_vfpu4_type2(
    word: u32,
    d: u8,
    s: u8,
    sz: u8,
    vaddr: u32,
) -> Result<MipsOp, DecodeError> {
    let sub = (word >> 16) & 0x1F;
    let op = match sub {
        0 => VfpuUnaryOp::Vrnds,
        1 => VfpuUnaryOp::Vrndi,
        2 => VfpuUnaryOp::Vrndf1,
        3 => VfpuUnaryOp::Vrndf2,
        // 4 => vsbz (uncommon), 5 => vlgb (uncommon)
        8 => VfpuUnaryOp::Vf2h,
        9 => VfpuUnaryOp::Vh2f,
        12 => VfpuUnaryOp::Vuc2i,
        13 => VfpuUnaryOp::Vc2i,
        14 => VfpuUnaryOp::Vus2i,
        15 => VfpuUnaryOp::Vs2i,
        16 => VfpuUnaryOp::Vi2uc,
        17 => VfpuUnaryOp::Vi2c,
        18 => VfpuUnaryOp::Vi2us,
        19 => VfpuUnaryOp::Vi2s,
        _ => {
            return Ok(MipsOp::VfpuUnknown {
                opcode: word,
                pc: vaddr,
            });
        }
    };
    Ok(MipsOp::VfpuUnary {
        vd: d,
        vs: s,
        op,
        size: sz,
        imm: 0,
    })
}

/// VFPU4 type3: vsrt1, vsrt2, vsrt3, vsrt4, vbfy1, vbfy2,
///              vocp/vsocp, vfad, vavg
fn decode_vfpu4_type3(
    word: u32,
    d: u8,
    s: u8,
    sz: u8,
    vaddr: u32,
) -> Result<MipsOp, DecodeError> {
    let sub = (word >> 16) & 0x1F;
    let op = match sub {
        0 => VfpuUnaryOp::Vsrt1,
        1 => VfpuUnaryOp::Vsrt2,
        2 => VfpuUnaryOp::Vbfy1,
        3 => VfpuUnaryOp::Vbfy2,
        4 => VfpuUnaryOp::Vsocp,
        6 => VfpuUnaryOp::Vfad,
        7 => VfpuUnaryOp::Vavg,
        8 => VfpuUnaryOp::Vsrt3,
        9 => VfpuUnaryOp::Vsrt4,
        _ => {
            return Ok(MipsOp::VfpuUnknown {
                opcode: word,
                pc: vaddr,
            });
        }
    };
    Ok(MipsOp::VfpuUnary {
        vd: d,
        vs: s,
        op,
        size: sz,
        imm: 0,
    })
}

/// VFPU4 type4: vcmov0, vcmov1 (conditional move)
fn decode_vfpu4_type4(
    word: u32,
    d: u8,
    s: u8,
    sz: u8,
    vaddr: u32,
) -> Result<MipsOp, DecodeError> {
    // bits 22:21 select cmov variant
    let sub21 = (word >> 21) & 3;
    let cc = (word >> 16) & 0x1F;
    let op = match sub21 {
        0 => VfpuUnaryOp::Cmov0,
        1 => VfpuUnaryOp::Cmov1,
        _ => {
            return Ok(MipsOp::VfpuUnknown {
                opcode: word,
                pc: vaddr,
            });
        }
    };
    Ok(MipsOp::VfpuUnary {
        vd: d,
        vs: s,
        op,
        size: sz,
        imm: cc as u8,
    })
}

/// VFPU4 type5: vf2in, vf2iz, vf2iu, vf2id, vi2f
fn decode_vfpu4_type5(
    word: u32,
    d: u8,
    s: u8,
    sz: u8,
    vaddr: u32,
) -> Result<MipsOp, DecodeError> {
    let sub21 = (word >> 21) & 3;
    let imm_val = ((word >> 16) & 0x1F) as u8;
    let op = match sub21 {
        0 => VfpuUnaryOp::Vf2in,
        1 => VfpuUnaryOp::Vf2iz,
        2 => VfpuUnaryOp::Vf2iu,
        3 => VfpuUnaryOp::Vf2id,
        _ => {
            return Ok(MipsOp::VfpuUnknown {
                opcode: word,
                pc: vaddr,
            });
        }
    };
    Ok(MipsOp::VfpuUnary {
        vd: d,
        vs: s,
        op,
        size: sz,
        imm: imm_val,
    })
}

/// VFPU4 type6: vi2f, vwbn
fn decode_vfpu4_type6(
    word: u32,
    d: u8,
    s: u8,
    sz: u8,
    vaddr: u32,
) -> Result<MipsOp, DecodeError> {
    let sub21 = (word >> 21) & 3;
    let imm_val = ((word >> 16) & 0x1F) as u8;
    match sub21 {
        0 => Ok(MipsOp::VfpuUnary {
            vd: d,
            vs: s,
            op: VfpuUnaryOp::Vi2f,
            size: sz,
            imm: imm_val,
        }),
        1 | 3 => Ok(MipsOp::VfpuUnary {
            vd: d,
            vs: s,
            op: VfpuUnaryOp::Wbn,
            size: sz,
            imm: imm_val,
        }),
        _ => Ok(MipsOp::VfpuUnknown {
            opcode: word,
            pc: vaddr,
        }),
    }
}

/// VFPU4 type7: vcst (constant load)
fn decode_vfpu4_type7(
    word: u32,
    d: u8,
    s: u8,
    sz: u8,
    vaddr: u32,
) -> Result<MipsOp, DecodeError> {
    let sub21 = (word >> 21) & 3;
    let imm_val = ((word >> 16) & 0x1F) as u8;
    match sub21 {
        0 => Ok(MipsOp::VfpuUnary {
            vd: d,
            vs: s,
            op: VfpuUnaryOp::Vcst,
            size: sz,
            imm: imm_val,
        }),
        _ => Ok(MipsOp::VfpuUnknown {
            opcode: word,
            pc: vaddr,
        }),
    }
}

// -------------------------------------------------------------------------
// VFPU5 (opcode 0x37): prefix, viim, vfim
// -------------------------------------------------------------------------

/// Decode VFPU5 group (opcode 0x37).
///
/// PPSSPP tableVFPU5 dispatches on bits 25:23 (3 bits, 8 entries):
///   0,1 -> vpfxs, 2,3 -> vpfxt, 4,5 -> vpfxd, 6 -> viim.s, 7 -> vfim.s
pub(crate) fn decode_vfpu5(
    word: u32,
    _vaddr: u32,
) -> Result<MipsOp, DecodeError> {
    let sub = (word >> 23) & 7;

    match sub {
        0 | 1 => {
            // vpfxs (S prefix)
            Ok(MipsOp::VfpuPrefix {
                reg_idx: 0,
                data: word & 0x000F_FFFF,
            })
        }
        2 | 3 => {
            // vpfxt (T prefix)
            Ok(MipsOp::VfpuPrefix {
                reg_idx: 1,
                data: word & 0x000F_FFFF,
            })
        }
        4 | 5 => {
            // vpfxd (D prefix, only 12 bits)
            Ok(MipsOp::VfpuPrefix {
                reg_idx: 2,
                data: word & 0x0000_0FFF,
            })
        }
        6 => {
            // viim.s
            let t = vt(word);
            let imm = (word & 0xFFFF) as i16;
            Ok(MipsOp::VfpuViim { vt: t, imm })
        }
        7 => {
            // vfim.s
            let t = vt(word);
            let imm = (word & 0xFFFF) as u16;
            Ok(MipsOp::VfpuVfim { vt: t, imm })
        }
        _ => unreachable!("3-bit field always 0-7"),
    }
}

// -------------------------------------------------------------------------
// VFPU6 (opcode 0x3C): matrix ops
// -------------------------------------------------------------------------

/// Decode VFPU6 group (opcode 0x3C).
///
/// Complex sub-dispatch via bits 25:21 for matrix operations.
pub(crate) fn decode_vfpu6(
    word: u32,
    vaddr: u32,
) -> Result<MipsOp, DecodeError> {
    let d = vd(word);
    let s = vs(word);
    let t = vt(word);
    let sz = vec_size(word);
    let sub = (word >> 23) & 7;

    match sub {
        0 => {
            // vmmul
            Ok(MipsOp::VfpuMmul {
                vd: d,
                vs: s,
                vt: t,
                size: sz,
            })
        }
        1 => {
            // vtfm / vhtfm, distinguished by sub-bits
            let sub21 = (word >> 21) & 3;
            match sub21 {
                0 => Ok(MipsOp::VfpuMscl {
                    vd: d,
                    vs: s,
                    vt: t,
                    size: sz,
                }),
                _ => Ok(MipsOp::VfpuUnknown {
                    opcode: word,
                    pc: vaddr,
                }),
            }
        }
        2 => {
            // vtfm2/vhtfm2 or crsp based on sub-bits
            let sub21 = (word >> 21) & 3;
            match sub21 {
                0 => Ok(MipsOp::VfpuHtfm {
                    vd: d,
                    vs: s,
                    vt: t,
                    size: 2,
                }),
                1 => Ok(MipsOp::VfpuTfm {
                    vd: d,
                    vs: s,
                    vt: t,
                    size: 2,
                }),
                _ => Ok(MipsOp::VfpuUnknown {
                    opcode: word,
                    pc: vaddr,
                }),
            }
        }
        3 => {
            // vtfm3/vhtfm3 or crsp
            let sub21 = (word >> 21) & 3;
            match sub21 {
                0 => Ok(MipsOp::VfpuHtfm {
                    vd: d,
                    vs: s,
                    vt: t,
                    size: 3,
                }),
                1 => Ok(MipsOp::VfpuTfm {
                    vd: d,
                    vs: s,
                    vt: t,
                    size: 3,
                }),
                2 => Ok(MipsOp::VfpuCrsp {
                    vd: d,
                    vs: s,
                    vt: t,
                    size: sz,
                }),
                _ => Ok(MipsOp::VfpuUnknown {
                    opcode: word,
                    pc: vaddr,
                }),
            }
        }
        4 => {
            // vtfm4/vhtfm4 or qmul
            let sub21 = (word >> 21) & 3;
            match sub21 {
                0 => Ok(MipsOp::VfpuHtfm {
                    vd: d,
                    vs: s,
                    vt: t,
                    size: 4,
                }),
                1 => Ok(MipsOp::VfpuTfm {
                    vd: d,
                    vs: s,
                    vt: t,
                    size: 4,
                }),
                2 => Ok(MipsOp::VfpuCrsp {
                    vd: d,
                    vs: s,
                    vt: t,
                    size: sz,
                }),
                _ => Ok(MipsOp::VfpuUnknown {
                    opcode: word,
                    pc: vaddr,
                }),
            }
        }
        5 => {
            // PPSSPP tableVFPU6 indices 20-23: vcrsp.t/vqmul.q
            // Dispatches on bits 22:21 but all 4 entries map to the
            // same cross-product/quaternion-multiply operation;
            // vector size distinguishes triple (vcrsp.t) from quad
            // (vqmul.q).
            Ok(MipsOp::VfpuCrsp {
                vd: d,
                vs: s,
                vt: t,
                size: sz,
            })
        }
        6 => {
            // PPSSPP tableVFPU6 indices 24-27: INVALID
            Ok(MipsOp::VfpuUnknown {
                opcode: word,
                pc: vaddr,
            })
        }
        7 => {
            // Matrix unary: vmmov, vmidt, vmzero, vmone, vrot
            let sub21 = (word >> 21) & 3;
            let sub16 = (word >> 16) & 0x1F;
            match sub21 {
                0 => {
                    let mat_op = match sub16 {
                        0 => VfpuMatOp::Mmov,
                        3 => VfpuMatOp::Midt,
                        6 => VfpuMatOp::Mzero,
                        7 => VfpuMatOp::Mone,
                        _ => {
                            return Ok(MipsOp::VfpuUnknown {
                                opcode: word,
                                pc: vaddr,
                            });
                        }
                    };
                    Ok(MipsOp::VfpuMatUnary {
                        vd: d,
                        vs: s,
                        op: mat_op,
                        size: sz,
                        imm: 0,
                    })
                }
                1 => {
                    // vrot: imm5 = lower 5 bits of vt field (rotation control)
                    let rot_imm = ((word >> 16) & 0x1F) as u8;
                    Ok(MipsOp::VfpuMatUnary {
                        vd: d,
                        vs: s,
                        op: VfpuMatOp::Vrot,
                        size: sz,
                        imm: rot_imm,
                    })
                }
                _ => Ok(MipsOp::VfpuUnknown {
                    opcode: word,
                    pc: vaddr,
                }),
            }
        }
        _ => Ok(MipsOp::VfpuUnknown {
            opcode: word,
            pc: vaddr,
        }),
    }
}

// -------------------------------------------------------------------------
// Memory operations
// -------------------------------------------------------------------------

/// Decode lv.s (opcode 0x32): load VFPU single.
///
/// Non-standard register field: `vt = ((op>>16)&0x1F) | ((op&3)<<5)`.
/// Offset: sign-extend of bits 15:2 shifted left by 2.
pub(crate) fn decode_lv_s(
    word: u32,
    _vaddr: u32,
) -> Result<MipsOp, DecodeError> {
    let vt_field =
        (((word >> 16) & 0x1F) | ((word & 3) << 5)) as u8;
    let rs = mips_rs(word);
    let offset = (word & 0xFFFC) as i16;
    Ok(MipsOp::VfpuLvS {
        vt: vt_field,
        rs,
        offset,
    })
}

/// Decode sv.s (opcode 0x3A): store VFPU single.
pub(crate) fn decode_sv_s(
    word: u32,
    _vaddr: u32,
) -> Result<MipsOp, DecodeError> {
    let vt_field =
        (((word >> 16) & 0x1F) | ((word & 3) << 5)) as u8;
    let rs = mips_rs(word);
    let offset = (word & 0xFFFC) as i16;
    Ok(MipsOp::VfpuSvS {
        vt: vt_field,
        rs,
        offset,
    })
}

/// Decode lv.q (opcode 0x36): load VFPU quad.
///
/// Register field: `vt = ((op>>16)&0x1F) | ((op&1)<<5)`.
pub(crate) fn decode_lv_q(
    word: u32,
    _vaddr: u32,
) -> Result<MipsOp, DecodeError> {
    let vt_field =
        (((word >> 16) & 0x1F) | ((word & 1) << 5)) as u8;
    let rs = mips_rs(word);
    let offset = (word & 0xFFFC) as i16;
    Ok(MipsOp::VfpuLvQ {
        vt: vt_field,
        rs,
        offset,
    })
}

/// Decode sv.q (opcode 0x3E): store VFPU quad.
pub(crate) fn decode_sv_q(
    word: u32,
    _vaddr: u32,
) -> Result<MipsOp, DecodeError> {
    let vt_field =
        (((word >> 16) & 0x1F) | ((word & 1) << 5)) as u8;
    let rs = mips_rs(word);
    let offset = (word & 0xFFFC) as i16;
    Ok(MipsOp::VfpuSvQ {
        vt: vt_field,
        rs,
        offset,
    })
}

/// Decode lvl.q/lvr.q (opcode 0x35): unaligned quad load.
///
/// Bit 1 distinguishes left(0) vs right(1).
pub(crate) fn decode_lvlr_q(
    word: u32,
    _vaddr: u32,
) -> Result<MipsOp, DecodeError> {
    let vt_field =
        (((word >> 16) & 0x1F) | ((word & 1) << 5)) as u8;
    let rs = mips_rs(word);
    let offset = (word & 0xFFFC) as i16;
    let is_right = (word >> 1) & 1 != 0;
    if is_right {
        Ok(MipsOp::VfpuLvrQ {
            vt: vt_field,
            rs,
            offset,
        })
    } else {
        Ok(MipsOp::VfpuLvlQ {
            vt: vt_field,
            rs,
            offset,
        })
    }
}

/// Decode svl.q/svr.q (opcode 0x3D): unaligned quad store.
///
/// Bit 1 distinguishes left(0) vs right(1).
pub(crate) fn decode_svlr_q(
    word: u32,
    _vaddr: u32,
) -> Result<MipsOp, DecodeError> {
    let vt_field =
        (((word >> 16) & 0x1F) | ((word & 1) << 5)) as u8;
    let rs = mips_rs(word);
    let offset = (word & 0xFFFC) as i16;
    let is_right = (word >> 1) & 1 != 0;
    if is_right {
        Ok(MipsOp::VfpuSvrQ {
            vt: vt_field,
            rs,
            offset,
        })
    } else {
        Ok(MipsOp::VfpuSvlQ {
            vt: vt_field,
            rs,
            offset,
        })
    }
}

// -------------------------------------------------------------------------
// Tests
// -------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;
    use psp_ir::MipsOp;

    /// Encode a VFPU0 instruction (opcode 0x18).
    fn encode_vfpu0(sub: u8, vd: u8, vs: u8, vt: u8, sz: u8) -> u32 {
        let (b7, b15) = size_bits(sz);
        (0x18u32 << 26)
            | ((sub as u32) << 23)
            | ((vt as u32 & 0x7F) << 16)
            | (b15 << 15)
            | ((vs as u32 & 0x7F) << 8)
            | (b7 << 7)
            | (vd as u32 & 0x7F)
    }

    /// Encode a VFPU5 instruction (opcode 0x37).
    fn encode_vfpu5(sub: u8, data: u32) -> u32 {
        (0x37u32 << 26) | ((sub as u32) << 23) | (data & 0x007F_FFFF)
    }

    /// Encode a lv.s instruction (opcode 0x32).
    fn encode_lv_s(vt: u8, rs: u8, offset: i16) -> u32 {
        let vt_low5 = (vt & 0x1F) as u32;
        let vt_hi2 = ((vt >> 5) & 3) as u32;
        (0x32u32 << 26)
            | ((rs as u32) << 21)
            | (vt_low5 << 16)
            | ((offset as u16 as u32) & 0xFFFC)
            | vt_hi2
    }

    /// Encode a COP2 MFV instruction (opcode 0x12, rs=0x00).
    fn encode_cop2_mfv(rt: u8, vd: u8) -> u32 {
        (0x12u32 << 26)
            | (0x00u32 << 21)
            | ((rt as u32) << 16)
            | ((vd as u32 & 0x7F) << 11)
    }

    /// Encode a VFPU3 vcmp instruction (opcode 0x1B).
    fn encode_vfpu3_vcmp(
        vs: u8,
        vt: u8,
        cond: u8,
        sz: u8,
    ) -> u32 {
        let (b7, b15) = size_bits(sz);
        (0x1Bu32 << 26)
            | (0u32 << 23) // sub=0 for vcmp
            | ((vt as u32 & 0x7F) << 16)
            | (b15 << 15)
            | ((vs as u32 & 0x7F) << 8)
            | (b7 << 7)
            | (cond as u32 & 0xF)
    }

    /// Convert vector size (1-4) to (bit7, bit15) values.
    fn size_bits(sz: u8) -> (u32, u32) {
        // size = 1 + b7 + (b15 << 1)
        let s = (sz - 1) as u32;
        let b7 = s & 1;
        let b15 = (s >> 1) & 1;
        (b7, b15)
    }

    #[test]
    fn test_vec_size_helper() {
        // size=1: b7=0, b15=0
        assert_eq!(vec_size(0x0000_0000), 1);
        // size=2: b7=1, b15=0 => bit 7 set
        assert_eq!(vec_size(0x0000_0080), 2);
        // size=3: b7=0, b15=1 => bit 15 set
        assert_eq!(vec_size(0x0000_8000), 3);
        // size=4: b7=1, b15=1 => both set
        assert_eq!(vec_size(0x0000_8080), 4);
    }

    #[test]
    fn test_decode_vfpu0_vadd_q() {
        // vadd.q with sub=0, vd=0x10, vs=0x20, vt=0x30, size=4
        let word = encode_vfpu0(0, 0x10, 0x20, 0x30, 4);
        let op = decode_vfpu0(word, 0x08800000).unwrap();
        match op {
            MipsOp::VfpuAdd {
                vd, vs, vt, size, ..
            } => {
                assert_eq!(vd, 0x10);
                assert_eq!(vs, 0x20);
                assert_eq!(vt, 0x30);
                assert_eq!(size, 4);
            }
            _ => panic!("Expected VfpuAdd, got {op:?}"),
        }
    }

    #[test]
    fn test_decode_vfpu5_vpfxs() {
        // vpfxs with data=0x000F0E4
        let word = encode_vfpu5(0, 0x000F_00E4);
        let op = decode_vfpu5(word, 0x08800000).unwrap();
        match op {
            MipsOp::VfpuPrefix { reg_idx, data } => {
                assert_eq!(reg_idx, 0, "vpfxs = reg_idx 0");
                assert_eq!(data, 0x000F_00E4 & 0x000F_FFFF);
            }
            _ => panic!("Expected VfpuPrefix, got {op:?}"),
        }
    }

    #[test]
    fn test_decode_lv_s_field_extraction() {
        // lv.s vt=0x25 (5 low bits=0x05, high 2 bits=0x01),
        // rs=4, offset=0x0040
        let word = encode_lv_s(0x25, 4, 0x0040);
        let op = decode_lv_s(word, 0x08800000).unwrap();
        match op {
            MipsOp::VfpuLvS { vt, rs, offset } => {
                assert_eq!(vt, 0x25, "vt should be 0x25");
                assert_eq!(rs, Reg::Gpr(4));
                assert_eq!(offset, 0x0040);
            }
            _ => panic!("Expected VfpuLvS, got {op:?}"),
        }
    }

    #[test]
    fn test_decode_cop2_mfv() {
        // mfv rt=8, vd=3
        let word = encode_cop2_mfv(8, 3);
        let op = decode_cop2(word, 0x08800000).unwrap();
        match op {
            MipsOp::VfpuMfv { rt, vd } => {
                assert_eq!(rt, Reg::Gpr(8));
                assert_eq!(vd, 3);
            }
            _ => panic!("Expected VfpuMfv, got {op:?}"),
        }
    }

    #[test]
    fn test_decode_vfpu3_vcmp() {
        // vcmp with cond=2 (EQ), vs=0x10, vt=0x20, size=4
        let word =
            encode_vfpu3_vcmp(0x10, 0x20, 2, 4);
        let op = decode_vfpu3(word, 0x08800000).unwrap();
        match op {
            MipsOp::VfpuCmp {
                vs, vt, cond, size,
            } => {
                assert_eq!(vs, 0x10);
                assert_eq!(vt, 0x20);
                assert_eq!(cond, 2);
                assert_eq!(size, 4);
            }
            _ => panic!("Expected VfpuCmp, got {op:?}"),
        }
    }

    #[test]
    fn test_vfpu_branch_likely() {
        // COP2 branch: bvt with likely=true
        // opcode=0x12, rs=0x08, bit16=1(true), bit17=1(likely),
        // cc=2
        let word = (0x12u32 << 26)
            | (0x08u32 << 21)
            | (2u32 << 18)      // cc=2
            | (1u32 << 17)      // likely
            | (1u32 << 16)      // true branch
            | (4u16 as u32);    // offset=4
        let op = decode_cop2(word, 0x08800000).unwrap();
        match op {
            MipsOp::VfpuBvt {
                cc,
                target,
                likely,
            } => {
                assert_eq!(cc, 2);
                assert!(likely, "should be likely");
                // target = 0x08800000 + 4 + 4*4 = 0x08800014
                assert_eq!(target, 0x08800014);
            }
            _ => panic!("Expected VfpuBvt, got {op:?}"),
        }
    }

    #[test]
    fn test_decode_vfpu4_type0_vrcp() {
        // vrcp: VFPU4Jump dispatch=0, tableVFPU4[16]
        // 0xD0102020: bits25:21=0, bits20:16=0x10(16)=vrcp
        let op = decode_vfpu4(0xD0102020, 0x08857CBC).unwrap();
        match op {
            MipsOp::VfpuUnary { op, .. } => {
                assert_eq!(op, VfpuUnaryOp::Rcp, "sub=16 should be vrcp");
            }
            _ => panic!("Expected VfpuUnary(Rcp), got {op:?}"),
        }
    }

    #[test]
    fn test_decode_vfpu4_type0_vrsq() {
        // vrsq: VFPU4Jump dispatch=0, tableVFPU4[17]
        // 0xD0110808: bits25:21=0, bits20:16=0x11(17)=vrsq
        let op = decode_vfpu4(0xD0110808, 0x08857E90).unwrap();
        match op {
            MipsOp::VfpuUnary { op, .. } => {
                assert_eq!(op, VfpuUnaryOp::Rsq, "sub=17 should be vrsq");
            }
            _ => panic!("Expected VfpuUnary(Rsq), got {op:?}"),
        }
    }

    #[test]
    fn test_decode_vfpu6_vcrsp() {
        // vcrsp/vqmul: VFPU6 bits25:23=5
        // 0xF2A68224: top6=0x3C, bits25:21=0x15, primary=5
        let op = decode_vfpu6(0xF2A68224, 0x08857EA0).unwrap();
        match op {
            MipsOp::VfpuCrsp { .. } => {}
            _ => panic!("Expected VfpuCrsp, got {op:?}"),
        }
    }

    #[test]
    fn test_decode_vfpu5_vfim() {
        // vfim.s: VFPU5 bits25:23=7
        // 0xDFE6BC00: top6=0x37, sub=7
        let op = decode_vfpu5(0xDFE6BC00, 0x08857CEC).unwrap();
        match op {
            MipsOp::VfpuVfim { vt, imm } => {
                assert_eq!(imm, 0xBC00);
                // vt = bits 22:16 = 0x66
                assert_eq!(vt, 0x66);
            }
            _ => panic!("Expected VfpuVfim, got {op:?}"),
        }
    }

    #[test]
    fn test_decode_vfpu5_vpfxt_correct_sub() {
        // vpfxt: PPSSPP sub=2, should map to reg_idx=1
        let word = encode_vfpu5(2, 0x000F_00E4);
        let op = decode_vfpu5(word, 0x08800000).unwrap();
        match op {
            MipsOp::VfpuPrefix { reg_idx, .. } => {
                assert_eq!(reg_idx, 1, "sub=2 should be vpfxt (reg_idx=1)");
            }
            _ => panic!("Expected VfpuPrefix, got {op:?}"),
        }
    }

    #[test]
    fn test_decode_vfpu5_vpfxd_correct_sub() {
        // vpfxd: PPSSPP sub=4, should map to reg_idx=2
        let word = encode_vfpu5(4, 0x0000_0ABC);
        let op = decode_vfpu5(word, 0x08800000).unwrap();
        match op {
            MipsOp::VfpuPrefix { reg_idx, data } => {
                assert_eq!(reg_idx, 2, "sub=4 should be vpfxd (reg_idx=2)");
                assert_eq!(data, 0x0ABC & 0x0FFF);
            }
            _ => panic!("Expected VfpuPrefix, got {op:?}"),
        }
    }

    #[test]
    fn test_decode_all_8_unknown_opcodes() {
        // Verify all 8 previously-unknown opcodes now decode
        let unknowns = [
            (0xD0102020u32, 0x08857CBCu32),
            (0xD0102222, 0x08857CD0),
            (0xD0100101, 0x08857CD4),
            (0xD0110808, 0x08857E90),
            (0xD0110888, 0x08857EB4),
            (0xF2A68224, 0x08857EA0),
            (0xF2A4A625, 0x08857EA4),
            (0xDFE6BC00, 0x08857CEC),
        ];
        for (word, pc) in unknowns {
            let result =
                crate::decode_word(word, pc);
            match result {
                Ok(MipsOp::VfpuUnknown { .. }) => {
                    panic!(
                        "Opcode 0x{word:08X} at 0x{pc:08X} still decodes as VfpuUnknown"
                    );
                }
                Ok(_) => {} // good, decoded to something
                Err(e) => {
                    panic!(
                        "Opcode 0x{word:08X} at 0x{pc:08X} produced decode error: {e:?}"
                    );
                }
            }
        }
    }
}
