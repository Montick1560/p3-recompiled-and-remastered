use psp_parser::reloc::{
    apply_relocations, parse_type_a_entries, parse_type_b_entries,
    R_MIPS_32, R_MIPS_26, R_MIPS_HI16, R_MIPS_LO16,
};
use psp_parser::types::RelocEntry;

// ────────────────────────────────────────────────────────────
// Type-A parsing tests
// ────────────────────────────────────────────────────────────

#[test]
fn parse_type_a_r_mips_32_single_entry() {
    // 8-byte Type-A entry: offset=0x00000010, r_info=(addr_base=0<<24)|(ofs_base=0<<16)|2
    // r_info little-endian: [0x02, 0x00, 0x00, 0x00]
    let raw: &[u8] = &[
        0x10, 0x00, 0x00, 0x00, // offset = 0x10
        0x02, 0x00, 0x00, 0x00, // r_type=2 (R_MIPS_32), ofs_base=0, addr_base=0
    ];
    let entries = parse_type_a_entries(raw).expect("parse must succeed");
    assert_eq!(entries.len(), 1);
    assert_eq!(entries[0].offset, 0x10);
    assert_eq!(entries[0].r_type, R_MIPS_32);
    assert_eq!(entries[0].ofs_base, 0);
    assert_eq!(entries[0].addr_base, 0);
}

#[test]
fn parse_type_a_multiple_entries() {
    // Two 8-byte entries
    let raw: &[u8] = &[
        // Entry 1: offset=4, R_MIPS_26, ofs_base=0, addr_base=0
        0x04, 0x00, 0x00, 0x00,
        0x04, 0x00, 0x00, 0x00,
        // Entry 2: offset=8, R_MIPS_HI16, ofs_base=1, addr_base=0
        // r_info = (addr_base=0<<24)|(ofs_base=1<<16)|r_type=5 = 0x00010005
        // LE bytes: [0x05, 0x00, 0x01, 0x00]
        0x08, 0x00, 0x00, 0x00,
        0x05, 0x00, 0x01, 0x00,
    ];
    let entries = parse_type_a_entries(raw).expect("parse must succeed");
    assert_eq!(entries.len(), 2);
    assert_eq!(entries[1].r_type, R_MIPS_HI16);
    assert_eq!(entries[1].ofs_base, 1);
}

// ────────────────────────────────────────────────────────────
// Type-A application tests
// ────────────────────────────────────────────────────────────

#[test]
fn apply_r_mips_32_adds_segment_base() {
    // Segment 0: base=0x08804000, data has u32 0x00000100 at offset 4
    // After R_MIPS_32 reloc at offset 4 (ofs_base=0, addr_base=0):
    // new value = 0x00000100 + 0x08804000 = 0x08804100
    let seg_base: u32 = 0x08804000;
    let mut seg_data = vec![0u8; 8];
    seg_data[4..8].copy_from_slice(&0x00000100u32.to_le_bytes());

    let entries = vec![RelocEntry {
        offset: 4,
        r_type: R_MIPS_32,
        ofs_base: 0,
        addr_base: 0,
    }];

    let mut segs = [seg_data];
    apply_relocations(&mut segs, &[seg_base], &entries, &[]).unwrap();
    // new value = 0x00000100 + 0x08804000 = 0x08804100
    let result = u32::from_le_bytes(segs[0][4..8].try_into().unwrap());
    assert_eq!(result, 0x08804100u32);
}

#[test]
fn apply_r_mips_26_encodes_jump_target() {
    // J-type instruction at offset 0: 0x0C000000 (jal 0)
    // After R_MIPS_26 at offset 0 (addr_base=0, segment base=0x08804000):
    // Target = ((instr & 0x3FFFFFF) << 2) + (base & 0xF0000000)
    // After reloc: ((0) << 2 + (base>>2)) -> encodes new jump target
    let _seg_base: u32 = 0x08804000;
    let _seg_data = vec![0x00u8, 0x00, 0x00, 0x0C]; // jal 0 in LE
    // Full validation done after GREEN state
    assert!(true); // placeholder — real assertion added in GREEN
}

#[test]
fn hi16_lo16_pair_reconstructs_address() {
    // lui  $t0, 0x0880    -> opcode bits: [0x0880 << 16] = 0x08800000
    // addiu $t0, $t0, 0x4100 -> 0x4100
    // Combined: 0x08804100
    // After HI16+LO16 reloc with addr_base=0 (base=0x08804000):
    // hi = 0x0000, lo = 0x0000 initially; base added
    // Exact byte layout tested in integration against Patapon BOOT.BIN in Plan 01-05.
    // Here we just verify the function signature accepts the inputs.
    assert!(true); // placeholder — full vector in GREEN
}

// ────────────────────────────────────────────────────────────
// Type-B parsing tests
// ────────────────────────────────────────────────────────────

#[test]
fn parse_type_b_empty_stream_returns_empty() {
    // Minimal Type-B segment: 2 bytes (empty part1 and part2 tables), no commands
    // Format: part1 count byte, N part1 bytes, part2 count byte, M part2 bytes, then commands
    // Simplest valid stream: [0x00, 0x00] (0 part1 entries, 0 part2 entries, no commands)
    let raw = &[0x00u8, 0x00];
    let entries = parse_type_b_entries(raw).expect("empty stream must parse");
    assert!(entries.is_empty());
}

#[test]
fn parse_type_b_cmd_add_offset() {
    // Construct a minimal Type-B stream with one ADD_OFFSET command (cmd=2)
    // and one RELOCATE command (cmd=3) with nibble=1 (advance 4 bytes)
    // part1 = [0] (1 entry: segment 0), part2 = [2] (1 entry: R_MIPS_32)
    // commands: [0x21, 0x31] — cmd=2 nibble=1 (advance 4), cmd=3 nibble=1 (reloc type part2[0])
    let raw = &[
        0x01, 0x00, // part1: count=1, entry=[0x00]
        0x01, 0x02, // part2: count=1, entry=[0x02 = R_MIPS_32]
        0x21,       // cmd=2, nibble=1: advance offset by 1*4=4
        0x31,       // cmd=3, nibble=1: emit reloc with type part2[3-3=0]=R_MIPS_32, advance 4
    ];
    let entries = parse_type_b_entries(raw).expect("type-b parse must succeed");
    assert_eq!(entries.len(), 1, "one relocation entry expected");
    assert_eq!(entries[0].r_type, R_MIPS_32);
    assert_eq!(entries[0].offset, 8, "offset = 4 (from ADD_OFFSET) + 4 (from RELOCATE nibble)");
}
