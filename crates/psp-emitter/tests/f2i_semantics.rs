//! Compiles the emitted recomp.h with the host C++ compiler and checks that
//! psp_f2i reproduces PSP float->int saturation on this host.
use psp_emitter::cpp_generator::CppGenerator;
use std::process::Command;

#[test]
fn psp_f2i_saturates_like_the_psp() {
    let dir = std::env::temp_dir().join(format!("psprecomp_f2i_{}", std::process::id()));
    std::fs::create_dir_all(&dir).expect("mkdir");
    std::fs::write(dir.join("recomp.h"), CppGenerator::emit_recomp_h()).expect("write recomp.h");
    let src = dir.join("f2i.cpp");
    std::fs::write(&src, r#"
#include "recomp.h"
#include <cstdio>
#include <limits>
static int fails = 0;
static void check(float in, uint32_t want, const char* what) {
    uint32_t got = (uint32_t)psp_f2i(in);
    if (got != want) { std::printf("%s: got 0x%08X want 0x%08X\n", what, got, want); fails++; }
}
int main() {
    check(std::numeric_limits<float>::quiet_NaN(), 0x7FFFFFFFu, "NaN");
    check(std::numeric_limits<float>::infinity(), 0x7FFFFFFFu, "+inf");
    check(-std::numeric_limits<float>::infinity(), 0x80000000u, "-inf");
    check(3.0e9f, 0x7FFFFFFFu, "+3e9");
    check(-3.0e9f, 0x80000000u, "-3e9");
    check(2147483648.0f, 0x7FFFFFFFu, "2^31");
    check(-2147483648.0f, 0x80000000u, "-2^31");
    check(1.75f, 1u, "1.75");
    check(-1.75f, (uint32_t)-1, "-1.75");
    check(floorf(-1.25f), (uint32_t)-2, "floor(-1.25)");
    if (fails == 0) std::printf("OK\n");
    return fails ? 1 : 0;
}
"#).expect("write source");
    let bin = dir.join("f2i");
    let cc = Command::new("c++")
        .args(["-std=c++17", "-O2", "-I"]).arg(&dir)
        .arg("-o").arg(&bin).arg(&src)
        .output().expect("host c++ compiler must be available");
    assert!(cc.status.success(), "compile failed:\n{}", String::from_utf8_lossy(&cc.stderr));
    let run = Command::new(&bin).output().expect("run f2i harness");
    let stdout = String::from_utf8_lossy(&run.stdout);
    assert_eq!(run.status.code(), Some(0), "{stdout}");
    let _ = std::fs::remove_dir_all(&dir);
}
