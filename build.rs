use std::path::{Path, PathBuf};
use walkdir::WalkDir;

fn get_files(path: &str) -> Vec<String> {
    let files: Vec<String> = WalkDir::new(path)
        .into_iter()
        .filter_map(|e| {
            let file = e.unwrap();
            let metadata = file.metadata().unwrap();

            if let Some(filename) = file.path().to_str() {
                if metadata.is_file() && !filename.ends_with(".h") {
                    return Some(file);
                }
            }

            None
        })
        .map(|entry| entry.into_path().to_str().unwrap().to_string())
        .collect();
    files
}

fn add_files(build: &mut cc::Build, path: &str) {
    let files = get_files(path);
    build.files(files);
}

fn build_libopenmpt(env: &str) {
    let mut build = cc::Build::new();

    println!("cargo:rerun-if-changed=external/libopenmpt");

    build.include("external/libopenmpt");
    build.include("external/libopenmpt/common");
    build.include("external/libopenmpt/src");
    build.include("libflac-sys/flac/include");

    if env.contains("windows") {
        build.flag("/std:c++17");
        build.flag("/EHsc");
        build.define("UNICODE", None);
    } else if env.contains("darwin") {
        build.flag("-std=c++17");
    } else {
        build.flag("-std=c++17");
        build.cpp_link_stdlib("stdc++");
    }

    build.define("LIBOPENMPT_BUILD", None);
    build.define("MPT_WITH_FLAC", None);
    build.define("MPT_ENABLE_FILEIO", None);

    add_files(&mut build, "external/libopenmpt/soundlib");
    add_files(&mut build, "external/libopenmpt/common");
    add_files(&mut build, "external/libopenmpt/sounddsp");

    build.file("external/libopenmpt/libopenmpt/libopenmpt_c.cpp");
    build.file("external/libopenmpt/libopenmpt/libopenmpt_cxx.cpp");
    build.file("external/libopenmpt/libopenmpt/libopenmpt_impl.cpp");
    build.file("external/libopenmpt/libopenmpt/libopenmpt_ext_impl.cpp");
    build.file("external/libopenmpt/src/openmpt/soundfile_write/wav_write.cpp");
    build.file("external/libopenmpt/interface.cpp");

    build.compile("cpp_code");

    // linker stuff
    if env.contains("windows") {
        println!("cargo:rustc-link-lib=Rpcrt4");
    } else if env.contains("darwin") {
        println!("cargo:rustc-link-lib=c++");
    } else {
        println!("cargo:rustc-link-lib=stdc++");
    }
}

// The source files a nuade player lists in its nuade_player.cmake (`set(NUADE_PLAYER_SOURCES a.c b.c)`)
fn nuade_player_sources(player_dir: &Path) -> Vec<PathBuf> {
    let cmake = std::fs::read_to_string(player_dir.join("nuade_player.cmake")).unwrap();
    let list = cmake
        .split("NUADE_PLAYER_SOURCES")
        .nth(1)
        .and_then(|rest| rest.split(')').next())
        .unwrap_or_else(|| panic!("No NUADE_PLAYER_SOURCES in {:?}", player_dir));

    list.split_whitespace()
        .map(|f| player_dir.join(f))
        .collect()
}

// Does what cmake/nuade_players.cmake does: nuade finds its players through a generated table.
fn write_nuade_registry(ids: &[String]) -> PathBuf {
    let decls: String = ids
        .iter()
        .map(|id| format!("extern const NuadePlayer nuade_player_{id};\n"))
        .collect();
    let entries: String = ids
        .iter()
        .map(|id| format!("    &nuade_player_{id},\n"))
        .collect();

    let content = format!(
        "#include \"nuade_registry.h\"\n\n{decls}\nconst NuadePlayer* const nuade_registry_players[] = {{\n{entries}    0,\n}};\nconst uint32_t nuade_registry_player_count = {};\n",
        ids.len()
    );

    let out = PathBuf::from(std::env::var("OUT_DIR").unwrap()).join("nuade_players.c");
    std::fs::write(&out, content).unwrap();
    out
}

fn build_nuade(env: &str) {
    let root = Path::new("external/nuade");
    let mut build = cc::Build::new();

    println!("cargo:rerun-if-changed=external/nuade");
    println!("cargo:rerun-if-changed=external/nuade_interface.c");

    build.include(root.join("include"));
    build.include(root.join("src"));
    // Vendored code, warnings are not ours to fix
    build.warnings(false);

    if env.contains("windows") {
        build.flag("/std:c11");
        build.flag("/utf-8");
        build.define("_CRT_SECURE_NO_WARNINGS", None);
    } else {
        build.flag("-std=c11");
    }

    let mut files = get_files(root.join("src").to_str().unwrap());
    files.retain(|f| f.ends_with(".c"));
    build.files(files);

    let mut player_ids: Vec<String> = std::fs::read_dir(root.join("players"))
        .unwrap()
        .map(|e| e.unwrap().path())
        .filter(|p| p.join("nuade_player.cmake").exists())
        .map(|p| p.file_name().unwrap().to_str().unwrap().to_string())
        .collect();
    player_ids.sort();

    for id in &player_ids {
        let dir = root.join("players").join(id);
        build.include(&dir);
        build.files(nuade_player_sources(&dir));
    }

    build.file(write_nuade_registry(&player_ids));
    build.file("external/nuade_interface.c");

    build.compile("nuade_code");

    if !env.contains("windows") {
        println!("cargo:rustc-link-lib=m");
    }
}

fn main() {
    let env = std::env::var("TARGET").unwrap();

    build_libopenmpt(&env);
    build_nuade(&env);
}
