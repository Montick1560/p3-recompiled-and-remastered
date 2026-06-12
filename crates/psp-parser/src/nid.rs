//! NID database parser — stub for Plan 01-03 Task 1 compilation.
//! Full implementation added in Task 2.
use std::collections::HashMap;
use std::path::Path;
use crate::errors::ParseError;

/// Parse the PPSSPP NID database XML and return a NID-to-name map.
pub fn load_nid_database(xml_path: &Path) -> Result<HashMap<u32, String>, ParseError> {
    let content = std::fs::read_to_string(xml_path)?;
    let doc = roxmltree::Document::parse(&content)?;
    let mut db: HashMap<u32, String> = HashMap::new();
    for node in doc.descendants() {
        if node.tag_name().name() != "FUNCTION" {
            continue;
        }
        let nid_text = node
            .children()
            .find(|n| n.tag_name().name() == "NID")
            .and_then(|n| n.text());
        let name_text = node
            .children()
            .find(|n| n.tag_name().name() == "NAME")
            .and_then(|n| n.text());
        if let (Some(nid_s), Some(name)) = (nid_text, name_text) {
            let trimmed = nid_s.trim().trim_start_matches("0x").trim_start_matches("0X");
            if let Ok(nid) = u32::from_str_radix(trimmed, 16) {
                db.insert(nid, name.trim().to_string());
            }
        }
    }
    Ok(db)
}

/// Returns the function name for a NID, or a hex fallback string if unknown.
///
/// The fallback is "NID_0xXXXXXXXX" — never panics.
pub fn resolve_nid(db: &HashMap<u32, String>, nid: u32) -> String {
    db.get(&nid)
        .cloned()
        .unwrap_or_else(|| format!("NID_0x{nid:08X}"))
}
