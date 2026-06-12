//! PRX import stub walker — stub for Plan 01-03 Task 1 compilation.
//! Full implementation added in Task 2.
use byteorder::{LittleEndian, ReadBytesExt};
use std::collections::HashMap;
use std::io::Cursor;
use crate::errors::ParseError;
use crate::prx::vaddr_to_file_offset;
use crate::types::ImportStub;

/// Parse all import stubs from the SceModuleInfo libstub table.
///
/// `libstub_top` and `libstub_btm` are virtual addresses from SceModuleInfo.
/// Each SceStubLibraryEntry is 0x14 bytes (20 bytes) in the standard layout.
///
/// Reference: V1 `imports.py:parse_import_stubs()`.
pub fn parse_import_stubs(
    data: &[u8],
    elf: &goblin::elf::Elf,
    libstub_top: u32,
    libstub_btm: u32,
    nid_db: &HashMap<u32, String>,
) -> Result<Vec<ImportStub>, ParseError> {
    let mut stubs = Vec::new();
    let mut vaddr = libstub_top;

    while vaddr < libstub_btm {
        let file_off = vaddr_to_file_offset(elf, vaddr).ok_or_else(|| ParseError::Prx {
            message: format!("libstub entry vaddr 0x{vaddr:08X} out of file range"),
        })?;

        // SceStubLibraryEntry layout (20 bytes):
        // u32 lib_name_addr, u16 version, u16 flags,
        // u8 func_nid_count, u8 var_nid_count, u16 entry_size,
        // u32 nid_table_addr, u32 stub_table_addr
        let mut cur = Cursor::new(&data[file_off..]);
        let lib_name_addr = cur.read_u32::<LittleEndian>()?;
        let _version = cur.read_u16::<LittleEndian>()?;
        let _flags = cur.read_u16::<LittleEndian>()?;
        let func_count = cur.read_u8()? as usize;
        let _var_count = cur.read_u8()?;
        let entry_size = cur.read_u16::<LittleEndian>()? as u32;
        let nid_table_addr = cur.read_u32::<LittleEndian>()?;
        let stub_table_addr = cur.read_u32::<LittleEndian>()?;

        // Read library name string from lib_name_addr
        let module_name = if lib_name_addr != 0 {
            if let Some(off) = vaddr_to_file_offset(elf, lib_name_addr) {
                read_cstring(data, off)
            } else {
                "unknown".to_string()
            }
        } else {
            "unknown".to_string()
        };

        // Read each NID and corresponding stub address
        for i in 0..func_count {
            let nid = read_u32_at(data, elf, nid_table_addr + i as u32 * 4)?;
            let stub_addr = read_u32_at(data, elf, stub_table_addr + i as u32 * 4)?;
            let name = crate::nid::resolve_nid(nid_db, nid);
            stubs.push(ImportStub {
                nid,
                stub_addr,
                name,
                module_name: module_name.clone(),
            });
        }

        // Advance to next entry; entry_size of 0 means 20 bytes (default)
        let advance = if entry_size > 0 { entry_size } else { 20 };
        vaddr = vaddr.saturating_add(advance);
    }

    Ok(stubs)
}

fn read_u32_at(data: &[u8], elf: &goblin::elf::Elf, vaddr: u32) -> Result<u32, ParseError> {
    let off = vaddr_to_file_offset(elf, vaddr).ok_or_else(|| ParseError::Prx {
        message: format!("vaddr 0x{vaddr:08X} out of file range"),
    })?;
    Ok(u32::from_le_bytes(data[off..off + 4].try_into().unwrap()))
}

fn read_cstring(data: &[u8], offset: usize) -> String {
    let end = data[offset..].iter().position(|&b| b == 0).unwrap_or(64);
    String::from_utf8_lossy(&data[offset..offset + end]).into_owned()
}
