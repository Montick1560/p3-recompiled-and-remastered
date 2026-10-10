//! Reachable-extent tests: a function whose decoded range runs past its last
//! reachable instruction into undecodable bytes (a mod code cave followed by
//! encrypted data) must be cut to the code its control flow can reach instead
//! of becoming an empty stub. Found via Patapon 3 DxD's label-op cave
//! 0x08BC428C (OL_Mission.bin data section): `jr ra; lw s0,16(s0)` sets the
//! caller's `s0`; as a stub, the caller wrote a menu entry over a script
//! handler object and the DxD settings menu froze.

use crate::{decode_function, reachable_len};

fn words_to_bytes(words: &[u32]) -> Vec<u8> {
    words.iter().flat_map(|w| w.to_le_bytes()).collect()
}

// The cave, then the first garbage words that follow it in the overlay.
//   0x08BC428C: lui at,0x8ac
//   0x08BC4290: lbu at,-17402(at)
//   0x08BC4294: addiu at,at,-153
//   0x08BC4298: beq at,zero,0x08BC42AC
//   0x08BC429C: ori at,zero,0x0
//   0x08BC42A0: addu sp,ra,sp
//   0x08BC42A4: j 0x08804950
//   0x08BC42A8: addu ra,sp,ra
//   0x08BC42AC: jr ra
//   0x08BC42B0: lw s0,16(s0)
//   0x08BC42B4..: data (0x9F60ED6D at 0x08BC42C4 does not decode)
const CAVE_BASE: u32 = 0x08BC_428C;
const CAVE: [u32; 15] = [
    0x3C01_08AC, 0x9021_BC06, 0x2421_FF67, 0x1020_0004, 0x3401_0000, 0x03FD_E821,
    0x0A20_1254, 0x03BF_F821, 0x03E0_0008, 0x8E10_0010, 0xAF1D_8698, 0x56EE_18D7,
    0x2103_DA6A, 0xE055_A112, 0x9F60_ED6D,
];

#[test]
fn whole_cave_range_does_not_decode() {
    let bytes = words_to_bytes(&CAVE);
    assert!(decode_function(&bytes, CAVE_BASE, &[]).is_err());
}

#[test]
fn cave_is_cut_after_the_jr_ra_delay_slot() {
    let bytes = words_to_bytes(&CAVE);
    assert_eq!(reachable_len(&bytes, CAVE_BASE, &[]), Some(10 * 4));
    let ops = decode_function(&bytes[..40], CAVE_BASE, &[]).expect("cut range decodes");
    assert_eq!(ops.len(), 10);
}

#[test]
fn reachable_undecodable_word_gives_none() {
    // beq at,zero -> 0x08BC42C4 (the undecodable word) is reachable.
    let mut words = CAVE;
    words[3] = 0x1020_0009;
    assert_eq!(reachable_len(&words_to_bytes(&words), CAVE_BASE, &[]), None);
}

#[test]
fn extra_root_extends_the_walk() {
    // A mid-entry at 0x08BC42B4 (data) makes the walk reach the bad word.
    let bytes = words_to_bytes(&CAVE);
    assert_eq!(reachable_len(&bytes, CAVE_BASE, &[0x08BC_42B4]), None);
}

#[test]
fn indirect_jump_is_not_cut() {
    // jr t9 (0x03200008) may be a switch whose cases follow it: no cut.
    let words = [0x0320_0008, 0x0000_0000, 0x9F60_ED6D];
    assert_eq!(reachable_len(&words_to_bytes(&words), 0x0880_0000, &[]), None);
}

#[test]
fn backward_loop_and_unconditional_branch() {
    // 0: addiu a0,a0,-1 ; 4: bne a0,zero,0 ; 8: nop ; C: b 0x18 ; 10: nop ;
    // 14: garbage (skipped by the b) ; 18: jr ra ; 1C: nop ; 20: garbage
    let words = [
        0x2484_FFFF, 0x1480_FFFE, 0x0000_0000, 0x1000_0002, 0x0000_0000, 0x9F60_ED6D,
        0x03E0_0008, 0x0000_0000, 0x9F60_ED6D,
    ];
    assert_eq!(reachable_len(&words_to_bytes(&words), 0x0880_0000, &[]), Some(8 * 4));
}
