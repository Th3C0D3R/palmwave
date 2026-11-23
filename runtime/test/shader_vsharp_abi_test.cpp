// Palmwave: the stale-module V# ABI guard (src/shader_library.h, vsharp_abi_compatible), without a GPU.
//
// A synthetic spirv/index.json in a temporary folder holds one entry per case. Translated entries (they carry the
// translator's "capabilities" array, as tools/rdna2spirv.py's shader_meta writes for every module) must name exactly
// vsharp_addressing "signed48-v1"; a missing, different, empty or non-string value is a module from another translator
// revision and must be refused. Hand-written fixture entries with neither field are unversioned and stay accepted, so
// the GLSL stand-in modules of the D3D12 tests keep working. Loading and lookup are unchanged for every entry: only
// pipeline creation applies the guard. No module bytes, game data or GPU are involved.
#include "../src/shader_library.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
}

// A 64-digit hex key per case, so every entry is found on its own.
std::string key(char digit) { return std::string(64, digit); }

}  // namespace

int main() {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / "palmwave_vsharp_abi_test";
    if (ec) {
        std::fprintf(stderr, "no temporary folder: %s\n", ec.message().c_str());
        return 1;
    }
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    if (ec) {
        std::fprintf(stderr, "cannot create %s\n", dir.string().c_str());
        return 1;
    }
    {
        std::ofstream index(dir / "index.json", std::ios::binary);
        index << "{"
                 "\"current\": {\"stage\": \"cs\", \"fssm_sha256\": \"" << key('1') << "\", "
                 "\"capabilities\": [\"Shader\"], \"vsharp_addressing\": \"signed48-v1\"},"
                 "\"stale_missing\": {\"stage\": \"cs\", \"fssm_sha256\": \"" << key('2') << "\", "
                 "\"capabilities\": [\"Shader\"]},"
                 "\"stale_other\": {\"stage\": \"ps\", \"fssm_sha256\": \"" << key('3') << "\", "
                 "\"capabilities\": [], \"vsharp_addressing\": \"zext48\"},"
                 "\"stale_next\": {\"stage\": \"vs\", \"fssm_sha256\": \"" << key('4') << "\", "
                 "\"capabilities\": [\"Shader\"], \"vsharp_addressing\": \"signed48-v2\"},"
                 "\"bad_type\": {\"stage\": \"gs\", \"fssm_sha256\": \"" << key('5') << "\", "
                 "\"capabilities\": [\"Shader\"], \"vsharp_addressing\": 1},"
                 "\"bad_empty\": {\"stage\": \"ms\", \"fssm_sha256\": \"" << key('6') << "\", "
                 "\"vsharp_addressing\": \"\"},"
                 "\"fixture\": {\"stage\": \"ps\", \"fssm_sha256\": \"" << key('7') << "\", \"bindings\": []},"
                 "\"fixture_marked\": {\"stage\": \"vs\", \"fssm_sha256\": \"" << key('8') << "\", "
                 "\"vsharp_addressing\": \"signed48-v1\"},"
                 "\"fixture_wrong\": {\"stage\": \"cs\", \"fssm_sha256\": \"" << key('9') << "\", "
                 "\"vsharp_addressing\": \"zext48\"},"
                 "\"fspx_stale\": {\"stage\": \"cs\", \"fspx_code_sha256\": \"" << key('a') << "\", "
                 "\"capabilities\": [\"Shader\"]}"
                 "}";
    }
    palmwave::ShaderLibrary library;
    std::string error;
    check(library.load(dir, &error), "the synthetic index loads");
    check(library.size() == 10, "every entry is loaded: the guard does not drop entries at load time");

    const auto entry = [&](const char* stage, char digit) { return library.find(stage, key(digit)); };
    const auto accepted = [&](const palmwave::ShaderEntry* e) { return e && palmwave::vsharp_abi_compatible(*e); };
    const auto refused = [&](const palmwave::ShaderEntry* e) { return e && !palmwave::vsharp_abi_compatible(*e); };

    check(accepted(entry("cs", '1')), "a translated module with signed48-v1 is accepted");
    check(entry("cs", '1') && entry("cs", '1')->translator_metadata, "the translator's capabilities array is seen");
    check(refused(entry("cs", '2')), "a translated module without vsharp_addressing (an older translator) is refused");
    check(refused(entry("ps", '3')), "a translated module with another contract is refused");
    check(refused(entry("vs", '4')), "a translated module with a later contract revision is refused");
    check(refused(entry("gs", '5')), "a non-string vsharp_addressing is refused");
    check(entry("gs", '5') && entry("gs", '5')->vsharp_addressing == "<not a string>",
          "a non-string value is recorded as an unknown contract");
    check(refused(entry("ms", '6')), "an empty vsharp_addressing is refused, not mistaken for an unversioned fixture");
    check(accepted(entry("ps", '7')), "a hand-written fixture without translator metadata stays accepted");
    check(entry("ps", '7') && !entry("ps", '7')->translator_metadata && entry("ps", '7')->vsharp_addressing.empty(),
          "the fixture has neither marker");
    check(accepted(entry("vs", '8')), "a fixture that names signed48-v1 is accepted");
    check(refused(entry("cs", '9')), "any entry that names another contract is refused, metadata or not");
    const auto fspx = library.fspx_candidates("cs", key('a'));
    check(fspx.size() == 1 && refused(fspx.front()), "a stale FSPX translation is refused too");

    fs::remove_all(dir, ec);
    if (failures) {
        std::fprintf(stderr, "shader_vsharp_abi_test: %d check(s) failed\n", failures);
        return 1;
    }
    std::printf("shader_vsharp_abi_test: translated modules need vsharp_addressing \"signed48-v1\"; missing, other, "
                "empty and non-string markers are refused; unversioned hand-written fixtures stay accepted\n");
    return 0;
}
