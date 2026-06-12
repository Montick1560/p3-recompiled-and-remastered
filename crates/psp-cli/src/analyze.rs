//! Full `psprecomp analyze` pipeline: Ghidra invocation, Rust post-processing, mid-entry
//! detection, and analysis.json output.

use anyhow::{bail, Context, Result};
use base64::Engine as _;
use psp_parser::analysis_json::{JsonFunction, JsonMidEntry, JsonXref};
use std::path::Path;
use std::process::Command;

/// Invoke Ghidra headless analysis and write raw JSON to `output`.
///
/// Verifies that the ghidra-allegrex plugin is installed before running.
pub fn run_ghidra_analysis(binary: &Path, ghidra_dir: &Path, output: &Path) -> Result<()> {
    // Verify plugin is installed
    let plugin_dir = ghidra_dir.join("Ghidra/Processors/Allegrex");
    if !plugin_dir.exists() {
        bail!(
            "ghidra-allegrex plugin not found at {}\n\
             Install it: copy the Allegrex/ directory from the ghidra-allegrex ZIP \
             into {}/Ghidra/Processors/",
            plugin_dir.display(),
            ghidra_dir.display()
        );
    }

    // Locate ExtractAnalysis.java relative to the binary
    let script = find_analysis_script()?;
    let script_dir = script
        .parent()
        .context("ExtractAnalysis.java has no parent directory")?
        .to_str()
        .context("script directory path not UTF-8")?
        .to_owned();
    let headless = ghidra_dir.join("support/analyzeHeadless");

    // Ghidra aborts if the project parent directory doesn't exist (e.g. a
    // fresh boot cleared /tmp) — create it instead of failing cryptically.
    std::fs::create_dir_all("/tmp/ghidra_projects")
        .context("Failed to create /tmp/ghidra_projects")?;

    let status = Command::new(&headless)
        .args([
            "/tmp/ghidra_projects",
            "psprecomp_analysis",
            "-import",
            binary.to_str().context("binary path not UTF-8")?,
            "-scriptPath",
            &script_dir,
            "-postScript",
            "ExtractAnalysis.java",
            output.to_str().context("output path not UTF-8")?,
            "-deleteProject",
            "-overwrite",
        ])
        .status()
        .with_context(|| format!("Failed to execute {}", headless.display()))?;

    if !status.success() {
        bail!("Ghidra headless failed with exit code {:?}", status.code());
    }
    Ok(())
}

/// Find ExtractAnalysis.java relative to the executable or in `$PWD/analysis/`.
fn find_analysis_script() -> Result<std::path::PathBuf> {
    let candidates = [
        std::env::current_dir()?.join("analysis/ExtractAnalysis.java"),
        std::env::current_exe()?
            .parent()
            .unwrap_or(Path::new("."))
            .join("../analysis/ExtractAnalysis.java"),
    ];
    for path in &candidates {
        if path.exists() {
            return Ok(path.clone());
        }
    }
    bail!("ExtractAnalysis.java not found; expected at ./analysis/ExtractAnalysis.java");
}

/// Detect mid-function entry points from Ghidra xrefs (V1-compatible approach).
///
/// V1's `_find_midentry_points()` used xrefs from Ghidra's ReferenceManager —
/// DATA references from .data/.rodata pointing to addresses inside known function
/// bodies (but not at function starts). This produces ~532 entries for Patapon BOOT.BIN.
///
/// Branch-scan (previous approach) found 31,042 — too broad for Phase 2.
fn detect_mid_entries_from_xrefs(
    functions: &[JsonFunction],
    xrefs: &[JsonXref],
) -> Vec<JsonMidEntry> {
    use std::collections::HashSet;

    let func_starts: HashSet<u64> = functions
        .iter()
        .filter_map(|f| {
            u64::from_str_radix(f.address.trim_start_matches("0x"), 16).ok()
        })
        .collect();

    let mut sorted_funcs: Vec<(u64, u64, String)> = functions
        .iter()
        .filter_map(|f| {
            let start = u64::from_str_radix(f.address.trim_start_matches("0x"), 16).ok()?;
            Some((start, start + f.size, f.address.clone()))
        })
        .collect();
    sorted_funcs.sort_by_key(|(s, _, _)| *s);

    let mut mid_set: HashSet<u64> = HashSet::new();
    let mut mid_entries: Vec<JsonMidEntry> = Vec::new();

    for xref in xrefs {
        let to_addr = match u64::from_str_radix(
            xref.to_addr.trim_start_matches("0x").trim_start_matches("0X"),
            16,
        ) {
            Ok(a) => a,
            Err(_) => continue,
        };
        if func_starts.contains(&to_addr) {
            continue;
        }

        let pos = sorted_funcs.partition_point(|(s, _, _)| *s <= to_addr);
        if pos == 0 {
            continue;
        }
        let (start, end, parent_addr) = &sorted_funcs[pos - 1];
        if to_addr < *start || to_addr >= *end {
            continue;
        }

        if mid_set.insert(to_addr) {
            mid_entries.push(JsonMidEntry {
                addr: format!("0x{to_addr:08X}"),
                parent_addr: parent_addr.clone(),
            });
        }
    }

    mid_entries
}

/// Run HLE entry point scan: discover function pointers passed to HLE APIs.
///
/// Builds the stub address map from either PRX imports or ELF .lib.stub,
/// scans the .text segment for JAL call sites, and merges new entries into
/// the functions list.
fn run_hle_entry_scan(
    elf_obj: &goblin::elf::Elf,
    nid_map: &std::collections::HashMap<u32, String>,
    import_stubs: &[psp_parser::types::ImportStub],
    segments: &[psp_parser::types::Segment],
    seg_data_vecs: &[Vec<u8>],
    functions: &mut Vec<JsonFunction>,
) -> Result<crate::hle_entry_scanner::HleDiscoveryResult> {
    use crate::hle_entry_scanner;

    // Build HLE stub address map
    let stub_map = if !import_stubs.is_empty() {
        // PRX: build from analysis.json imports directly
        hle_entry_scanner::build_stub_map_from_imports(import_stubs)
    } else {
        // ELF (like Patapon): discover via the consolidated .lib.stub walker.
        // seg_data_vecs holds the loaded segment bytes; bases come from the
        // rebased segment vaddrs (ET_EXEC rebases by 0, i.e. linked addresses).
        let seg_bases: Vec<u32> = segments.iter().map(|s| s.p_vaddr).collect();
        hle_entry_scanner::discover_import_stubs_for_elf(
            elf_obj,
            &seg_bases,
            seg_data_vecs,
            nid_map,
        )
    };

    if stub_map.is_empty() {
        tracing::warn!("No HLE funcptr API stubs found; skipping scan");
        return Ok(hle_entry_scanner::HleDiscoveryResult {
            call_sites_scanned: 0,
            discoveries: Vec::new(),
            new_entries: Vec::new(),
            size_corrections: Vec::new(),
            functions_before: functions.len(),
        });
    }

    // Find .text segment (PF_X = 0x1)
    let text_seg = segments.iter().enumerate().find(|(_, s)| {
        s.p_flags & 0x1 != 0 // PF_X: executable
    });

    let (text_bytes, text_base) = match text_seg {
        Some((idx, seg)) => (&seg_data_vecs[idx][..], seg.p_vaddr),
        None => {
            tracing::warn!("No executable segment found; skipping HLE scan");
            return Ok(hle_entry_scanner::HleDiscoveryResult {
                call_sites_scanned: 0,
                discoveries: Vec::new(),
                new_entries: Vec::new(),
                size_corrections: Vec::new(),
                functions_before: functions.len(),
            });
        }
    };

    // Scan for HLE entry points
    let result = hle_entry_scanner::scan_hle_entries(
        text_bytes, text_base, &stub_map, functions,
    );

    tracing::info!(
        "HLE scan: {} call sites, {} discoveries ({} new), {} known",
        result.call_sites_scanned,
        result.discoveries.len(),
        result.new_entries.len(),
        result.discoveries.len() - result.new_entries.len()
    );

    // Merge new entries into functions list
    for entry in &result.new_entries {
        functions.push(JsonFunction {
            name: format!("FUN_{:08X}", entry.address),
            address: format!("0x{:08X}", entry.address),
            size: entry.estimated_size,
            is_external: false,
            is_thunk: false,
            source: "hle_scan".into(),
        });
    }

    Ok(result)
}

/// Full analyze pipeline: Ghidra + Rust post-processing → analysis.json.
pub fn run_analyze(
    binary: &std::path::PathBuf,
    output: &std::path::PathBuf,
    ghidra_dir: Option<&std::path::PathBuf>,
    nid_db: &std::path::PathBuf,
    load_base_override: Option<u32>,
) -> Result<()> {
    use base64::engine::general_purpose::STANDARD as B64;
    use psp_parser::{elf, nid, prx};

    tracing::info!("Analyzing {}", binary.display());

    // 1. Read binary
    let raw_data = std::fs::read(binary)
        .with_context(|| format!("Cannot read {}", binary.display()))?;

    // 2. Parse ELF/PRX; compute the load base exactly once (plan D3)
    let elf_obj = elf::parse_elf(&raw_data)?;
    let is_prx = prx::is_prx(&elf_obj);
    tracing::info!("Binary type: {}", if is_prx { "PRX" } else { "ELF" });
    let load_base = crate::prx_load::compute_load_base(is_prx, load_base_override);
    if is_prx {
        tracing::info!("PRX load base: 0x{load_base:08X}");
    }

    // 3. Extract segments (BSS zeroed to p_memsz) and rebase them. Heap base,
    // seg_bases, JSON segment records, and the HLE scan all derive from the
    // rebased p_vaddr values (no-op for ET_EXEC: load_base == 0).
    let mut segments = elf::extract_segments(&raw_data, &elf_obj);
    elf::rebase_segments(&mut segments, load_base);
    let heap_base = elf::calculate_heap_base(&segments);
    tracing::info!("Heap base: 0x{heap_base:08X}");

    // 4. Parse and apply relocations (PRX only)
    let (mut seg_data_vecs, seg_bases): (Vec<Vec<u8>>, Vec<u32>) =
        segments.iter().map(|s| (s.data.clone(), s.p_vaddr)).unzip();
    let all_reloc_entries = if is_prx {
        crate::prx_load::apply_prx_relocations(
            &raw_data,
            &elf_obj,
            &mut seg_data_vecs,
            &seg_bases,
        )?
    } else {
        vec![]
    };

    // 5. Parse NIDs and import stubs (D6: from the relocated image, so every
    // pointer field is final; D8: hard error — never a silent empty imports[])
    let nid_map = nid::load_nid_database(nid_db)
        .with_context(|| format!("Cannot load NID DB from {}", nid_db.display()))?;
    let (import_stubs, prx_module_info) = if is_prx {
        let (stubs, mi) = crate::prx_load::parse_prx_imports(
            &elf_obj,
            load_base,
            &seg_bases,
            &seg_data_vecs,
            &nid_map,
        )?;
        (stubs, Some(mi))
    } else {
        (vec![], None)
    };
    tracing::info!("Resolved {} import stubs", import_stubs.len());

    // 6. Run Ghidra headless analysis (reuse existing output if available)
    let ghidra_raw_path = output.with_extension("ghidra_raw.json");
    if ghidra_raw_path.exists() {
        tracing::info!(
            "Using existing {}",
            ghidra_raw_path.display()
        );
    } else if let Some(ghidra) = ghidra_dir {
        run_ghidra_analysis(binary, ghidra, &ghidra_raw_path)?;
    } else {
        tracing::warn!(
            "--ghidra-dir not provided; skipping Ghidra analysis"
        );
        std::fs::write(
            &ghidra_raw_path,
            r#"{"functions":[],"xrefs":[],"constructors":[]}"#,
        )?;
    }

    // 7. Load Ghidra raw output
    let ghidra_json_str = std::fs::read_to_string(&ghidra_raw_path)
        .with_context(|| format!("Cannot read {}", ghidra_raw_path.display()))?;
    let ghidra_data: serde_json::Value =
        serde_json::from_str(&ghidra_json_str).context("Ghidra raw JSON is invalid")?;

    // 8. Merge all data into AnalysisJson
    use psp_parser::analysis_json::*;

    let mut functions: Vec<JsonFunction> =
        serde_json::from_value(ghidra_data["functions"].clone())
            .unwrap_or_default();

    let xrefs: Vec<JsonXref> =
        serde_json::from_value(ghidra_data["xrefs"].clone())
            .unwrap_or_default();

    let constructors: Vec<String> =
        serde_json::from_value(ghidra_data["constructors"].clone())
            .unwrap_or_default();

    // 8.1. Merge fixes (plan T5 item 7; both idempotent no-ops on Patapon):
    // rename the module-start function to "entry" (the runtime hard-links the
    // symbol), and drop jal_target artifacts outside the loaded image.
    let entry_va = load_base.wrapping_add(elf_obj.header.e_entry as u32);
    crate::prx_load::rename_entry_function(&mut functions, entry_va);
    let image_start = segments.iter().map(|s| s.p_vaddr).min().unwrap_or(0);
    let image_end = segments
        .iter()
        .map(|s| s.p_vaddr.saturating_add(s.p_memsz))
        .max()
        .unwrap_or(0);
    let dropped =
        crate::prx_load::drop_out_of_image_jal_targets(&mut functions, image_start, image_end);
    if dropped > 0 {
        tracing::info!("Dropped {dropped} out-of-image jal_target artifact(s)");
    }

    // 8.5. HLE entry point discovery
    // Scan decoded instructions near import stub call sites for function
    // pointers passed to sceKernelCreateThread, sceKernelCreateCallback, etc.
    let functions_before = functions.len();
    let hle_result = run_hle_entry_scan(
        &elf_obj,
        &nid_map,
        &import_stubs,
        &segments,
        &seg_data_vecs,
        &mut functions,
    )?;
    if functions.len() > functions_before {
        tracing::info!(
            "HLE scan: {} -> {} functions (+{})",
            functions_before,
            functions.len(),
            functions.len() - functions_before
        );
    }

    // 8.6. Correct giant function sizes by clamping to next boundary
    let size_corrections =
        crate::hle_entry_scanner::correct_function_sizes(&mut functions);
    if !size_corrections.is_empty() {
        tracing::info!(
            "Corrected {} giant function sizes",
            size_corrections.len()
        );
    }

    // 8.7. Write discovered_entries.json audit file
    let discovered_path = output.with_extension("").with_file_name(
        "discovered_entries.json",
    );
    crate::hle_entry_scanner::write_discovered_entries_json(
        &hle_result,
        &size_corrections,
        &discovered_path,
    )?;

    // 9. Build segment records (base64-encoded, p_memsz bytes).
    let json_segments: Vec<JsonSegment> = segments
        .iter()
        .zip(seg_data_vecs.iter())
        .map(|(s, d)| JsonSegment {
            p_vaddr: format!("0x{:08X}", s.p_vaddr),
            p_filesz: s.p_filesz as u64,
            p_memsz: s.p_memsz as u64,
            p_flags: s.p_flags,
            data_b64: B64.encode(d),
        })
        .collect();

    // 10. Mid-entry detection via Ghidra xrefs (V1-compatible, ANALYSIS-07).
    // Now runs on the corrected function list (HLE entries + size fixes),
    // which means xrefs that previously pointed "inside" a giant function may
    // now correctly target the newly-added standalone functions instead.
    let mid_entries = detect_mid_entries_from_xrefs(&functions, &xrefs);
    tracing::info!(
        "Detected {} mid-function entry points (xref-based)",
        mid_entries.len()
    );

    let module_name = binary
        .file_stem()
        .and_then(|s| s.to_str())
        .unwrap_or("unknown")
        .to_lowercase();

    let analysis = AnalysisJson {
        binary_path: binary.display().to_string(),
        module_name,
        heap_base: format!("0x{heap_base:08X}"),
        // PRX provenance (plan T5 item 8): absent for ET_EXEC (skip_serializing_if)
        // so the Patapon analysis.json stays byte-identical.
        prx: prx_module_info.map(|mi| JsonPrxInfo {
            load_base: format!("0x{load_base:08X}"),
            module_name: mi.name.clone(),
            gp: format!("0x{:08X}", mi.gp),
            entry: format!("0x{entry_va:08X}"),
        }),
        functions,
        imports: import_stubs
            .iter()
            .map(|s| JsonImport {
                nid: format!("0x{:08X}", s.nid),
                stub_addr: format!("0x{:08X}", s.stub_addr),
                name: s.name.clone(),
                module_name: s.module_name.clone(),
            })
            .collect(),
        relocations: all_reloc_entries
            .iter()
            .map(|r| JsonReloc {
                offset: format!("0x{:08X}", r.offset),
                r_type: r.r_type,
                ofs_base: r.ofs_base,
                addr_base: r.addr_base,
            })
            .collect(),
        xrefs,
        constructors,
        mid_entries,
        segments: json_segments,
    };

    // 11. Write output
    let json_str =
        serde_json::to_string_pretty(&analysis).context("Failed to serialize analysis.json")?;
    std::fs::write(output, json_str)
        .with_context(|| format!("Failed to write {}", output.display()))?;

    tracing::info!(
        "Analysis complete: {} functions, {} imports, {} mid-entries -> {}",
        analysis.functions.len(),
        analysis.imports.len(),
        analysis.mid_entries.len(),
        output.display()
    );
    Ok(())
}
