// The LSL compatibility suite: how much of the language Falcon runs today.
//
//   homeworldz-script-compat-tests <tailslide scripts folder> <baseline folder> [--update]
//
// Two measures:
//   semantics   the checks in semantics.cpp (what a script computes and says)
//   corpus      Tailslide's test scripts: of the ones Tailslide accepts, how
//               many Falcon compiles
//
// Most of both fail today, so this is not a test that everything passes. It
// records which checks pass (semantics-passing.txt, corpus-passing.txt in the
// baseline folder) and exits non-zero when that set differs from the record:
// a check that stopped passing is a regression, and one that started passing
// must be added so it is held from then on. --update rewrites the record.

#include "harness.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace compat {
void run_semantics();
}

namespace {

namespace fs = std::filesystem;

std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

std::set<std::string> read_record(const fs::path& path) {
    std::set<std::string> names;
    std::ifstream in(path, std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) names.insert(line);
    }
    return names;
}

void write_record(const fs::path& path, const std::set<std::string>& names) {
    std::ofstream out(path, std::ios::binary);
    for (const auto& name : names) out << name << '\n';
}

// Compares what passes with the record. Returns how many names differ.
int compare(const std::string& suite, const std::set<std::string>& passing, std::size_t total,
            const fs::path& record_path, bool update) {
    std::cout << suite << ": " << passing.size() << " of " << total << " pass\n";
    if (update) {
        write_record(record_path, passing);
        std::cout << "  recorded in " << record_path.generic_string() << "\n";
        return 0;
    }
    const auto recorded = read_record(record_path);
    int differences = 0;
    for (const auto& name : recorded) {
        if (passing.contains(name)) continue;
        std::cout << "  REGRESSION  no longer passes: " << name << "\n";
        ++differences;
    }
    for (const auto& name : passing) {
        if (recorded.contains(name)) continue;
        std::cout << "  NEW  now passes and is not recorded: " << name << "\n";
        ++differences;
    }
    return differences;
}

// Whether Tailslide accepts a script: it marks each error it must report with
// $[E1xxxx] on the line, and three scripts are refused without a mark (its
// conformance.cc checks them by hand).
//
// Two of its errors belong to Second Life's retired LSO compiler alone, as the
// scripts that carry them say; Mono compiles those scripts, and so does Falcon.
bool tailslide_accepts(const std::string& relative, const std::string& source) {
    static const std::set<std::string> refused_unmarked{
        "parserstackdepth3.lsl", "print_no_shadowing.lsl", "type_error_no_assert.lsl"};
    static const std::set<std::string> lso_only{"E10021", "E10037"};
    for (std::size_t at = source.find("$[E1"); at != std::string::npos; at = source.find("$[E1", at + 1)) {
        if (!lso_only.contains(source.substr(at + 2, 6))) return false;
    }
    return !refused_unmarked.contains(relative);
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> arguments(argv + 1, argv + argc);
    const bool update = std::erase(arguments, "--update") != 0;
    const bool details = std::erase(arguments, "--details") != 0; // why each failing check fails
    if (arguments.size() != 2) {
        std::cerr << "usage: homeworldz-script-compat-tests <tailslide scripts folder> "
                     "<baseline folder> [--update] [--details]\n";
        return 2;
    }
    const fs::path corpus_root = arguments[0];
    const fs::path baseline_root = arguments[1];
    if (!fs::is_directory(corpus_root) || !fs::is_directory(baseline_root)) {
        std::cerr << "not a folder: " << (fs::is_directory(corpus_root) ? baseline_root : corpus_root).generic_string()
                  << "\n";
        return 2;
    }

    // Semantics.
    compat::run_semantics();
    std::set<std::string> semantics_passing;
    std::set<std::string> semantics_names;
    for (const auto& result : compat::results()) {
        semantics_names.insert(result.name);
        if (result.passed && !semantics_passing.insert(result.name).second) {
            std::cerr << "two checks share the name: " << result.name << "\n";
            return 2;
        }
    }
    int differences = compare("semantics", semantics_passing, compat::results().size(),
                              baseline_root / "semantics-passing.txt", update);
    if (details) {
        for (const auto& result : compat::results()) {
            if (!result.passed) std::cout << "  fails: " << result.name << "\n      " << result.detail << "\n";
        }
    }

    // Corpus.
    std::vector<fs::path> files;
    for (const auto& entry : fs::recursive_directory_iterator(corpus_root))
        if (entry.is_regular_file() && entry.path().extension() == ".lsl") files.push_back(entry.path());
    std::sort(files.begin(), files.end());
    if (files.empty()) {
        std::cerr << "no .lsl files under " << corpus_root.generic_string() << "\n";
        return 2;
    }
    std::set<std::string> corpus_passing;
    std::size_t accepted = 0;
    std::vector<std::string> wrongly_accepted;
    for (const auto& path : files) {
        const std::string relative = fs::relative(path, corpus_root).generic_string();
        const std::string source = read_file(path);
        const bool compiled = compat::compiles(source);
        if (tailslide_accepts(relative, source)) {
            ++accepted;
            if (compiled) corpus_passing.insert(relative);
        } else if (compiled) {
            wrongly_accepted.push_back(relative);
        }
    }
    differences += compare("corpus (scripts Tailslide accepts that Falcon compiles)", corpus_passing, accepted,
                           baseline_root / "corpus-passing.txt", update);
    std::cout << "  " << files.size() - accepted << " scripts Tailslide refuses; Falcon compiles "
              << wrongly_accepted.size() << " of them\n";
    for (const auto& name : wrongly_accepted) std::cout << "    compiles but should not: " << name << "\n";

    if (differences != 0)
        std::cout << differences << " difference(s) from the record. A regression needs fixing; "
                     "a new pass needs recording (run with --update).\n";
    return differences == 0 ? 0 : 1;
}
