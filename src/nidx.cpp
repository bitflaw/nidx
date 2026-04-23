#include "../include/simdjson.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <cstdlib>
#include <filesystem>
#include <sys/wait.h>
#include <format>
#include <unistd.h>
#include <getopt.h>

#define BOLD_WHITE "\033[1;97m"
#define GREEN      "\033[32m"
#define BLUE       "\033[1;34m"
#define YELLOW     "\033[33m"
#define DIM        "\033[90m"
#define RESET      "\033[0m"

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
void parse_pkgs (bool unstable = false);
void get_pkgs (bool get_unstable = false);
void query_pkgs (std::vector<std::string>& target_pkgs,
    std::string& pkg_set,
    bool enable_exact_matching = false,
    bool use_unstable = false
    );

const fs::path DATA_DIR = get_data_dir();
const fs::path TMP_DIR = fs::temp_directory_path();
const fs::path stable_db =   DATA_DIR/"stable.sqlite3.db";
const fs::path unstable_db = DATA_DIR/"unstable.sqlite3.db";

int main(int argc, char* argv[])
{
  if (argc == 1)
  {
    usage(argv[0]);
    return EXIT_FAILURE;
  }

  parse_pkgs();
  int opt;

  bool enable_exact_matching = false;
  bool use_unstable = false;
  std::vector<std::string> search_pkgs {};
  std::string search_pkg_set;

  while ((opt = getopt(argc, argv, "hvleuU:p:s:")) != -1)
  {
    switch (opt)
    {
      case 'h':
        usage(argv[0]);
        exit(EXIT_SUCCESS);
      case 'v':
        std::cout<<argv[0]+2<<" v"<<Version::string()<<std::endl;
        exit(EXIT_SUCCESS);
      case 'l': // lsp mode
        std::cout<<"Coming soon to an OS near you!"<<std::endl;
        exit(EXIT_FAILURE);
      case 'U': // update the index
        if (std::string{optarg} == "unstable")
        {
          fs::remove (unstable_db);
          parse_pkgs (true);
          exit(EXIT_SUCCESS);
        }
        else if (std::string{optarg} == "stable")
        {
          fs::remove (stable_db);
          parse_pkgs (false);
          exit(EXIT_SUCCESS);
        }
        else
        {
          usage(argv[0]);
          exit(EXIT_FAILURE);
        }
      case 'u': // search the unstable index
        if (!fs::exists(unstable_db))
        {
          std::cout<<"FETCHING FROM UNSTABLE..."<<std::endl;
          parse_pkgs(true);
        }
        use_unstable = true;
        break;
      case 'p': //package(s) to search for
        search_pkgs.emplace_back(optarg);
        break;
      case 's': // pkg-set to search for packages
        search_pkg_set = std::string {optarg};
        break;
      case 'e': //exact matching enabled
        enable_exact_matching = true;
        break;
      default:
        usage(argv[0]);
        exit(EXIT_FAILURE);
    }
  }
  for (int i = optind; i < argc; i++) search_pkgs.emplace_back(argv[i]);
  query_pkgs (search_pkgs, search_pkg_set, enable_exact_matching, use_unstable);

  exit(EXIT_SUCCESS);
}

void usage (std::string_view pname)
{
  std::string usage_str = "Usage: {} [-h help] [-v version] [-l enable lsp-mode] "
    "[-U {{unstable|stable}} update package index]\n\t   [-p package(s)] [-s package set for packages]"
    "[-e enable exact matching]\n\t   [-u use unstable to search]";
  std::cout<<std::vformat(usage_str, std::make_format_args(pname))<<std::endl;
}

void query_pkgs (
    std::vector<std::string>& target_pkgs,
    std::string& pkg_set,
    bool enable_exact_matching,
    bool use_unstable
  )
{
  std::string partial = "";
  if (!enable_exact_matching)
  {
    partial += "(";
    for (size_t i = 0; i< target_pkgs.size(); i++)
    {
      partial += "name LIKE ?";
      if (i < target_pkgs.size() -1) partial += " OR ";
    }
    partial += ")";
  } else
  {
    partial += "name IN (";
    for (size_t i = 0; i< target_pkgs.size(); i++) partial += (i == 0 ? "?" : ", ?");
    partial += ")";
  }
  if (!pkg_set.empty())
    partial += " AND pkg_set LIKE ? ";

  std::string query = "SELECT name, version, pkg_set, desc, '{}' as branch"
                      " FROM {}pkgs WHERE "+partial;
  std::string ovr = "SELECT * FROM (";
  ovr += std::vformat(query, std::make_format_args("S", "main."));

  SQLite::Database db_stable {stable_db.string(), SQLite::OPEN_READONLY};

  if (use_unstable)
  {
    try
    {
      db_stable.exec("ATTACH DATABASE '"+unstable_db.string()+"' AS unstable");
    } catch (SQLite::Exception& e)
    {
      std::cerr<<e.what()<<std::endl;
    }
    ovr += " UNION ALL " + std::vformat(query, std::make_format_args("U", "unstable."));
  }
  ovr += ") LIMIT 50;";

  SQLite::Statement stmt {db_stable, ovr};

  size_t n = target_pkgs.size();
  if (enable_exact_matching)
    for (size_t i = 0; i < n; i++)
    {
      stmt.bind(i+1, target_pkgs[i]);
      if (use_unstable) stmt.bind((i+1) + n, target_pkgs[i]);
    }
  else
    for (size_t i = 0; i < n; i++)
    {
      stmt.bind(i+1, target_pkgs[i] + "%");
      if (use_unstable) stmt.bind((i+1)+n, target_pkgs[i] + "%");
    }
  if (!pkg_set.empty())
  {
    stmt.bind(target_pkgs.size()+1, "%"+pkg_set+"%");
    stmt.bind((n+1)+n, "%"+pkg_set+"%");
  }

  std::string connector = "  \u2570\u2500\u2500\u2500 ";// "  ╰─── ";

  try
  {
    size_t count = 0;
    std::cout << "\n";
    while (stmt.executeStep())
    {
      //   0     1      2     3    4
      // name   ver pkg_set desc branch
      std::string branch = stmt.getColumn(4);
      std::string pkg_col;
      branch[0] == 'S' ? pkg_col = GREEN : pkg_col = YELLOW;
      std::cout
        << YELLOW "\u25cf" RESET "  " // ●
        << pkg_col << stmt.getColumn(0) << RESET
        << BLUE " v"<< stmt.getColumn(1) << RESET
        << DIM " (" << stmt.getColumn(2) <<")\n" RESET
        << "   " DIM << connector << RESET
        <<stmt.getColumn(3) << "\n\n";
      count++;
    }
    std::cout<<count<<" RECORDS FOUND." << std::endl;
  } catch (SQLite::Exception& e)
  {
    std::cerr<<"SQLite Exception: "<<e.what()<<std::endl;
    exit(EXIT_FAILURE);
  }
}

void get_pkgs (bool get_unstable)
{
  std::string cmd;
  std::string tmp_dir_string = TMP_DIR.string();
  std::string data_dir_string = DATA_DIR.string();
  if (get_unstable)
  {
    std::string unstable_url = "github:NixOS/nixpkgs/nixos-unstable";
    cmd = std::vformat(
        "nix search {2} ^ --json > {0}/tmp.json && mv {0}/tmp.json {1}/unstable.json",
        std::make_format_args(tmp_dir_string, data_dir_string, unstable_url)
        );
    int retval = std::system (cmd.c_str());

    if (!WIFEXITED(retval) || WEXITSTATUS(retval) != 0)
    {
      std::cerr<<"Error while fetching from nixos-unstable! errcode: "<<retval<<std::endl;
      // exit(retval);
    }
  }
  cmd = std::vformat(
      "nix search nixpkgs ^ --json > {0}/tmp.json && mv {0}/tmp.json {1}/nixpkgs.json",
      std::make_format_args(tmp_dir_string, data_dir_string)
      );

  int retval = std::system (cmd.c_str());

  if (!WIFEXITED(retval) || WEXITSTATUS(retval) != 0)
  {
    std::cerr<<"Error while fetching from nixpkgs! errcode: "<<retval<<std::endl;
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

void parse_pkgs (bool update_unstable)
{
  fs::path db_name {stable_db};
  fs::path json_filepath {DATA_DIR/"nixpkgs.json"};
  if (update_unstable)
  {
    db_name = unstable_db;
    json_filepath  = DATA_DIR/"unstable.json";
  }
  if (fs::exists(db_name)) return;
  update_unstable ? get_pkgs(true) : get_pkgs();

  SQLite::Database db {db_name.string(), SQLite::OPEN_READWRITE|SQLite::OPEN_CREATE};
  db.exec(
      "CREATE TABLE IF NOT EXISTS pkgs (name VARCHAR(255),"
      "version VARCHAR(15), pkg_set TEXT, desc TEXT)"
      );

  sjod::parser parser;
  auto pkgs = simdjson::padded_string::load(json_filepath.generic_string());
  sjod::document pkg_doc = parser.iterate(pkgs);

  SQLite::Statement insert_stmt {
    db,
    "INSERT INTO pkgs (name, version, pkg_set, desc) VALUES (?,?,?,?)"
  };
  SQLite::Transaction txn {db};
  sjod::object pkg_iterable = pkg_doc.get_object();
  for (auto pkg : pkg_iterable)
  {
    std::string pkg_set = std::string {pkg.unescaped_key().value()};
    sjod::object value = pkg.value();
    std::string desc = std::string {value["description"].get_string().value()};
    std::string version = std::string {value["version"].get_string().value()};
    std::string pname = std::string {value["pname"].get_string().value()};
    lower(pname);

    insert_stmt.bind(1, pname);
    insert_stmt.bind(2, version);
    insert_stmt.bind(3, pkg_set);
    insert_stmt.bind(4, desc);
    insert_stmt.exec();
    insert_stmt.reset();
  }
  txn.commit();

  db.exec("CREATE INDEX idx_pname ON pkgs (name)");
  db.exec("CREATE INDEX idx_pkgset ON pkgs (pkg_set)");
  db.exec("ANALYZE");
  db.exec("VACUUM");

  if (update_unstable) fs::remove(DATA_DIR/"nixpkgs.json");// incase of unstable
  fs::remove (json_filepath.c_str());
}
