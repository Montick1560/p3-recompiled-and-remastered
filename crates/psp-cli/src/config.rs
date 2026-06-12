//! TOML game configuration for per-game stubs, function skips, and patches.
//!
//! Config file location: passed via --config flag.
//! All fields are optional; absence means "use defaults".
//!
//! Example game.toml:
//!   [[stubs]]
//!   name = "sceUtilityGetSystemParamInt"
//!
//!   [[skips]]
//!   address = "0x08804000"
//!   reason = "VFPU crash — implement in Phase 6"
//!
//!   [[patches]]
//!   address = "0x08810004"
//!   instruction = "0x00000000"  # NOP override
//!   reason = "fixup for broken vtable init"

use serde::Deserialize;

/// Top-level game configuration loaded from a TOML file.
#[derive(Debug, Clone, Default, Deserialize)]
pub struct GameConfig {
    /// Functions to replace with no-op HLE stubs (by name).
    #[serde(default)]
    pub stubs: Vec<StubEntry>,
    /// Functions to skip entirely (emitted as empty stubs).
    #[serde(default)]
    pub skips: Vec<SkipEntry>,
    /// Instruction-level patches (override a specific address with a new word).
    #[serde(default)]
    pub patches: Vec<PatchEntry>,
    /// Overrides for batch size (default: 50 or CLI --batch-size).
    #[serde(default)]
    pub functions_per_file: Option<usize>,
}

/// A function name to replace with an HLE no-op stub.
#[derive(Debug, Clone, Deserialize)]
pub struct StubEntry {
    /// Function name as it appears in analysis.json.
    pub name: String,
}

/// A function address to skip entirely (emitted as an empty stub).
#[derive(Debug, Clone, Deserialize)]
pub struct SkipEntry {
    /// Hex address string, e.g. "0x08804000".
    pub address: String,
    #[serde(default)]
    #[allow(dead_code)]
    pub reason: String,
}

/// An instruction-level patch replacing a word at a specific address.
#[derive(Debug, Clone, Deserialize)]
pub struct PatchEntry {
    /// Hex address string.
    pub address: String,
    /// Replacement instruction word as hex string.
    pub instruction: String,
    #[serde(default)]
    #[allow(dead_code)]
    pub reason: String,
}

/// Load game config from a TOML file path. Returns default config if path is None.
pub fn load_config(path: Option<&std::path::Path>) -> anyhow::Result<GameConfig> {
    match path {
        None => Ok(GameConfig::default()),
        Some(p) => {
            let text = std::fs::read_to_string(p)
                .map_err(|e| anyhow::anyhow!("Cannot read config {}: {e}", p.display()))?;
            let cfg: GameConfig = toml::from_str(&text)
                .map_err(|e| anyhow::anyhow!("Invalid TOML in {}: {e}", p.display()))?;
            Ok(cfg)
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn empty_toml_parses_to_defaults() {
        let cfg: GameConfig = toml::from_str("").unwrap();
        assert!(cfg.stubs.is_empty());
        assert!(cfg.skips.is_empty());
        assert!(cfg.patches.is_empty());
        assert!(cfg.functions_per_file.is_none());
    }

    #[test]
    fn stub_entry_parsed() {
        let toml = r#"
        [[stubs]]
        name = "sceKernelSleep"
        "#;
        let cfg: GameConfig = toml::from_str(toml).unwrap();
        assert_eq!(cfg.stubs.len(), 1);
        assert_eq!(cfg.stubs[0].name, "sceKernelSleep");
    }

    #[test]
    fn functions_per_file_override() {
        let toml = "functions_per_file = 100\n";
        let cfg: GameConfig = toml::from_str(toml).unwrap();
        assert_eq!(cfg.functions_per_file, Some(100));
    }
}
