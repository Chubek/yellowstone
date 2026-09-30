// qobjar - create, list, extract and modify `ar` archives.
//
// Operation letters follow ar(1):
//   d delete   p print contents   q quick append   r replace/insert
//   t list table of contents   x extract
// Key letters: c create quietly, s write a symbol index, u update only if
// newer, v be verbose, D make the output deterministic, T make a thin archive.
#include "../qobjcommon/cli.hpp"
#include "../qobjcommon/options.hpp"

#include <algorithm>
#include <map>

using namespace qbfd;
using qobj::OptionSpec;

namespace {

constexpr OptionSpec kHelp{0, "help", false, "", "show this help"};

struct Flags {
  bool verbose = false, deterministic = true, thin = false;
  bool index = true, create = false, updateOnly = false, preserve = false;
  ArchiveFlavor flavor = ArchiveFlavor::Svr4;
  std::string directory = ".";
};

int readEntries(const std::filesystem::path& archive, bool required,
                std::vector<ArchiveEntry>& entries, const Flags& flags,
                std::string& error) {
  entries.clear();
  if (!std::filesystem::exists(archive)) {
    if (required) {
      error = "no such archive: " + archive.string();
      return 1;
    }
    return 0;  // `ar r` creates the archive
  }
  auto opened = qbfd::openArchive(archive);
  if (!opened) {
    error = opened.error().message;
    return 1;
  }
  for (const auto& m : opened->archive->members()) {
    if (m.special) continue;
    ArchiveEntry e;
    e.name = m.name;
    e.mode = m.mode;
    e.mtime = m.mtime;
    e.uid = m.uid;
    e.gid = m.gid;
    if (flags.thin) {
      // A thin member's payload lives beside the archive; read it so the new
      // archive is complete.
      auto payload = opened->archive->readMember(m);
      if (!payload) {
        error = "cannot read thin member " + m.name + ": " + payload.error().message;
        return 1;
      }
      e.data.assign(payload->begin(), payload->end());
    } else {
      auto payload = opened->archive->memberData(m);
      e.data.assign(payload.begin(), payload.end());
    }
    entries.push_back(std::move(e));
  }
  return 0;
}

int listTable(const std::filesystem::path& archive) {
  auto opened = qbfd::openArchive(archive);
  if (!opened) return qobj::reportError(archive.string(), opened.error());
  for (const auto& m : opened->archive->members()) {
    if (m.special) continue;
    // GNU ar prefixes the archive name, which matters when several archives
    // are listed together.
    std::cout << archive.filename().string() << ':' << m.name << '\n';
  }
  return 0;
}

int printMembers(const std::filesystem::path& archive,
                 const std::vector<std::string>& wanted) {
  auto opened = qbfd::openArchive(archive);
  if (!opened) return qobj::reportError(archive.string(), opened.error());
  for (const auto& m : opened->archive->members()) {
    if (m.special) continue;
    if (!wanted.empty() &&
        std::find(wanted.begin(), wanted.end(), m.name) == wanted.end())
      continue;
    auto payload = opened->archive->readMember(m);
    if (!payload) return qobj::reportError(m.name, payload.error());
    std::cout.write(reinterpret_cast<const char*>(payload->data()),
                    static_cast<std::streamsize>(payload->size()));
  }
  return 0;
}

int extract(const std::filesystem::path& archive,
            const std::vector<std::string>& wanted, const Flags& flags) {
  auto opened = qbfd::openArchive(archive);
  if (!opened) return qobj::reportError(archive.string(), opened.error());
  std::error_code ec;
  std::filesystem::create_directories(flags.directory, ec);
  for (const auto& m : opened->archive->members()) {
    if (m.special) continue;
    if (!wanted.empty() &&
        std::find(wanted.begin(), wanted.end(), m.name) == wanted.end())
      continue;
    auto payload = opened->archive->readMember(m);
    if (!payload) return qobj::reportError(m.name, payload.error());
    // A member name may carry a path; only the final component is written, so
    // extracting an archive cannot create directories outside the target.
    auto target = std::filesystem::path(flags.directory) /
                     std::filesystem::path(m.name).filename();
    if (flags.verbose) std::cout << "x - " << m.name << '\n';
    if (!qobj::writeFile(target, *payload))
      return qobj::reportError(target.string(),
                               std::runtime_error("cannot write " + target.string()));
  }
  return 0;
}

// r and q share the member-replacement logic; q only appends.
int addMembers(const std::filesystem::path& archive,
               const std::vector<std::string>& files, const Flags& flags,
               bool replace) {
  std::vector<ArchiveEntry> entries;
  std::string error;
  if (int rc = readEntries(archive, false, entries, flags, error)) {
    std::cerr << "qobjar: " << archive.string() << ": " << error << '\n';
    return rc;
  }
  for (const auto& name : files) {
    auto entry = qbfd::archiveEntryFromFile(name);
    if (!entry) {
      std::cerr << "qobjar: " << name << ": " << entry.error().message << '\n';
      return 1;
    }
    auto it = std::find_if(entries.begin(), entries.end(), [&](const ArchiveEntry& e) {
      return e.name == entry->name;
    });
    if (it != entries.end()) {
      if (!replace) continue;  // q appends only
      if (flags.updateOnly) {
        // -u keeps the archive member when the file is not newer.
        std::error_code ec;
        if (std::filesystem::last_write_time(name, ec) <=
            std::filesystem::last_write_time(archive, ec))
          continue;
      }
      if (flags.verbose) std::cout << "r - " << entry->name << '\n';
      it->data = std::move(entry->data);
      it->mtime = entry->mtime;
    } else {
      if (flags.verbose) std::cout << "a - " << entry->name << '\n';
      entries.push_back(std::move(*entry));
    }
  }
  ArchiveOptions options;
  options.flavor = flags.flavor;
  options.deterministic = flags.deterministic;
  options.index = flags.index;
  auto built = qbfd::buildArchive(entries, options);
  if (!built) return qobj::reportError(archive.string(), built.error());
  if (!qobj::writeFile(archive, *built))
    return qobj::reportError(archive.string(),
                             std::runtime_error("cannot write " + archive.string()));
  return 0;
}

int deleteMembers(const std::filesystem::path& archive,
                  const std::vector<std::string>& names, const Flags& flags) {
  std::vector<ArchiveEntry> entries;
  std::string error;
  if (int rc = readEntries(archive, true, entries, flags, error)) {
    std::cerr << "qobjar: " << archive.string() << ": " << error << '\n';
    return rc;
  }
  std::vector<ArchiveEntry> kept;
  for (auto& e : entries) {
    if (std::find(names.begin(), names.end(), e.name) == names.end())
      kept.push_back(std::move(e));
  }
  ArchiveOptions options;
  options.flavor = flags.flavor;
  options.deterministic = flags.deterministic;
  options.index = flags.index;
  auto built = qbfd::buildArchive(kept, options);
  if (!built) return qobj::reportError(archive.string(), built.error());
  if (!qobj::writeFile(archive, *built))
    return qobj::reportError(archive.string(),
                             std::runtime_error("cannot write " + archive.string()));
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<OptionSpec> specs = {
      kHelp,
      {'v', "verbose", false, "", "list each action as it is taken"},
      {'c', "create", false, "", "suppress the warning when creating an archive"},
      {'s', "index", false, "", "write a symbol index"},
      {'S', "no-index", false, "", "do not write a symbol index"},
      {'u', "update", false, "", "only replace members that are newer"},
      {'D', "deterministic", false, "", "zero timestamps, uid, gid and mode"},
      {'U', "no-deterministic", false, "", "record real timestamps and ownership"},
      {'T', "thin", false, "", "create a thin archive"},
      {'b', "bsd", false, "", "write the BSD flavour (#1/<len> names)"},
      {'P', "preserve", false, "", "keep existing members not named on the command line"},
  };
  qobj::Options options("qobjar", "operation archive [members...]", specs);
  if (!options.parse(argc, argv)) {
    std::cerr << "qobjar: " << options.error() << "\n\n" << options.usage();
    return 2;
  }
  if (options.has(kHelp)) {
    std::cout << options.usage();
    return 0;
  }
  if (options.positional().size() < 2) {
    std::cerr << "qobjar: need an operation and an archive\n\n" << options.usage();
    return 2;
  }
  if (options.has('s') && options.has('S')) {
    std::cerr << "qobjar: choose one of -s and -S\n";
    return 2;
  }
  if (options.has('D') && options.has('U')) {
    std::cerr << "qobjar: choose one of -D and -U\n";
    return 2;
  }

  Flags flags;
  flags.verbose = options.has('v');
  flags.create = options.has('c');
  flags.index = !options.has('S');
  flags.updateOnly = options.has('u');
  flags.thin = options.has('T');
  flags.deterministic = !options.has('U');
  flags.preserve = options.has('P');
  flags.flavor = options.has('b') ? ArchiveFlavor::Bsd
                                  : (flags.thin ? ArchiveFlavor::Thin
                                                : ArchiveFlavor::Svr4);

  // ar(1) spells the operation and its key letters as one word: `ar rcsD`.
  // Split that, applying the trailing letters to the flags.
  std::string operation = options.positional()[0];
  if (!operation.empty() && operation[0] == '-') operation.erase(0, 1);
  if (operation.size() > 1) {
    for (size_t i = 1; i < operation.size(); ++i) {
      switch (char key = operation[i]; key) {
        case 'c': flags.create = true; break;
        case 's': flags.index = true; break;
        case 'S': flags.index = false; break;
        case 'u': flags.updateOnly = true; break;
        case 'v': flags.verbose = true; break;
        case 'D': flags.deterministic = true; break;
        case 'U': flags.deterministic = false; break;
        case 'T': flags.thin = true; break;
        case 'b': flags.flavor = ArchiveFlavor::Bsd; break;
        case 'P': flags.preserve = true; break;
        // Accepted for compatibility and ignored: they select behaviour this
        // tool does not implement, and silently doing the simpler thing is
        // better than refusing the whole command line.
        case 'i': case 'l': case 'N': case 'o': case 'V':
          break;
        default:
          std::cerr << "qobjar: unknown key letter: " << key << '\n';
          return 2;
      }
    }
    operation.resize(1);
  }
  if (flags.thin) flags.flavor = ArchiveFlavor::Thin;
  else if (options.has('b')) flags.flavor = ArchiveFlavor::Bsd;

  const std::filesystem::path archive = options.positional()[1];
  std::vector<std::string> members(options.positional().begin() + 2,
                                   options.positional().end());
  // For `x`, a leading operand that names an existing directory is where to
  // extract; anything else is a member name to select.
  if (operation == "x" && !members.empty() &&
      std::filesystem::is_directory(members.front())) {
    flags.directory = members.front();
    members.erase(members.begin());
  }

  if (operation == "t") return listTable(archive);
  if (operation == "p") return printMembers(archive, members);
  if (operation == "x") return extract(archive, members, flags);
  if (operation == "d") {
    if (members.empty()) {
      std::cerr << "qobjar: d needs at least one member name\n";
      return 2;
    }
    return deleteMembers(archive, members, flags);
  }
  if (operation == "r" || operation == "q") {
    if (members.empty()) {
      std::cerr << "qobjar: " << operation << " needs at least one file\n";
      return 2;
    }
    // `r` creates the archive when it is missing; -c only silences the notice.
    if (!flags.create && !std::filesystem::exists(archive))
      std::cerr << "qobjar: creating " << archive.string() << '\n';
    return addMembers(archive, members, flags, operation == "r");
  }
  std::cerr << "qobjar: unknown operation: " << operation
            << "\nSupported: d p q r t x\n";
  return 2;
}
