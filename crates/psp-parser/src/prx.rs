use crate::errors::ParseError;

pub const PT_PSPREL1: u32 = 0x700000A0;
pub const PT_PSPREL2: u32 = 0x700000A1;
pub const ET_PSP_PRX: u16 = 0xFFA0;

/// Returns true if the ELF e_type indicates a PSP relocatable module (PRX).
pub fn is_prx(elf: &goblin::elf::Elf) -> bool {
    elf.header.e_type == ET_PSP_PRX
}

/// Returns (type_a_indices, type_b_indices) into elf.program_headers.
pub fn find_reloc_segments(elf: &goblin::elf::Elf) -> (Vec<usize>, Vec<usize>) {
    let mut type_a = Vec::new();
    let mut type_b = Vec::new();
    for (i, ph) in elf.program_headers.iter().enumerate() {
        match ph.p_type {
            PT_PSPREL1 => type_a.push(i),
            PT_PSPREL2 => type_b.push(i),
            _ => {}
        }
    }
    (type_a, type_b)
}

/// Parse SceModuleInfo from raw binary bytes.
///
/// In non-stripped PRX files, SceModuleInfo is in the .lib.ent ELF section.
/// In stripped files (no section headers), it is at p_paddr of the first PT_LOAD segment.
/// Returns (libstub_top, libstub_btm, module_name) on success.
pub fn parse_module_info(
    data: &[u8],
    elf: &goblin::elf::Elf,
) -> Result<(u32, u32, String), ParseError> {
    // Try section-based lookup first (.lib.ent or .rodata.sceModuleInfo)
    for sh in &elf.section_headers {
        let name = elf.shdr_strtab.get_at(sh.sh_name).unwrap_or("");
        if name == ".lib.ent" || name == ".rodata.sceModuleInfo" {
            return read_module_info_at(data, sh.sh_offset as usize);
        }
    }
    // Fall back to p_paddr of first PT_LOAD (stripped PRX)
    use goblin::elf::program_header::PT_LOAD;
    for ph in &elf.program_headers {
        if ph.p_type == PT_LOAD {
            let file_offset = vaddr_to_file_offset(elf, ph.p_paddr as u32)
                .ok_or_else(|| ParseError::Prx {
                    message: format!("Cannot map p_paddr 0x{:08X} to file offset", ph.p_paddr),
                })?;
            return read_module_info_at(data, file_offset);
        }
    }
    Err(ParseError::Prx {
        message: "SceModuleInfo not found via .lib.ent or PT_LOAD p_paddr".into(),
    })
}

/// Maps a virtual address to a file offset using program headers.
pub fn vaddr_to_file_offset(elf: &goblin::elf::Elf, vaddr: u32) -> Option<usize> {
    use goblin::elf::program_header::PT_LOAD;
    for ph in &elf.program_headers {
        if ph.p_type != PT_LOAD {
            continue;
        }
        let start = ph.p_vaddr as u32;
        let end = start.checked_add(ph.p_filesz as u32)?;
        if vaddr >= start && vaddr < end {
            let offset = (vaddr - start) as usize + ph.p_offset as usize;
            return Some(offset);
        }
    }
    None
}

fn read_module_info_at(data: &[u8], offset: usize) -> Result<(u32, u32, String), ParseError> {
    // SceModuleInfo layout (0x34 bytes):
    // u16 module_attrs, u16 module_version, [27]u8 name, u8 pad,
    // u32 gp_value, u32 libent_top, u32 libent_btm, u32 libstub_top, u32 libstub_btm
    use byteorder::{LittleEndian, ReadBytesExt};
    use std::io::Cursor;
    let mut cur = Cursor::new(&data[offset..]);
    let _attrs = cur.read_u16::<LittleEndian>()?;
    let _version = cur.read_u16::<LittleEndian>()?;
    let mut name_bytes = [0u8; 27];
    std::io::Read::read_exact(&mut cur, &mut name_bytes)?;
    let _pad = cur.read_u8()?;
    let _gp = cur.read_u32::<LittleEndian>()?;
    let _libent_top = cur.read_u32::<LittleEndian>()?;
    let _libent_btm = cur.read_u32::<LittleEndian>()?;
    let libstub_top = cur.read_u32::<LittleEndian>()?;
    let libstub_btm = cur.read_u32::<LittleEndian>()?;
    let module_name = String::from_utf8_lossy(
        name_bytes
            .iter()
            .take_while(|&&b| b != 0)
            .copied()
            .collect::<Vec<_>>()
            .as_slice(),
    )
    .into_owned();
    Ok((libstub_top, libstub_btm, module_name))
}
