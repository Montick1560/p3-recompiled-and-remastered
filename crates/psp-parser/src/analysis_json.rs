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
    /// PRX provenance — present only for relocatable (e_type 0xFFA0) modules.
    /// Absent (not null) for ET_EXEC so their output stays byte-identical.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub prx: Option<JsonPrxInfo>,
    pub functions: Vec<JsonFunction>,
    pub imports: Vec<JsonImport>,
    pub relocations: Vec<JsonReloc>,
    pub xrefs: Vec<JsonXref>,
    pub constructors: Vec<String>,
    pub mid_entries: Vec<JsonMidEntry>,
    pub segments: Vec<JsonSegment>,
}

/// PRX load provenance: how a relocatable module was rebased (issue #52 T5).
///
/// All fields are hex strings per the schema convention.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct JsonPrxInfo {
    /// Load base the module was rebased to, e.g. "0x08804000".
    pub load_base: String,
    /// SceModuleInfo name (e.g. "hacklink") — the in-binary module name, not
    /// the top-level `module_name` file stem.
    pub module_name: String,
    /// Post-relocation $gp value from SceModuleInfo.
    pub gp: String,
    /// Module entry point: load_base + e_entry.
    pub entry: String,
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
            prx: None,
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

    #[test]
    fn prx_none_is_absent_from_serialized_output() {
        // ET_EXEC byte-identity (plan D7): the key must be absent, not null.
        let json = AnalysisJson {
            binary_path: "BOOT.BIN".into(),
            module_name: "patapon".into(),
            heap_base: "0x08AE0000".into(),
            prx: None,
            functions: vec![],
            imports: vec![],
            relocations: vec![],
            xrefs: vec![],
            constructors: vec![],
            mid_entries: vec![],
            segments: vec![],
        };
        let s = serde_json::to_string(&json).unwrap();
        assert!(!s.contains("prx"), "prx key must be absent for ET_EXEC: {s}");
        // Old files (no prx key) deserialize via #[serde(default)].
        let back: AnalysisJson = serde_json::from_str(&s).unwrap();
        assert!(back.prx.is_none());
    }

    #[test]
    fn prx_some_round_trips_all_hex_fields() {
        let prx = JsonPrxInfo {
            load_base: "0x08804000".into(),
            module_name: "examplemod".into(),
            gp: "0x08C85BD0".into(),
            entry: "0x08BC3D98".into(),
        };
        let s = serde_json::to_string(&prx).unwrap();
        let back: JsonPrxInfo = serde_json::from_str(&s).unwrap();
        assert_eq!(back.load_base, "0x08804000");
        assert_eq!(back.module_name, "examplemod");
        assert_eq!(back.gp, "0x08C85BD0");
        assert_eq!(back.entry, "0x08BC3D98");
    }
}
