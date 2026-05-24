#include <JsonSerializer.hpp>
#include <SQLiteCpp/SQLiteCpp.h>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <getopt.h>
#include <simdjson.h>
#include <stack>
#include <sys/wait.h>
#include <unistd.h>
#include <functional>
// #include <fstream>

#define BOLD_WHITE "\033[1;97m"
#define GREEN "\033[32m"
#define BLUE "\033[1;34m"
#define YELLOW "\033[33m"
#define DIM "\033[90m"
#define RESET "\033[0m"

namespace fs = std::filesystem;
namespace sjod = simdjson::ondemand;

struct Version
{
  static constexpr u32 major = 0;
  static constexpr u32 minor = 0;
  static constexpr u32 patch = 0;

  static std::string string()
  {
    return std::format("{}.{}.{}", major, minor, patch);
  }
};

void usage(std::string_view pname);
fs::path get_data_dir();
void parse_pkgs(bool unstable = false);
void get_pkgs(bool get_unstable = false);
void query_pkgs(std::vector<std::string> &target_pkgs, std::string &pkg_set,
                bool enable_exact_matching = false, bool use_unstable = false,
                bool case_sensitive_search = false
                );
namespace lsp
{
struct Zone
{
  i32 start {-1}, end {-1};
};

std::vector<std::string> src_lines{};
std::unordered_map<std::string, std::vector<Zone>> file_kw_zones{};

struct FileLoc
{
  Zone line_range {};
  Zone char_range {};
};


std::vector<std::string> split(std::string const &str, std::string const &delim,
                               std::size_t times = INT_MAX);
i32 parse_header(std::string header);

void start_lsp();
void handle_lsp_init(auto &json_obj);
void handle_lsp_file_open(auto &json_obj);
void handle_lsp_file_change(auto &json_obj);
void handle_lsp_completion(auto &json_obj);
void handle_lsp_hover(auto &json_obj);

} // namespace lsp

const fs::path DATA_DIR = get_data_dir();
const fs::path TMP_DIR = fs::temp_directory_path();
const fs::path stable_db = DATA_DIR / "stable.sqlite3.db";
const fs::path unstable_db = DATA_DIR / "unstable.sqlite3.db";
const fs::path lsp_log_file = DATA_DIR / "lsp.log";

i32 main(i32 argc, char *argv[])
{
  if (argc == 1)
  {
    usage(argv[0]);
    return EXIT_FAILURE;
  }

  parse_pkgs();
  i32 opt;

  bool enable_exact_matching = false;
  bool case_sensitive_search = false;
  bool use_unstable = false;
  std::vector<std::string> search_pkgs {};
  std::string search_pkg_set {};

  while ((opt = getopt(argc, argv, "hvleuSU:p:s:")) != -1)
  {
    switch (opt)
    {
      case 'h':
        usage(argv[0]);
        exit(EXIT_SUCCESS);
      case 'v':
        std::cout << argv[0] + 2 << " v" << Version::string() << std::endl;
        exit(EXIT_SUCCESS);
      case 'l':
        lsp::start_lsp();
        exit(EXIT_SUCCESS);
      case 'U':
        if (std::string{optarg} == "unstable")
        {
          fs::remove(unstable_db);
          parse_pkgs(true);
          exit(EXIT_SUCCESS);
        }
        else if (std::string{optarg} == "stable")
        {
          fs::remove(stable_db);
          parse_pkgs(false);
          exit(EXIT_SUCCESS);
        }
        else
        {
          usage(argv[0]);
          exit(EXIT_FAILURE);
        }
      case 'u':
        if (!fs::exists(unstable_db))
        {
          std::cout << "[INFO] FETCHING FROM UNSTABLE...";
          parse_pkgs(true);
          std::cout << "[INFO] OK" << std::endl;
        }
        use_unstable = true;
        break;
      case 'p':
        search_pkgs.emplace_back(optarg);
        break;
      case 's':
        search_pkg_set = std::string{optarg};
        break;
      case 'e':
        enable_exact_matching = true;
        break;
      case 'S':
        case_sensitive_search = true;
        break;
      default:
        usage(argv[0]);
        exit(EXIT_FAILURE);
    }
  }
  for (i32 i = optind; i < argc; i++)
    search_pkgs.emplace_back(argv[i]);
  query_pkgs(search_pkgs, search_pkg_set, enable_exact_matching, use_unstable, case_sensitive_search);

  exit(EXIT_SUCCESS);
}

void usage(std::string_view pname)
{
  std::string usage_str =
      "Usage: {}\n\t [-h help] [-v version] [-l enable lsp-mode] [-p package(s)]\n\t"
      "[-s package set ] [-S case-sensitive search][-e enable exact matching]\n\t"
      "[-U {{unstable|stable}} update package index] [-u use unstable to search]\n";

  std::cout << std::vformat(usage_str, std::make_format_args(pname))
            << std::endl;
}

void query_pkgs(std::vector<std::string> &target_pkgs, std::string &pkg_set,
                bool enable_exact_matching, bool use_unstable, bool case_sensitive_search
                )
{
  std::string name_column_to_use {"name"};
  if (case_sensitive_search) name_column_to_use = "csname";

  std::string partial {};
  if (enable_exact_matching)
  {
    partial += name_column_to_use + " IN (";
    for (size_t i = 0; i < target_pkgs.size(); i++)
      partial += (i == 0 ? "?" : ", ?");
  } else
  {
    partial += "(";
    for (size_t i = 0; i < target_pkgs.size(); i++)
    {
      partial += name_column_to_use + " LIKE ?";
      if (i < target_pkgs.size() - 1)
        partial += " OR ";
    }
  }
  partial += ")";
  if (!pkg_set.empty()) partial += " AND set_prefix LIKE ? ";

  std::string query = "SELECT csname, version, set_prefix, desc, '{}' as branch"
                      " FROM {}pkgs WHERE " +
                      partial;
  std::string ovr = "SELECT * FROM (";
  ovr += std::vformat(query, std::make_format_args("S", "main."));

  SQLite::Database db_stable{stable_db.string(), SQLite::OPEN_READONLY};

  if (use_unstable)
  {
    try
    {
      db_stable.exec("ATTACH DATABASE '" + unstable_db.string() +
                     "' AS unstable");
    } catch (SQLite::Exception &e)
    {
      std::cerr << e.what() << std::endl;
    }
    ovr += " UNION ALL " +
           std::vformat(query, std::make_format_args("U", "unstable."));
  }
  ovr += ") LIMIT 50;";

  SQLite::Statement stmt{db_stable, ovr};

  size_t n = target_pkgs.size();
  if (enable_exact_matching)
    for (size_t i = 0; i < n; i++)
    {
      stmt.bind(i + 1, target_pkgs[i]);
      if (use_unstable)
        stmt.bind((i + 1) + n, target_pkgs[i]);
    }
  else
    for (size_t i = 0; i < n; i++)
    {
      stmt.bind(i + 1, target_pkgs[i] + "%");
      if (use_unstable)
        stmt.bind((i + 1) + n, target_pkgs[i] + "%");
    }
  if (!pkg_set.empty())
  {
    stmt.bind(target_pkgs.size() + 1, pkg_set + "%");
    stmt.bind((n + 1) + n, pkg_set + "%");
  }

  std::string connector = "  \u2570\u2500\u2500\u2500 "; // "  ╰─── ";

  try
  {
    size_t count = 0;
    std::cout << "\n";
    while (stmt.executeStep())
    {
      //   0      1       2      3      4
      // csname  ver  set_pref  desc  branch
      std::string branch = stmt.getColumn(4);
      std::string pkg_col;
      branch[0] == 'S' ? pkg_col = GREEN : pkg_col = YELLOW;
      std::string pkg_set = stmt.getColumn(2);
      std::string pkg_set_out;
      pkg_set.empty() ?
        pkg_set_out = "" :
        pkg_set_out = DIM " (" + pkg_set + ")" RESET;
      std::cout << pkg_col + "\u25cf" RESET "  " // ●
                << pkg_col << stmt.getColumn(0) << RESET << BLUE " v"
                << stmt.getColumn(1) << RESET << pkg_set_out
                << "\n   " DIM << connector << RESET
                << stmt.getColumn(3) << "\n\n";
      count++;
    }
    std::string out {count == 1 ? " RECORD" : " RECORDS"};
    std::cout << count << out + " FOUND." << std::endl;
  } catch (SQLite::Exception &e)
  {
    std::cerr << e.what() << std::endl;
    exit(EXIT_FAILURE);
  }
}

void get_pkgs(bool get_unstable)
{
  std::string cmd;
  std::string tmp_dir_string = TMP_DIR.string();
  std::string data_dir_string = DATA_DIR.string();
  if (get_unstable)
  {
    std::string unstable_url = "github:NixOS/nixpkgs/nixos-unstable";
    cmd = std::vformat(
        "nix search {2} ^ --json > {0}/tmp.json && mv {0}/tmp.json "
        "{1}/unstable.json",
        std::make_format_args(tmp_dir_string, data_dir_string, unstable_url));
    i32 retval = std::system(cmd.c_str());

    if (!WIFEXITED(retval) || WEXITSTATUS(retval) != 0)
    {
      std::cerr << "Error while fetching from nixos-unstable! errcode: "
                << retval << std::endl;
      // exit(retval);
    }
  }
  cmd = std::vformat("nix search nixpkgs ^ --json > {0}/tmp.json && mv "
                     "{0}/tmp.json {1}/nixpkgs.json",
                     std::make_format_args(tmp_dir_string, data_dir_string));

  i32 retval = std::system(cmd.c_str());

  if (!WIFEXITED(retval) || WEXITSTATUS(retval) != 0)
  {
    std::cerr << "Error while fetching from nixpkgs! errcode: " << retval
              << std::endl;
    exit(retval);
  }
}

fs::path get_data_dir()
{
  fs::path data_dir_path{};
  if (const char *xdg = std::getenv("XDG_DATA_HOME"))
  {
    data_dir_path = std::filesystem::path(xdg) / ".nidx";
    if (!fs::exists(data_dir_path))
      fs::create_directory(data_dir_path);
    return data_dir_path;
  }

  const char *home = std::getenv("HOME");
  if (!home)
    throw std::runtime_error("HOME variable not set");

  data_dir_path = std::filesystem::path(home) / ".local" / "share" / ".nidx";
  if (!fs::exists(data_dir_path))
    if (!fs::create_directory(data_dir_path))
      throw std::runtime_error("Failed to create the data directory!");
  return data_dir_path;
}

void lower(std::string &str) { for (char &ch : str) ch = std::tolower(ch); }

void parse_pkgs(bool update_unstable)
{
  fs::path db_name{stable_db};
  fs::path json_filepath{DATA_DIR / "nixpkgs.json"};
  if (update_unstable)
  {
    db_name = unstable_db;
    json_filepath = DATA_DIR / "unstable.json";
  }
  if (fs::exists(db_name)) return;
  update_unstable ? get_pkgs(true) : get_pkgs();

  SQLite::Database db{db_name.string(),
                      SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE};
  db.exec("CREATE TABLE IF NOT EXISTS pkgs (name VARCHAR(255), csname VARCHAR(255),"
          "version VARCHAR(15), set_prefix TEXT, desc TEXT)");

  sjod::parser parser;
  auto pkgs = simdjson::padded_string::load(json_filepath.generic_string());
  sjod::document pkg_doc = parser.iterate(pkgs);

  SQLite::Statement insert_stmt {
      db,
      "INSERT INTO pkgs (name, csname, version, set_prefix, desc) VALUES (?,?,?,?,?)"
  };
  SQLite::Transaction txn {db};
  sjod::object pkg_iterable = pkg_doc.get_object();
  std::string pkg_set_prefix = "legacyPackages.x86_64-linux.";
  for (auto pkg : pkg_iterable)
  {
    std::string pkg_set = std::string{pkg.unescaped_key().value()}.substr(pkg_set_prefix.size());;
    std::string name {};
    size_t idx = pkg_set.find_last_of('.');
    if (idx != std::string::npos)
    {
      name = pkg_set.substr(idx+1);
      insert_stmt.bind(4, pkg_set.substr(0, idx));
    }
    else
    {
      name = pkg_set;
      insert_stmt.bind(4);
    }

    sjod::object value = pkg.value();
    std::string desc = std::string {value["description"].get_string().value()};
    std::string version = std::string {value["version"].get_string().value()};

    insert_stmt.bind(2, name); // case-sensitive name
    lower(name);
    insert_stmt.bind(1, name); // case-insensitive name
    insert_stmt.bind(3, version);
    insert_stmt.bind(5, desc);
    insert_stmt.exec();
    insert_stmt.reset();
  }
  txn.commit();

  db.exec("CREATE INDEX idx_name ON pkgs (name)");
  db.exec("CREATE INDEX idx_set_pref ON pkgs (set_prefix)");
  db.exec("ANALYZE");
  db.exec("VACUUM");

  if (update_unstable)
    fs::remove(DATA_DIR / "nixpkgs.json"); // incase of unstable
  fs::remove(json_filepath.c_str());
}

namespace lsp
{

std::vector<std::string> split(std::string const &str, std::string const &delim,
                               std::size_t times)
{
  std::vector<std::string> vtok;
  std::size_t start{0};
  auto end = str.find(delim);

  while ((times-- > 0) && (end != std::string::npos))
  {
    vtok.emplace_back(str.substr(start, end - start));
    start = end + delim.length();
    end = str.find(delim, start);
  }
  vtok.emplace_back(str.substr(start));

  return vtok;
}

i32 parse_header(std::string header)
{
  std::string::size_type idx = header.find(':');
  if (idx == std::string::npos)
    std::cerr<< "[ERROR] while parsing header: Invalid format for header!\n"
      << std::flush;
  idx++;

  return std::stoi(header.substr(idx));
}

i32 look_ahead(size_t idx)
{
  i32 closing_brace_idx{-1};
  std::stack<char> brace_match{};
  for (size_t i{idx}; i < src_lines.size(); i++)
  {
    size_t opening_found = src_lines[i].find("[");
    size_t closing_found = src_lines[i].find("]");
    if (opening_found != std::string::npos)
      brace_match.push('[');
    if (closing_found != std::string::npos)
    {
      if (brace_match.empty())
        return closing_brace_idx;
      brace_match.pop();
      closing_brace_idx = i;
    }
    if (brace_match.empty())
      return closing_brace_idx;
  }
  return closing_brace_idx;
}

void scan_for_keywords(std::string file_uri, std::vector<std::string> &keywords)
{
  std::vector<Zone> zones {};
  for (size_t i{0}; i < src_lines.size(); i++)
  {
    std::erase_if(keywords, [&](std::string &kw) {
      bool erase_kw = false;
      size_t start_idx = src_lines[i].find(kw);
      if (start_idx != std::string::npos) {
        i32 stop_idx = look_ahead(i);
        zones.emplace_back(Zone{static_cast<i32>(i), stop_idx});
        erase_kw = true;
      }
      return erase_kw;
    });
  }
  file_kw_zones.insert_or_assign(std::string {file_uri}, zones);
}

void perform_changes (FileLoc range, u32 range_length, std::string_view text)
{
  std::vector<std::string> line_changes = split(std::string {text}, "\n");

  if (line_changes.size() > 1)
  {
    // top line for the range
    std::string line_to_edit = src_lines[range.line_range.start];
    std::string half_to_keep = line_to_edit.substr(0, range.char_range.start);
    line_to_edit.erase(line_to_edit.begin() + range.char_range.start, line_to_edit.end());
    line_to_edit.append(line_changes.front());
    src_lines[range.line_range.start] = line_to_edit;

    // bottom line for the range
    line_to_edit.clear();
    line_to_edit = src_lines[range.line_range.end];
    line_to_edit.erase(line_to_edit.begin(), line_to_edit.begin() + range.char_range.end);
    src_lines[range.line_range.end] = line_changes.back() + line_to_edit;

    // perform insert for the middle lines
    src_lines.erase (
      src_lines.begin() + range.line_range.start,
      src_lines.begin() + range.line_range.end
    );
    src_lines.insert (
      src_lines.begin () + range.line_range.start,
      line_changes.begin () + 1,
      line_changes.end () - 2
    );
    return;
  }

  std::string line_to_edit = src_lines[range.line_range.start];
  std::string first_half = line_to_edit.substr(0, range.char_range.start);
  std::string last_half = line_to_edit.substr(range.char_range.start + range_length);
  src_lines[range.line_range.start] = first_half + std::string {text} + last_half;
}

void start_lsp()
{
  std::string header {};
  while (std::getline(std::cin, header))
  {
    if (header.empty()) continue;
    i32 len = parse_header(header);
    header.clear();
    std::string empty;
    std::getline(std::cin, empty);

    std::vector<char> buffer(len);
    std::cin.read(buffer.data(), len);
    std::string_view content{buffer.data(), (size_t)len};
    sjod::parser parser;
    simdjson::padded_string p{content};
    auto doc = parser.iterate(p);
    auto obj = doc.get_object();
    if (obj["method"] == "initialize")
      handle_lsp_init(obj);
    else if (obj["method"] == "textDocument/didOpen")
      handle_lsp_file_open(obj);
    else if (obj["method"] == "textDocument/didChange")
      handle_lsp_file_change(obj);
    else if (obj["method"] == "textDocument/completion")
      handle_lsp_completion(obj);
    else if (obj["method"] == "textDocument/hover")
      handle_lsp_hover(obj);
    else
      continue;
  }
}
using obj_T = simdjson::simdjson_result<simdjson::fallback::ondemand::object>;

auto output = [] (JsonObject response_obj, std::string response)
{
  serialize(JsonValue {response_obj}, response);
  std::cout
      << "Content-Length:" << response.size() << "\r\n\r\n" << response
      << std::flush;
};

auto send_null_response = [] (std::string id)
{
  JsonObject null_response
  {
    {"id", JsonValue {id}},
    {"jsonrpc", JsonValue {"2.0"}},
    {"result", JsonValue {std::monostate {}}}
  };
  std::string response {};
  output(null_response, response);
};

void handle_lsp_init(auto &json_obj)
{
  auto id = json_obj["id"];
  JsonValue id_value{};
  try
  {
    id_value = JsonValue{std::to_string(id.get_int64())};
  } catch (const simdjson::simdjson_error&)
  {
    id_value = JsonValue{std::string{id.get_string().value()}};
  }
  try
  {
    JsonObject response_obj {};
    response_obj.insert({"jsonrpc", JsonValue{"2.0"}});
    response_obj.insert({"id", id_value});

    JsonObject svr_info {
        {"name", JsonValue{"nidx"}},
        {"version", JsonValue{"0.1.0"}},
    };
    JsonObject capabilities {
        {"textDocumentSync", JsonValue{1}}, // full sync
        {"completionProvider", JsonValue {
                                           JsonObject{
                                             {"triggerCharacters", JsonValue{JsonArray {JsonValue{"."}}}},
                                             {"resolveProvider", JsonValue {false}}
                                           }
                                         }
        },
        {"hoverProvider", JsonValue{true}},
    };

    JsonObject result {};
    result.insert({"capabilities", JsonValue{capabilities}});
    result.insert({"serverInfo", JsonValue{svr_info}});

    response_obj.insert({"result", JsonValue{result}});
    std::string stringified_response{};
    std::invoke(output, response_obj, stringified_response);
  } catch (const std::exception& e)
  {
    std::cerr << "[INIT ERROR] " <<e.what()<<std::endl;
    send_null_response(std::get<std::string>(id_value.value));
  }
}

void handle_lsp_file_open(auto &json_obj)
{
  try
  {
    auto file_params = json_obj["params"]["textDocument"].get_object();
    std::string_view file_uri = file_params["uri"].get_string();
    std::string_view src = file_params["text"].get_string();
    src_lines = split(std::string{src}, "\n");
    std::vector<std::string> keywords{"buildInputs", "nativeBuildInputs",
                                      "packages"};
    scan_for_keywords(std::string{file_uri}, keywords);
  } catch (const std::exception& e)
  {
    std::cerr << "[OPEN ERROR] " <<e.what()<<std::endl;
  }
}

void handle_lsp_file_change(auto &json_obj)
{
  try
  {
    auto file_params = json_obj["params"]["textDocument"].get_object();
    std::string_view file_uri = file_params["uri"].get_string();
    auto file_changes = json_obj["params"]["contentChanges"].get_array();

    std::stringstream full_text {std::string {file_changes.at(0).find_field("text").get_string().value()}};
    src_lines.clear();
    std::string line;
    while (std::getline(full_text, line)) src_lines.emplace_back(line);

    std::vector<std::string> keywords{"buildInputs", "nativeBuildInputs", "packages"};
    scan_for_keywords(std::string{file_uri}, keywords);
  } catch (const std::exception& e)
  {
    std::cerr << "[SYNC ERROR] " <<e.what()<<std::endl;
  }
}


i32 search_right (i32 pos, std::string_view search_line)
{
  for (i32 i {pos}; i < (i32) search_line.size(); i++)
    if (search_line[i] == ' ' || search_line[i] == '\n')
      return i-1;
  return pos;
}

i32 search_left (i32 pos, std::string_view search_line)
{
  for (i32 i {pos}; i > 0; i--)
    if (search_line[i] == ' ' || search_line[i] == '\t')
      return i+1;
  return pos;
}

Zone search_for_bounds (i32 search_start_pos, std::string_view search_line)
{
  return Zone {
    .start = search_left(search_start_pos, search_line),
    .end = search_right(search_start_pos, search_line)
  };
}

std::optional<Zone> zone_from_char_pos (
    std::string_view file_uri,
    auto &hover_loc,
    bool search_both=true,
    bool left_search=true
    )
{
  i32 line = hover_loc["line"].get_int32();
  i32 character = hover_loc["character"].get_int32();
  bool is_in_kw_zone = false;
  try
  {
    std::vector<Zone> kw_zones = file_kw_zones.at(std::string {file_uri});
    for (Zone& kw_zone : kw_zones)
    {
      if (line >= kw_zone.start && line <= kw_zone.end)
      {
        is_in_kw_zone = true;
        break;
      }
    }
  } catch (const std::out_of_range&)
  {
    return std::nullopt;
  }
  if (!is_in_kw_zone) return std::nullopt;

  std::string src_line = src_lines[line];

  if (search_both)
    return search_for_bounds(character, src_line);
  else if (left_search)
    return Zone {
     .start = search_left (character, src_line),
       .end = character
    };
  else
   return Zone {
     .start = character,
       .end = search_right(character, src_line)
   };
}

void handle_lsp_completion (auto &json_obj)
{
  auto id = json_obj["id"];
  JsonValue id_value {};
  try
  {
    id_value = JsonValue{std::to_string(id.get_int64())};
  } catch (const simdjson::simdjson_error&)
  {
    id_value = JsonValue{std::string{id.get_string().value()}};
  }
  try
  {
    JsonObject response_obj {};
    std::string stringified {};
    response_obj.insert({"jsonrpc", JsonValue {"2.0"}});
    response_obj.insert({"id", id_value});

    auto params = json_obj["params"].get_object();
    std::string_view file_uri = params["textDocument"]["uri"].get_string();
    auto cmp_pos = params["position"].get_object();
    std::optional<Zone> location = zone_from_char_pos(file_uri, cmp_pos, false, true);
    if (!location.has_value())
    {
      // send null response...cmp position is not in any of our zones
      send_null_response(std::get<std::string>(id_value.value));
      return;
    }

    i32 line = cmp_pos["line"].get_int32();
    std::string src_line = src_lines[line];
    if (src_line.empty())
    {
      response_obj.insert({ "result", JsonValue {std::monostate {}} });
      std::invoke(output, response_obj, stringified);
      return;
    }

    std::string lookup = src_line.substr(location->start, location->end);
    std::string trimmed_lookup_value;
    for (u32 i = 0; i < lookup.size(); i++)
    {
      if (lookup[i] == '\t' || lookup[i] == ' ') continue;
      trimmed_lookup_value = lookup.substr(i);
      break;
    }
    std::string sql {"SELECT csname, version, set_prefix, desc FROM pkgs WHERE csname LIKE ?"};
    std::string pkg_set {};
    size_t idx = trimmed_lookup_value.find('.');
    if (idx != std::string::npos)
    {
      pkg_set = trimmed_lookup_value.substr(0, idx);
      sql.append(" AND set_prefix LIKE ?");
      trimmed_lookup_value = trimmed_lookup_value.substr(idx+1);
    }
    sql.append(" LIMIT 30");

    response_obj.insert_or_assign("result", JsonValue {std::monostate {}});
    SQLite::Database db {stable_db.string(), SQLite::OPEN_READWRITE};
    SQLite::Statement query {db, sql};
    query.bind(1, trimmed_lookup_value + '%');
    if (!pkg_set.empty()) query.bind(2, pkg_set + '%');
    JsonArray items {};
    while (query.executeStep())
    {
      //   0     1       2        3
      // csname ver  set_prefix  desc
      std::string name =    query.getColumn(0);
      std::string version = query.getColumn(1);
      std::string set_pref = query.getColumn(2);
      std::string desc =    query.getColumn(3);
      std::string full_name {};
      if (!set_pref.empty()) full_name = set_pref + "." + name;
      else full_name = name;

      JsonObject item {
        {"label", JsonValue {full_name}},
        {"kind", JsonValue {3}},
        {"detail", JsonValue {" v" + version}},
        {"documentation", JsonValue {desc}},
      };
      items.emplace_back (item);
    }

    JsonObject result {};
    result.insert({"isIncomplete", JsonValue {true}});
    result.insert({"items", JsonValue {items}});
    response_obj.insert_or_assign("result", JsonValue {result});

    stringified.clear();
    std::invoke(output, response_obj, stringified);
  } catch (const std::exception& e)
  {
    std::cerr << "[CMP ERROR] " <<e.what()<<std::endl;
    send_null_response(std::get<std::string>(id_value.value));
  }
}

auto rtrim = [] (std::string str)
{
  auto last_non_space = str.find_last_not_of(' ');
  if (last_non_space == std::string::npos) return std::string("");
  return str.substr(0, last_non_space+1);
};

void handle_lsp_hover(auto &json_obj)
{
  auto id = json_obj["id"];
  JsonValue id_value{};
  try
  {
    id_value = JsonValue{id.get_int64()};
  } catch (const simdjson::simdjson_error&)
  {
    id_value = JsonValue{std::string{id.get_string().value()}};
  }
  try
  {
    JsonObject response_obj {};
    std::string stringified {};
    response_obj.insert({"jsonrpc", JsonValue {"2.0"}});
    response_obj.insert({"id", id_value});

    std::string_view file_uri = json_obj["params"]["textDocument"]["uri"].get_string();
    auto hover_loc = json_obj["params"]["position"].get_object();
    std::optional<Zone> location = zone_from_char_pos (file_uri, hover_loc);
    if (!location.has_value())
    {
      // send null response...hover position is not in any of our zones
      send_null_response(std::get<std::string>(id_value.value));
      return;
    }

    i32 line = hover_loc["line"].get_int32();
    std::string src_line = src_lines[line];
    if (src_line.empty())
    {
      send_null_response(std::get<std::string>(id_value.value));
      return;
    }

    std::string lookup = src_line.substr(location->start, location->end);
    std::string trimmed_lookup_value;
    for (u32 i = 0; i < lookup.size(); i++)
    {
      if (lookup[i] == '\t' || lookup[i] == ' ') continue;
      trimmed_lookup_value = lookup.substr(i);
      break;
    }
    trimmed_lookup_value = rtrim(trimmed_lookup_value);

    size_t idx = trimmed_lookup_value.rfind('.');
    std::string pkg_set_prefix {};
    if (idx != std::string::npos)
    {
      pkg_set_prefix = trimmed_lookup_value.substr(0, idx);
      trimmed_lookup_value = trimmed_lookup_value.substr(idx+1);
    }

    response_obj.insert_or_assign("result", JsonValue {std::monostate {}});
    SQLite::Database db {stable_db.string(), SQLite::OPEN_READWRITE};
    std::string sql = "SELECT csname, version, set_prefix, desc FROM pkgs"
                      " WHERE csname = ?";
    if (pkg_set_prefix.empty()) sql += " AND set_prefix IS NULL";
    else sql += " AND set_prefix = ?";

    SQLite::Statement query {db, sql};
    query.bind(1, trimmed_lookup_value);
    if (!pkg_set_prefix.empty()) query.bind(2, pkg_set_prefix);

    if (query.executeStep())
    {
      //    1      2       3        4
      // csname  ver  set_prefix  desc
      std::string name =    query.getColumn(0);
      std::string version = query.getColumn(1);
      std::string set_pref = query.getColumn(2);
      std::string desc =    query.getColumn(3);
      std::string md = "{} [**v{}**]\n**{}**\n{}";
      JsonObject result {};
      if (!set_pref.empty()) set_pref = "(" + set_pref + ")";
      else set_pref = "(none)";
      JsonObject contents {
        {"kind", JsonValue {"markdown"}},
        {"value", JsonValue {std::vformat(md, std::make_format_args(name, version, set_pref, desc))}}
      };

      result.insert({"contents", JsonValue {contents}});
      response_obj.insert_or_assign("result", JsonValue {result});
    }

    stringified.clear();
    std::invoke(output, response_obj, stringified);
  } catch (const std::exception& e)
  {
    std::cerr << "[HOVER ERROR] " <<e.what()<<std::endl;
    send_null_response(std::get<std::string>(id_value.value));
  }
}

} // namespace lsp
