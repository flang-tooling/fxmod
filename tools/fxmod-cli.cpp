// Thin CLI built only on the public fxmod C++ API (<fxmod/module.hpp>).
// `info`/`emit-source` exist for manual testing and for diffing against
// fxmod.py's own `dump`/`info` output on the same fixture files. `wrap`
// is fxmod.py's wrap command generalized: instead of hand-building a
// Flang-specific checksummed module file, it converts a mismatched module
// to portable Fortran source and compiles *that* with the real target
// compiler, so the target produces its own native module file.
#include <fxmod/module.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

void usage() {
  std::cerr
      << "usage: fxmod-cli info <file>\n"
         "       fxmod-cli info --list <file>\n"
         "       fxmod-cli emit-source [--best-effort] <file> [-o "
         "<output>]\n"
         "       fxmod-cli wrap [--target gfortran|flang|ifx] [--cache "
         "<dir>] [--best-effort] [-v] [-n] -- <command...>\n";
}

int run_info(const std::vector<std::string> &args) {
  bool list = false;
  std::string file;
  for (const std::string &a : args) {
    if (a == "--list")
      list = true;
    else
      file = a;
  }
  if (file.empty()) {
    usage();
    return 2;
  }

  fxmod::ModuleFile mf = fxmod::ModuleFile::open(file);
  std::cout << "file      : " << file << "\n";
  std::cout << "family    : " << fxmod::family_name(mf.family()) << "\n";
  std::cout << "version   : " << mf.version() << "\n";
  std::cout << "module    : " << mf.name() << "\n";

  if (mf.family() == fxmod::Family::Gfortran) {
    std::vector<fxmod::ModuleFile::SymbolSummary> syms =
        mf.gfortran_public_symbols();
    std::cout << "public    : " << syms.size() << "\n";
    std::map<std::string, int> flavors;
    for (const auto &s : syms) {
      std::string key = s.flavor;
      if (s.is_artificial)
        key += " (artificial)";
      flavors[key]++;
    }
    std::cout << "public symbols by flavor:\n";
    for (const auto &[flavor, count] : flavors)
      std::cout << "  " << flavor << " " << count << "\n";
    if (list) {
      std::cout << "public symbols:\n";
      for (const auto &s : syms)
        std::cout << "  " << s.name << "\t" << s.flavor << "\n";
    }

    fxmod::EmitResult r = mf.emit_fortran_source(/*strict=*/false);
    std::cout << "untranslatable : " << r.problems.size() << "\n";
    for (const std::string &p : r.problems)
      std::cout << "  - " << p << "\n";
  } else if (mf.family() == fxmod::Family::Ifx) {
    std::vector<fxmod::ModuleFile::IfxEntitySummary> entities =
        mf.ifx_entities();
    std::cout << "entities  : " << entities.size() << "\n";
    if (list) {
      std::cout << "entities:\n";
      for (const auto &e : entities) {
        std::cout << "  " << e.name << "\tdecl_rec_kind=" << e.decl_rec_kind
                   << " sym_id=" << e.sym_id
                   << " class=" << static_cast<int>(e.klass)
                   << " skind=" << static_cast<int>(e.skind);
        if (e.value)
          std::cout << " value=" << *e.value;
        if (e.dummy_arg_type)
          std::cout << " type=" << *e.dummy_arg_type;
        if (e.field_type)
          std::cout << " type=" << *e.field_type;
        if (e.struct_type_name)
          std::cout << " type=" << *e.struct_type_name;
        if (e.array_element_type) {
          std::cout << " array(type=" << *e.array_element_type
                     << " ndimen=" << static_cast<int>(*e.array_ndimen)
                     << " size=" << *e.array_size;
          if (e.array_bounds)
            std::cout << " bounds=" << *e.array_bounds;
          else
            std::cout << ", bounds not decoded";
          std::cout << ")";
        }
        if (e.is_function) {
          std::cout << " " << (*e.is_function ? "function" : "subroutine")
                     << "(";
          for (std::size_t i = 0; i < e.argument_names.size(); ++i) {
            if (i)
              std::cout << ", ";
            std::cout << e.argument_names[i];
          }
          std::cout << ")";
          if (e.return_type)
            std::cout << " -> " << *e.return_type;
        }
        if (!e.field_names.empty()) {
          std::cout << " fields(";
          for (std::size_t i = 0; i < e.field_names.size(); ++i) {
            if (i)
              std::cout << ", ";
            std::cout << e.field_names[i];
          }
          std::cout << ")";
        }
        if (!e.common_variable_names.empty()) {
          std::cout << " common(";
          for (std::size_t i = 0; i < e.common_variable_names.size(); ++i) {
            if (i)
              std::cout << ", ";
            std::cout << e.common_variable_names[i];
          }
          std::cout << ")";
        }
        std::cout << "\n";
      }
    }
  }
  return 0;
}

int run_emit_source(const std::vector<std::string> &args) {
  bool strict = true;
  std::string file, output;
  for (std::size_t i = 0; i < args.size(); ++i) {
    if (args[i] == "--best-effort")
      strict = false;
    else if (args[i] == "-o" && i + 1 < args.size())
      output = args[++i];
    else
      file = args[i];
  }
  if (file.empty()) {
    usage();
    return 2;
  }

  fxmod::ModuleFile mf = fxmod::ModuleFile::open(file);
  fxmod::EmitResult r = mf.emit_fortran_source(strict);

  if (output.empty()) {
    std::cout << r.source;
  } else {
    std::ofstream out(output, std::ios::binary);
    out << r.source;
  }
  for (const std::string &p : r.problems)
    std::cerr << "fxmod-cli: skipped: " << p << "\n";
  return 0;
}

// ---------------------------------------------------------------------
// wrap
// ---------------------------------------------------------------------

// Search-path flags recognized on the wrapped command line, mirroring
// fxmod.py's module_search_dirs(). "-module" is ifx/ifort's own form,
// included so wrap also finds modules when the wrapped command is ifx.
const char *const kPathFlagsJoined[] = {"-I", "-J"};
const char *const kPathFlagsSeparate[] = {"-I", "-J", "-module-dir",
                                           "-module",
                                           "-fintrinsic-modules-path"};

std::vector<std::string> module_search_dirs(const std::vector<std::string> &argv) {
  std::vector<std::string> dirs;
  for (std::size_t i = 0; i < argv.size(); ++i) {
    const std::string &arg = argv[i];
    bool matched = false;
    for (const char *flag : kPathFlagsSeparate) {
      if (arg == flag && i + 1 < argv.size()) {
        dirs.push_back(argv[i + 1]);
        ++i;
        matched = true;
        break;
      }
    }
    if (matched)
      continue;
    for (const char *flag : kPathFlagsJoined) {
      std::size_t flen = std::strlen(flag);
      if (arg.rfind(flag, 0) == 0 && arg.size() > flen) {
        dirs.push_back(arg.substr(flen));
        matched = true;
        break;
      }
    }
    if (matched)
      continue;
    const std::string prefix = "-fintrinsic-modules-path=";
    if (arg.rfind(prefix, 0) == 0)
      dirs.push_back(arg.substr(prefix.size()));
  }
  dirs.push_back(".");
  return dirs;
}

fxmod::Family family_from_compiler_name(const std::string &argv0) {
  std::string base = fs::path(argv0).filename().string();
  for (char &c : base)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (base.find("ifx") != std::string::npos ||
      base.find("ifort") != std::string::npos)
    return fxmod::Family::Ifx;
  if (base.find("flang") != std::string::npos) // catches flang, flang-new,
                                                 // flang-tidy
    return fxmod::Family::Flang;
  if (base.find("gfortran") != std::string::npos)
    return fxmod::Family::Gfortran;
  return fxmod::Family::Unknown;
}

// Flags to make each family's compiler write its module file into `dir`
// when compiling the converted source. gfortran and Flang both accept
// `-J<dir>` (Flang documents it as an alias of `-module-dir`, single-
// valued, but that constraint only matters when the wrapped command's own
// flags are being edited -- these are argv fxmod-cli builds itself for a
// one-off, controlled invocation). ifx's `-module <dir>` form is the
// classic ifort convention, unverified against a real ifx invocation --
// if it's wrong for your ifx version, this is the one thing to fix.
std::vector<std::string> module_output_flags(fxmod::Family target,
                                              const std::string &dir) {
  switch (target) {
  case fxmod::Family::Gfortran:
  case fxmod::Family::Flang:
    return {"-J" + dir};
  case fxmod::Family::Ifx:
    return {"-module", dir};
  default:
    return {};
  }
}

int run_child(const std::vector<std::string> &argv, const std::string &cwd,
              bool quiet = false) {
  std::vector<char *> cargv;
  cargv.reserve(argv.size() + 1);
  for (const std::string &a : argv)
    cargv.push_back(const_cast<char *>(a.c_str()));
  cargv.push_back(nullptr);

  pid_t pid = fork();
  if (pid < 0) {
    std::perror("fxmod-cli: fork");
    return -1;
  }
  if (pid == 0) {
    if (!cwd.empty() && chdir(cwd.c_str()) != 0) {
      std::perror("fxmod-cli: chdir");
      _exit(127);
    }
    if (quiet) {
      int null_fd = open("/dev/null", O_WRONLY);
      if (null_fd >= 0) {
        dup2(null_fd, STDOUT_FILENO);
        dup2(null_fd, STDERR_FILENO);
        close(null_fd);
      }
    }
    execvp(cargv[0], cargv.data());
    std::perror(("fxmod-cli: exec " + argv[0]).c_str());
    _exit(127);
  }
  int status = 0;
  if (waitpid(pid, &status, 0) < 0) {
    std::perror("fxmod-cli: waitpid");
    return -1;
  }
  if (WIFEXITED(status))
    return WEXITSTATUS(status);
  return -1;
}

std::string lower_copy(std::string s) {
  for (char &c : s)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// The modules a translated source USEs, lowercased: the emitter writes each
// as a `use <name>[, only: ...]` line, intrinsic ones as `use, intrinsic ::`.
std::vector<std::string> used_modules(const std::string &source) {
  std::vector<std::string> out;
  std::istringstream in(source);
  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind("use ", 0) != 0)
      continue;
    std::string name = line.substr(4, line.find(',') == std::string::npos
                                          ? std::string::npos
                                          : line.find(',') - 4);
    while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back())))
      name.pop_back();
    out.push_back(lower_copy(name));
  }
  return out;
}

// The intrinsic types among those gfortran modules commonly use that the
// target compiler lacks (Flang on x86-64 has no real(16), for one), found by
// compiling a declaration of each: an interface using one could only fail
// to compile there, and is left out instead (see fxmod::EmitOptions).
std::vector<std::pair<std::string, int>>
probe_unsupported_kinds(const std::string &compiler, const fs::path &build) {
  static const std::pair<const char *, int> kCandidates[] = {
      {"REAL", 10}, {"REAL", 16}, {"INTEGER", 16}};
  std::vector<std::pair<std::string, int>> out;
  for (const auto &[category, kind] : kCandidates) {
    std::string type = lower_copy(category) + "(" + std::to_string(kind) + ")";
    fs::path src = build / ("fxmod_probe_" + lower_copy(category) +
                            std::to_string(kind) + ".f90");
    std::ofstream(src) << "subroutine fxmod_probe\n  " << type
                       << " :: x\nend subroutine fxmod_probe\n";
    if (run_child({compiler, "-fsyntax-only", src.string()}, build.string(),
                  /*quiet=*/true) != 0)
      out.emplace_back(category, kind);
  }
  return out;
}

std::string cache_dir_for(const std::string &explicit_dir) {
  if (!explicit_dir.empty())
    return explicit_dir;
  const char *xdg = std::getenv("XDG_CACHE_HOME");
  std::string base = xdg && *xdg ? xdg
                                  : std::string(std::getenv("HOME") ? std::getenv("HOME") : ".") +
                                        "/.cache";
  return base + "/fxmod";
}

// FNV-1a over the file's bytes, used only to key the "already converted"
// stamp files -- not related to gfortran/Flang's own checksums.
std::string content_hash(const fs::path &path) {
  std::ifstream f(path, std::ios::binary);
  std::uint64_t h = 0xcbf29ce484222325ull;
  char c;
  while (f.get(c)) {
    h ^= static_cast<unsigned char>(c);
    h *= 0x100000001b3ull;
  }
  char buf[17];
  std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(h));
  return std::string(buf, 16);
}

int run_wrap(const std::vector<std::string> &args) {
  std::string target_name, cache_arg;
  bool verbose = false, dry_run = false, best_effort = false;
  std::vector<std::string> command;
  std::size_t i = 0;
  for (; i < args.size(); ++i) {
    if (args[i] == "--target" && i + 1 < args.size())
      target_name = args[++i];
    else if (args[i] == "--cache" && i + 1 < args.size())
      cache_arg = args[++i];
    else if (args[i] == "-v" || args[i] == "--verbose")
      verbose = true;
    else if (args[i] == "-n" || args[i] == "--dry-run")
      dry_run = true;
    else if (args[i] == "--best-effort")
      best_effort = true;
    else if (args[i] == "--") {
      command.assign(args.begin() + i + 1, args.end());
      break;
    } else {
      std::cerr << "fxmod-cli: wrap: unrecognized option '" << args[i]
                << "'\n";
      usage();
      return 2;
    }
  }
  if (command.empty()) {
    std::cerr << "fxmod-cli: wrap needs a command after --\n";
    usage();
    return 2;
  }

  fxmod::Family target = fxmod::Family::Unknown;
  if (!target_name.empty()) {
    if (target_name == "gfortran")
      target = fxmod::Family::Gfortran;
    else if (target_name == "flang")
      target = fxmod::Family::Flang;
    else if (target_name == "ifx")
      target = fxmod::Family::Ifx;
    else {
      std::cerr << "fxmod-cli: wrap: unknown --target '" << target_name
                << "' (want gfortran, flang, or ifx)\n";
      return 2;
    }
  } else {
    target = family_from_compiler_name(command[0]);
  }
  if (target == fxmod::Family::Unknown) {
    std::cerr << "fxmod-cli: wrap: could not tell which compiler family '"
              << command[0]
              << "' is -- pass --target gfortran|flang|ifx explicitly\n";
    return 2;
  }

  std::vector<std::string> dirs =
      module_search_dirs(std::vector<std::string>(command.begin() + 1, command.end()));
  fs::path cache = cache_dir_for(cache_arg);
  fs::path build = cache / ".build";
  std::error_code ec;
  fs::create_directories(cache, ec);
  fs::create_directories(build, ec);

  int converted = 0, skipped = 0, already = 0;

  // A converted module that USEs another converted module can only compile
  // once that one's target-format .mod is in the cache. The module files
  // are therefore all translated first, and compiled afterwards in
  // dependency order, which is read off the `use` lines of the translated
  // source (the only place a module's dependencies are spelled out).
  struct Pending {
    fs::path mod_path, src_path, stamp, out_mod;
    std::string stem;
    std::vector<std::string> uses; // lowercased module names
  };
  std::vector<Pending> pending;
  std::optional<fxmod::EmitOptions> options; // probed on first need
  for (const std::string &dir_str : dirs) {
    fs::path dir(dir_str);
    if (!fs::is_directory(dir))
      continue;
    std::vector<fs::path> mods;
    for (const auto &entry : fs::directory_iterator(dir, ec)) {
      if (entry.path().extension() == ".mod")
        mods.push_back(entry.path());
    }
    std::sort(mods.begin(), mods.end());

    for (const fs::path &mod_path : mods) {
      std::unique_ptr<fxmod::ModuleFile> mf;
      try {
        mf = std::make_unique<fxmod::ModuleFile>(
            fxmod::ModuleFile::open(mod_path.string()));
      } catch (const std::exception &) {
        continue; // not a module file this library recognizes at all
      }
      if (mf->family() == target)
        continue; // already the target's own format

      std::string stem = mod_path.stem().string();
      std::string hash = content_hash(mod_path);
      fs::path stamp = cache / ("." + stem + "." + hash + ".stamp");
      fs::path out_mod = cache / (stem + ".mod");
      if (fs::exists(stamp) && fs::exists(out_mod)) {
        ++already;
        continue;
      }

      if (!options) {
        options.emplace();
        options->strict = !best_effort;
        options->unsupported_kinds = probe_unsupported_kinds(command[0], build);
        // What a translation's `use` statements can find: every module
        // file on the search path or already converted into the cache.
        std::vector<std::string> available;
        std::vector<std::string> scan = dirs;
        scan.push_back(cache.string());
        for (const std::string &d : scan) {
          if (!fs::is_directory(d))
            continue;
          for (const auto &entry : fs::directory_iterator(d, ec))
            if (entry.path().extension() == ".mod")
              available.push_back(lower_copy(entry.path().stem().string()));
        }
        options->available_modules = std::move(available);
        if (verbose)
          for (const auto &[category, kind] : options->unsupported_kinds)
            std::cerr << "fxmod-cli: " << command[0] << " lacks "
                      << lower_copy(category) << "(" << kind
                      << "); leaving out interfaces that use it\n";
      }

      fxmod::EmitResult r;
      try {
        r = mf->emit_fortran_source(*options);
      } catch (const std::exception &e) {
        if (verbose)
          std::cerr << "fxmod-cli: skipping " << mod_path << ": " << e.what()
                     << "\n";
        ++skipped;
        continue;
      }
      if (best_effort && !r.problems.empty() && verbose)
        std::cerr << "fxmod-cli: " << mod_path << ": " << r.problems.size()
                   << " symbol(s) skipped (--best-effort)\n";

      fs::path src_path = cache / (stem + ".f90");
      {
        std::ofstream out(src_path, std::ios::binary);
        out << r.source;
      }
      pending.push_back({mod_path, src_path, stamp, out_mod, stem,
                         used_modules(r.source)});
    }
  }

  // Depth-first, dependencies before dependents; a dependency outside this
  // run (already cached, or not converted at all) imposes no order.
  std::map<std::string, std::size_t> by_name;
  for (std::size_t k = 0; k < pending.size(); ++k)
    by_name.emplace(lower_copy(pending[k].stem), k);
  std::vector<std::size_t> order;
  std::vector<int> state(pending.size(), 0); // 0 new, 1 visiting, 2 done
  std::function<void(std::size_t)> visit = [&](std::size_t k) {
    if (state[k] != 0)
      return; // done, or a cycle -- which cannot compile in any order
    state[k] = 1;
    for (const std::string &dep : pending[k].uses) {
      auto it = by_name.find(dep);
      if (it != by_name.end())
        visit(it->second);
    }
    state[k] = 2;
    order.push_back(k);
  };
  for (std::size_t k = 0; k < pending.size(); ++k)
    visit(k);

  for (std::size_t k : order) {
    const Pending &p = pending[k];
    std::vector<std::string> cc = {command[0]};
    std::vector<std::string> out_flags = module_output_flags(target, cache.string());
    cc.insert(cc.end(), out_flags.begin(), out_flags.end());
    cc.push_back("-I" + cache.string());
    for (const std::string &d : dirs)
      cc.push_back("-I" + d);
    cc.push_back("-c");
    cc.push_back(fs::absolute(p.src_path).string());

    if (dry_run) {
      if (verbose) {
        std::cerr << "fxmod-cli: [dry-run] would run:";
        for (const auto &a : cc)
          std::cerr << " " << a;
        std::cerr << "\n";
      }
      continue;
    }

    int status = run_child(cc, build.string());
    if (status != 0) {
      std::cerr << "fxmod-cli: " << command[0]
                 << " failed to compile converted module '" << p.stem
                 << "' (exit " << status << "), leaving original in place\n";
      ++skipped;
      continue;
    }
    if (!fs::exists(p.out_mod)) {
      std::cerr << "fxmod-cli: converting '" << p.stem
                 << "' compiled cleanly but did not produce " << p.out_mod
                 << " -- check module_output_flags() for this target\n";
      ++skipped;
      continue;
    }

    for (const auto &entry : fs::directory_iterator(cache, ec)) {
      std::string name = entry.path().filename().string();
      if (name.rfind("." + p.stem + ".", 0) == 0 &&
          name.size() > 7 && name.substr(name.size() - 6) == ".stamp")
        fs::remove(entry.path(), ec);
    }
    std::ofstream(p.stamp).close();
    ++converted;
    if (verbose)
      std::cerr << "fxmod-cli: converted " << p.mod_path << " -> " << p.out_mod
                 << "\n";
  }

  if (verbose)
    std::cerr << "fxmod-cli: " << converted << " converted, " << already
               << " already cached, " << skipped << " skipped, cache "
               << cache << "\n";

  std::vector<std::string> new_command = {command[0], "-I" + cache.string()};
  new_command.insert(new_command.end(), command.begin() + 1, command.end());

  if (dry_run) {
    for (std::size_t j = 0; j < new_command.size(); ++j)
      std::cout << (j ? " " : "") << new_command[j];
    std::cout << "\n";
    return 0;
  }

  std::vector<char *> cargv;
  cargv.reserve(new_command.size() + 1);
  for (const std::string &a : new_command)
    cargv.push_back(const_cast<char *>(a.c_str()));
  cargv.push_back(nullptr);
  execvp(cargv[0], cargv.data());
  std::perror(("fxmod-cli: exec " + new_command[0]).c_str());
  return 127;
}

} // namespace

int main(int argc, char **argv) {
  std::vector<std::string> args(argv + 1, argv + argc);
  if (args.empty()) {
    usage();
    return 2;
  }
  std::string cmd = args[0];
  std::vector<std::string> rest(args.begin() + 1, args.end());

  try {
    if (cmd == "info")
      return run_info(rest);
    if (cmd == "emit-source")
      return run_emit_source(rest);
    if (cmd == "wrap")
      return run_wrap(rest);
    usage();
    return 2;
  } catch (const std::exception &e) {
    std::cerr << "fxmod-cli: " << e.what() << "\n";
    return 1;
  }
}
