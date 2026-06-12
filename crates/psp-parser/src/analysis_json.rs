//! Serde-compatible types for analysis.json — the output produced by `psprecomp analyze`.
//!
//! The schema uses hex strings for addresses to preserve leading zeros and
//! make the JSON human-readable. Use `format!("0x{:08X}", addr)` when serializing.

use serde::{Deserialize, Serialize};

/// Top-level analysis.json schema.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct AnalysisJson {
    pub binary_path: String,
    pub module_name: String,
    /// Hex string, e.g. "0x08AE0000"
    pub heap_base: String,
    pub functions: Vec<JsonFunction>,
    pub imports: Vec<JsonImport>,
    pub relocations: Vec<JsonReloc>,
    pub xrefs: Vec<JsonXref>,
    pub constructors: Vec<String>,
    pub mid_entries: Vec<JsonMidEntry>,
    pub segments: Vec<JsonSegment>,
}

/// A function detected by Ghidra or cross-validation.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct JsonFunction {
    pub name: String,
    /// Hex string
    pub address: String,
    pub size: u64,
    /// Defaults to false when absent (ExtractAnalysis.java omits external functions).
    #[serde(default)]
    pub is_external: bool,
    pub is_thunk: bool,
    /// "ghidra", "jal_target", or "init_array"
    pub source: String,
}

/// A resolved NID import stub.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct JsonImport {
    /// Hex string, e.g. "0xD632ACDB"
    pub nid: String,
    /// Hex string
    pub stub_addr: String,
    pub name: String,
    pub module_name: String,
}

/// A single relocation entry (after application, stored for Phase 2 reference).
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct JsonReloc {
    /// Hex string
    pub offset: String,
    pub r_type: u8,
    pub ofs_base: u8,
    pub addr_base: u8,
}

/// A cross-reference from Ghidra's ReferenceManager (Layer 1 function pointer detection).
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct JsonXref {
    /// Hex string
    pub from_addr: String,
    /// Hex string
    pub to_addr: String,
    /// "DATA", "CALL", etc.
    pub ref_type: String,
}

/// A mid-function entry point requiring a wrapper in Phase 2.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct JsonMidEntry {
    /// Hex string — the mid-entry address inside the parent function
    pub addr: String,
    /// Hex string — the parent function's entry address
    pub parent_addr: String,
}

/// A PT_LOAD segment with base64-encoded bytes (p_memsz total, BSS zeroed).
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct JsonSegment {
    /// Hex string
    pub p_vaddr: String,
    pub p_filesz: u64,
    /// Always >= p_filesz; the extra bytes are zero (BSS).
    pub p_memsz: u64,
    pub p_flags: u32,
    /// Base64-encoded segment bytes, length == p_memsz
    pub data_b64: String,
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn analysis_json_roundtrip() {
        let json = AnalysisJson {
            binary_path: "BOOT.BIN".into(),
            module_name: "patapon".into(),
            heap_base: "0x08AE0000".into(),
            functions: vec![JsonFunction {
                name: "FUN_08804000".into(),
                address: "0x08804000".into(),
                size: 256,
                is_external: false,
                is_thunk: false,
                source: "ghidra".into(),
            }],
            imports: vec![],
            relocations: vec![],
            xrefs: vec![],
            constructors: vec!["0x08805000".into()],
            mid_entries: vec![],
            segments: vec![],
        };
        // Round-trip through JSON string
        let s = serde_json::to_string(&json).unwrap();
        let back: AnalysisJson = serde_json::from_str(&s).unwrap();
        assert_eq!(back.module_name, "patapon");
        assert_eq!(back.functions[0].address, "0x08804000");
    }
}
