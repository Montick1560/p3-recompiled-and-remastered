use crate::errors::ParseError;
use crate::types::Segment;
use goblin::elf::program_header::PT_LOAD;

/// Parse an ELF/PRX binary from raw bytes.
///
/// Returns the parsed goblin Elf struct. Two common non-ELF inputs from real
/// game dumps get actionable errors instead of a cryptic bad-magic failure
/// (found bringing up an untested game, issue #37 spirit): the `~PSP`
/// encrypted-executable header, and all-zero dummy BOOT.BIN placeholders.
pub fn parse_elf(data: &[u8]) -> Result<goblin::elf::Elf<'_>, ParseError> {
    if data.starts_with(b"~PSP") {
        return Err(ParseError::EncryptedPsp);
    }
    if data.len() >= 64 && data[..64].iter().all(|&b| b == 0) {
        return Err(ParseError::DummyZeroes);
    }
    Ok(goblin::elf::Elf::parse(data)?)
}

#[cfg(test)]
mod parse_guard_tests {
    use super::*;

    #[test]
    fn encrypted_psp_magic_gets_actionable_error() {
        let mut data = b"~PSP".to_vec();
        data.resize(256, 0xAA);
        let err = parse_elf(&data).unwrap_err();
        assert!(matches!(err, ParseError::EncryptedPsp));
        assert!(err.to_string().contains("encrypted ~PSP"));
    }

    #[test]
    fn all_zero_dummy_gets_actionable_error() {
        let data = vec![0u8; 4096];
        let err = parse_elf(&data).unwrap_err();
        assert!(matches!(err, ParseError::DummyZeroes));
        assert!(err.to_string().contains("dummy"));
    }

    #[test]
    fn real_elf_magic_still_reaches_goblin() {
        // \x7fELF but truncated: must NOT hit the guards; goblin reports it.
        let data = b"\x7fELF".to_vec();
        let err = parse_elf(&data).unwrap_err();
        assert!(matches!(err, ParseError::Elf(_)));
    }
}

/// Extract all PT_LOAD segments. BSS region (p_memsz > p_filesz) is zeroed.
///
/// CRITICAL: Always uses p_memsz for the output slice size.
/// Missing this zeroing causes the dlmalloc infinite loop (V1 pitfall #1).
pub fn extract_segments(data: &[u8], elf: &goblin::elf::Elf) -> Vec<Segment> {
    elf.program_headers
        .iter()
        .filter(|ph| ph.p_type == PT_LOAD)
        .map(|ph| {
            let filesz = ph.p_filesz as usize;
            let memsz = ph.p_memsz as usize;
            // Copy file bytes, then zero-fill BSS.
            let file_end = (ph.p_offset as usize) + filesz;
            let file_bytes = &data[ph.p_offset as usize..file_end];
            let mut seg_data = Vec::with_capacity(memsz);
            seg_data.extend_from_slice(file_bytes);
            seg_data.resize(memsz, 0u8); // zero-fill BSS
            Segment {
                p_vaddr: ph.p_vaddr as u32,
                p_filesz: ph.p_filesz as u32,
                p_memsz: ph.p_memsz as u32,
                p_flags: ph.p_flags,
                p_type: ph.p_type,
                data: seg_data,
            }
        })
        .collect()
}

/// Add `load_base` to every segment's `p_vaddr` (no-op when 0).
///
/// Relocatable PRX modules are linked at 0 and rebased to the PSP user-module
/// load base (`crate::prx::PSP_USER_MODULE_BASE` by default) at load time;
/// ET_EXEC binaries load where linked and pass `load_base == 0`. Everything
/// downstream (heap base, relocation bases, JSON segment records, the HLE
/// scan) derives from the rebased `p_vaddr` values automatically.
pub fn rebase_segments(segments: &mut [Segment], load_base: u32) {
    if load_base == 0 {
        return;
    }
    for seg in segments {
        seg.p_vaddr = seg.p_vaddr.wrapping_add(load_base);
    }
}

#[cfg(test)]
mod rebase_tests {
    use super::*;

    fn seg(p_vaddr: u32) -> Segment {
        Segment {
            p_vaddr,
            p_filesz: 16,
            p_memsz: 32,
            p_flags: 5,
            p_type: goblin::elf::program_header::PT_LOAD,
            data: vec![0; 32],
        }
    }

    #[test]
    fn rebase_adds_load_base_to_every_vaddr() {
        let mut segs = [seg(0), seg(0x1000)];
        rebase_segments(&mut segs, 0x0880_4000);
        assert_eq!(segs[0].p_vaddr, 0x0880_4000);
        assert_eq!(segs[1].p_vaddr, 0x0880_5000);
    }

    #[test]
    fn rebase_zero_is_a_noop() {
        let mut segs = [seg(0x0880_4000)];
        rebase_segments(&mut segs, 0);
        assert_eq!(segs[0].p_vaddr, 0x0880_4000);
    }
}

/// Calculate heap base as max(segment end addresses) rounded up to 64KB.
///
/// ASSERT: heap_base > every segment's p_vaddr + p_memsz (V1 pitfall #8).
pub fn calculate_heap_base(segments: &[Segment]) -> u32 {
    const ALIGN_64K: u32 = 64 * 1024;
    let max_end = segments
        .iter()
        .map(|s| s.p_vaddr.saturating_add(s.p_memsz))
        .max()
        .unwrap_or(0);
    let aligned = max_end.wrapping_add(ALIGN_64K - 1) & !(ALIGN_64K - 1);
    assert!(
        aligned >= max_end,
        "heap_base 0x{aligned:08X} must be >= max segment end 0x{max_end:08X}"
    );
    aligned
}
