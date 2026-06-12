use thiserror::Error;

#[derive(Debug, Error)]
pub enum ParseError {
    #[error("ELF parse error: {0}")]
    Elf(#[from] goblin::error::Error),
    #[error("I/O error: {0}")]
    Io(#[from] std::io::Error),
    #[error("XML parse error: {0}")]
    Xml(#[from] roxmltree::Error),
    #[error("PRX format error: {message}")]
    Prx { message: String },
    #[error("Relocation error: {message}")]
    Reloc { message: String },
    #[error("NID error: {message}")]
    Nid { message: String },
}
