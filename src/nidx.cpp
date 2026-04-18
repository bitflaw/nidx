#include "../include/simdjson.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <cstdlib>
#include <filesystem>
#include <sys/wait.h>
#include <format>
#include <unistd.h>
#include <getopt.h>

namespace fs = std::filesystem;
namespace sjod = simdjson::ondemand;

struct Version
{
  static constexpr int major = 0;
  static constexpr int minor = 0;
  static constexpr int patch = 0;

  static std::string string()
  {
    return std::format("{}.{}.{}", major, minor, patch);
  }
};

void usage (std::string_view pname);
fs::path get_data_dir();
void parse_pkgs ();
void get_pkgs ();

const fs::path DATA_DIR = get_data_dir();
const fs::path TMP_DIR = fs::temp_directory_path();

int main(int argc, char* argv[])
{
  if (argc == 1)
  {
    usage(argv[0]);
    return EXIT_FAILURE;
  }

  parse_pkgs();
  int opt;
  while ((opt = getopt(argc, argv, "hvlPup:s:")) != -1)
  {
    switch (opt)
    {
      case 'h':
        usage(argv[0]);
        exit(EXIT_SUCCESS);
      case 'v':
        std::cout<<argv[0]<<" v"<<Version::string()<<"\n"<<std::endl;
        exit(EXIT_SUCCESS);
      case 'l':
        // lsp mode
        std::cout<<"Coming soon to an OS near you\n"<<std::endl;
        break;
      case 'P':
        //partial_match enabled
        std::cout<<"Coming soon to an OS near you\n"<<std::endl;
        break;
      case 'u':
        fs::remove (DATA_DIR/"pkgs.sqlite3.db");
        parse_pkgs ();
        exit(EXIT_SUCCESS);
      case 'p':
        //package(s) to search for
        std::cout<<"Coming soon to an OS near you\n"<<std::endl;
        break;
      case 's':
        // package set for packages
        std::cout<<"Coming soon to an OS near you\n"<<std::endl;
        break;
      default:
        usage(argv[0]);
        exit(EXIT_FAILURE);
    }
  }
  exit(EXIT_SUCCESS);
}

void usage (std::string_view pname)
{
  std::string usage = "Usage: {0} [-h help] [-v version] [-l enable lsp-mode] "
    "[-u update nixpkgs index]\n\t\t   [-p package(s)] [-s package set for packages]"
    "[-P enable partial matching]\n";
  std::cout<<std::vformat(usage, std::make_format_args(pname))<<std::endl;
}

void get_pkgs ()
{
  std::string tmp_dir_string = TMP_DIR.string();
  std::string data_dir_string = DATA_DIR.string();
  std::string cmd = std::vformat(
        "nix search nixpkgs ^ --json > {0}/tmp.json && mv {0}/tmp.json {1}/nixpkgs.json",
        std::make_format_args(tmp_dir_string, data_dir_string)
      );

  int retval = std::system (cmd.c_str());

  if (!WIFEXITED(retval) || WEXITSTATUS(retval) != 0)
  {
    exit(retval);
  }
}

fs::path get_data_dir()
{
  fs::path data_dir_path {};
  if (const char* xdg = std::getenv("XDG_DATA_HOME"))
  {
    data_dir_path = std::filesystem::path(xdg) / ".nidx";
    if (!fs::exists(data_dir_path)) fs::create_directory(data_dir_path);
    return data_dir_path;
  }

  const char* home = std::getenv("HOME");
  if (!home) throw std::runtime_error("HOME variable not set");

  data_dir_path = std::filesystem::path(home) /".local" / "share" / ".nidx";
  if (!fs::exists(data_dir_path))
  {
    if (!fs::create_directory(data_dir_path))
      throw std::runtime_error ("Failed to create the data directory!");
  }
  return data_dir_path;
}

void lower (std::string& str)
{
  for (char& ch : str) ch = std::tolower(ch);
}

void parse_pkgs ()
{
  fs::path db_name = DATA_DIR/"pkgs.sqlite3.db";
  if (fs::exists(db_name)) return;
  get_pkgs ();
  SQLite::Database db {db_name.string(), SQLite::OPEN_READWRITE|SQLite::OPEN_CREATE};
  db.exec(
      "CREATE TABLE IF NOT EXISTS pkgs (name VARCHAR(255),"
      "desc TEXT, version VARCHAR(15), pkg_set TEXT)"
      );
  db.exec("");

  sjod::parser parser;
  fs::path json_filepath = DATA_DIR/"nixpkgs.json";
  auto pkgs = simdjson::padded_string::load(json_filepath.generic_string());
  sjod::document pkg_doc = parser.iterate(pkgs);

  SQLite::Statement insert_stmt {
    db,
    "INSERT INTO pkgs (name, desc, version, pkg_set) VALUES (?,?,?,?)"
  };
  SQLite::Transaction txn {db};
  sjod::object pkg_iterable = pkg_doc.get_object();
  for (auto pkg : pkg_iterable)
  {
    std::string_view pkg_set = pkg.unescaped_key();
    size_t pos = pkg_set.rfind(".");
    if (pos != std::string_view::npos)
    {
      std::string_view stripped = pkg_set.substr(0, pos);
      pkg_set = stripped;
    }
    sjod::object value = pkg.value();
    std::string_view desc = value["description"];
    std::string_view version = value["version"];
    std::string_view pname = value["pname"].get_string();
    std::string fmt_pname = std::string {pname};
    lower(fmt_pname);

    insert_stmt.bind(1, fmt_pname);
    insert_stmt.bind(2, desc.data());
    insert_stmt.bind(3, version.data());
    insert_stmt.bind(4, pkg_set.data());
    insert_stmt.exec();
    insert_stmt.reset();
  }
  txn.commit();

  db.exec("CREATE INDEX idx_pname ON pkgs (name)");
  db.exec("CREATE INDEX idx_pkgset ON pkgs (pkg_set)");
  db.exec("ANALYZE");
  db.exec("VACUUM");

  fs::remove (json_filepath.c_str());
}
