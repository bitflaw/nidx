#include <JsonBuilder.hpp>
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

#define BOLD_WHITE "\033[1;97m"
#define GREEN "\033[32m"
#define BLUE "\033[1;34m"
#define YELLOW "\033[33m"
#define DIM "\033[90m"
#define RESET "\033[0m"

namespace fs = std::filesystem;
namespace sjod = simdjson::ondemand;

struct Version {
  static constexpr u32 major = 0;
  static constexpr u32 minor = 0;
  static constexpr u32 patch = 0;

  static std::string string() {
    return std::format("{}.{}.{}", major, minor, patch);
  }
};

void usage(std::string_view pname);
fs::path get_data_dir();
void parse_pkgs(bool unstable = false);
void get_pkgs(bool get_unstable = false);
void query_pkgs(std::vector<std::string> &target_pkgs, std::string &pkg_set,
                bool enable_exact_matching = false, bool use_unstable = false);
namespace lsp {
struct Zone {
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
void handle_lsp_init(auto &json_obj);
void handle_lsp_file_open(auto &json_obj);
void handle_lsp_file_change(auto &json_obj);
void handle_lsp_completion(auto &json_obj);
void handle_lsp_hover(auto &json_obj);
void start_lsp();
} // namespace lsp

const fs::path DATA_DIR = get_data_dir();
const fs::path TMP_DIR = fs::temp_directory_path();
const fs::path stable_db = DATA_DIR / "stable.sqlite3.db";
const fs::path unstable_db = DATA_DIR / "unstable.sqlite3.db";

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
  bool use_unstable = false;
  std::vector<std::string> search_pkgs{};
  std::string search_pkg_set;

  while ((opt = getopt(argc, argv, "hvleuU:p:s:")) != -1)
  {
    switch (opt)
    {
    case 'h':
      usage(argv[0]);
      exit(EXIT_SUCCESS);
    case 'v':
      std::cout << argv[0] + 2 << " v" << Version::string() << std::endl;
      exit(EXIT_SUCCESS);
    case 'l': // lsp mode
      lsp::start_lsp();
      exit(EXIT_SUCCESS);
    case 'U': // update the index
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
    case 'u': // search the unstable index
      if (!fs::exists(unstable_db))
      {
        std::cout << "FETCHING FROM UNSTABLE..." << std::endl;
        parse_pkgs(true);
      }
      use_unstable = true;
      break;
    case 'p': // package(s) to search for
      search_pkgs.emplace_back(optarg);
      break;
    case 's': // pkg-set to search for packages
      search_pkg_set = std::string{optarg};
      break;
    case 'e': // exact matching enabled
      enable_exact_matching = true;
      break;
    default:
      usage(argv[0]);
      exit(EXIT_FAILURE);
    }
  }
  for (i32 i = optind; i < argc; i++)
    search_pkgs.emplace_back(argv[i]);
  query_pkgs(search_pkgs, search_pkg_set, enable_exact_matching, use_unstable);

  exit(EXIT_SUCCESS);
}

void usage(std::string_view pname)
{
  std::string usage_str =
      "Usage: {}\n\t [-h help] [-v version] [-l enable lsp-mode] "
      "[-U {{unstable|stable}} update package index]\n\t [-p package(s)] [-s "
      "package set for packages]"
      "[-e enable exact matching]\n\t [-u use unstable to search]";
  std::cout << std::vformat(usage_str, std::make_format_args(pname))
            << std::endl;
}

void query_pkgs(std::vector<std::string> &target_pkgs, std::string &pkg_set,
                bool enable_exact_matching, bool use_unstable
                )
{
  std::string partial = "";
  if (!enable_exact_matching)
  {
    partial += "(";
    for (size_t i = 0; i < target_pkgs.size(); i++)
    {
      partial += "name LIKE ?";
      if (i < target_pkgs.size() - 1)
        partial += " OR ";
    }
    partial += ")";
  } else
  {
    partial += "name IN (";
    for (size_t i = 0; i < target_pkgs.size(); i++)
      partial += (i == 0 ? "?" : ", ?");
    partial += ")";
  }
  if (!pkg_set.empty())
    partial += " AND pkg_set LIKE ? ";

  std::string query = "SELECT name, version, pkg_set, desc, '{}' as branch"
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
    stmt.bind(target_pkgs.size() + 1, "%" + pkg_set + "%");
    stmt.bind((n + 1) + n, "%" + pkg_set + "%");
  }

  std::string connector = "  \u2570\u2500\u2500\u2500 "; // "  ╰─── ";

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
      std::cout << YELLOW "\u25cf" RESET "  " // ●
                << pkg_col << stmt.getColumn(0) << RESET << BLUE " v"
                << stmt.getColumn(1) << RESET << DIM " (" << stmt.getColumn(2)
                << ")\n" RESET << "   " DIM << connector << RESET
                << stmt.getColumn(3) << "\n\n";
      count++;
    }
    std::cout << count << " RECORDS FOUND." << std::endl;
  } catch (SQLite::Exception &e)
  {
    std::cerr << "SQLite Exception: " << e.what() << std::endl;
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

void lower(std::string &str)
{
  for (char &ch : str)
    ch = std::tolower(ch);
}

void parse_pkgs(bool update_unstable)
{
  fs::path db_name{stable_db};
  fs::path json_filepath{DATA_DIR / "nixpkgs.json"};
  if (update_unstable)
  {
    db_name = unstable_db;
    json_filepath = DATA_DIR / "unstable.json";
  }
  if (fs::exists(db_name))
    return;
  update_unstable ? get_pkgs(true) : get_pkgs();

  SQLite::Database db{db_name.string(),
                      SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE};
  db.exec("CREATE TABLE IF NOT EXISTS pkgs (name VARCHAR(255),"
          "version VARCHAR(15), pkg_set TEXT, desc TEXT)");

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
    std::string pkg_set = std::string{pkg.unescaped_key().value()};
    sjod::object value = pkg.value();
    std::string desc = std::string{value["description"].get_string().value()};
    std::string version = std::string{value["version"].get_string().value()};
    std::string pname = std::string{value["pname"].get_string().value()};
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

  if (update_unstable)
    fs::remove(DATA_DIR / "nixpkgs.json"); // incase of unstable
  fs::remove(json_filepath.c_str());
}

namespace lsp
{

// NOTE: LSP spec impl but just for completion for pkgs based on an already
// existent or easily buildable index
// 1. initialize
// WARN: don't need these
// 2. textDocument/didOpen
// 3. textDocument/didChange
// INFO: most important to us, will complete the pkgs.
// 4. textDocument/completion
// 5. textDocument/hover.

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
    throw std::runtime_error(
        "[ERROR] while parsing header: Invalid format for header!"
        );
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
      return;
    else if (obj["method"] == "textDocument/hover")
      handle_lsp_hover(obj);
    else
      continue;
  }
}
// using obj_T = simdjson::simdjson_result<simdjson::fallback::ondemand::object>;

auto output = [] (JsonObject response_obj, std::string response)
{
  builder(JsonValue {response_obj}, response);
  std::cout
      << "Content-Length:" << response.size() << "\r\n\r\n" << response
      << std::flush;
};

void handle_lsp_init(auto &json_obj)
{
  JsonObject response_obj {};
  response_obj.insert({"jsonrpc", JsonValue{"2.0"}});
  auto id = json_obj["id"];
  JsonValue id_value{};
  try
  {
    id_value = JsonValue{id.get_int64()};
  } catch (const simdjson::simdjson_error&)
  {
    id_value = JsonValue{std::string{id.get_string().value()}};
  }
  response_obj.insert({"id", id_value});

  JsonObject svr_info {
      {"name", JsonValue{"nidx"}},
      {"version", JsonValue{"0.1.0"}},
  };
  // JsonObject completion_attrs {
  //   {"resolveProvider", JsonValue {false}}
  // };
  JsonObject capabilities {
      {"textDocumentSync", JsonValue{2}}, // incremental sync
      // {"completionProvider", JsonValue {completion_attrs}}
      {"hoverProvider", JsonValue{true}},
  };

  JsonObject result {};
  result.insert({"capabilities", JsonValue{capabilities}});
  result.insert({"serverInfo", JsonValue{svr_info}});

  response_obj.insert({"result", JsonValue{result}});
  std::string stringified_response{};
  std::invoke(output, response_obj, stringified_response);
}

void handle_lsp_file_open(auto &json_obj)
{
  auto file_params = json_obj["params"]["textDocument"].get_object();
  std::string_view file_uri = file_params["uri"].get_string();
  std::string_view src = file_params["text"].get_string();
  src_lines = split(std::string{src}, "\n");
  std::vector<std::string> keywords{"buildInputs", "nativeBuildInputs",
                                    "packages"};
  scan_for_keywords(std::string{file_uri}, keywords);
}

void handle_lsp_file_change(auto &json_obj)
{
  auto file_params = json_obj["params"]["textDocument"].get_object();
  std::string_view file_uri = file_params["uri"].get_string();
  auto file_changes = json_obj["params"]["contentChanges"].get_array();
  for (size_t i {0}; i < file_changes.count_elements(); i++)
  {
    auto change = file_changes.at(i);
    if (
        std::string_view text = change.at(i).find_field("text").value();
        !text.empty()
        )
    {
      src_lines.clear();
      src_lines = split(std::string {text}, "\n");
      std::vector<std::string> keywords{"buildInputs", "nativeBuildInputs",
                                    "packages"};
      scan_for_keywords(std::string{file_uri}, keywords);
      continue;
    }
    auto range_obj = change["range"].get_object();
    FileLoc range {
      .line_range = {
        .start = range_obj["start"]["line"].get_int32(),
        .end = range_obj["end"]["line"].get_int32(),
      },
      .char_range = {
        .start = range_obj["start"]["character"].get_int32(),
        .end = range_obj["end"]["character"].get_int32(),
      }
    };
    try
    {
      std::vector<Zone> kw_zones = file_kw_zones.at(std::string {file_uri});
      for (Zone& zone : kw_zones)
      {
        Zone line_range = range.line_range;
        bool is_overlapping = (line_range.start <= zone.end) && (line_range.end >= zone.start);
        if (is_overlapping)
        {
          u32 range_length = change["rangeLength"].get_uint32();
          std::string_view text = change["text"].get_string();
          perform_changes(range, range_length, text);
        }
      }
    } catch (const std::out_of_range&)
    {
      continue;
    }
  }
}

// void handle_lsp_completion (auto &json_obj)
// {}

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

std::optional<Zone> zone_from_char_pos (std::string_view file_uri, auto &hover_loc)
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
  return search_for_bounds(character, src_line);
}

void handle_lsp_hover(auto &json_obj)
{
  JsonObject response_obj {};
  std::string stringified {};
  response_obj.insert({"jsonrpc", JsonValue {"2.0"}});
  auto id = json_obj["id"];
  JsonValue id_value{};
  try
  {
    id_value = JsonValue{id.get_int64()};
  } catch (const simdjson::simdjson_error&)
  {
    id_value = JsonValue{std::string{id.get_string().value()}};
  }
  response_obj.insert({"id", id_value});
  std::string_view file_uri= json_obj["params"]["textDocument"]["uri"].get_string();
  auto hover_loc = json_obj["params"]["position"].get_object();
  std::optional<Zone> location = zone_from_char_pos (file_uri, hover_loc);
  if (!location.has_value())
  {
    // send null response...hover position is not in any of our zones
    response_obj.insert({ "result", JsonValue {std::monostate {}} });
    std::invoke(output, response_obj, stringified);
    return;
  }

  i32 line = hover_loc["line"].get_int32();
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

  response_obj.insert_or_assign("result", JsonValue {std::monostate {}});
  SQLite::Database db {stable_db.string(), SQLite::OPEN_READWRITE};
  SQLite::Statement query {db, "SELECT * FROM pkgs WHERE name = ?"};
  query.bind(1, trimmed_lookup_value);
  if (query.executeStep())
  {
    //   0     1      2     3
    // name   ver pkg_set desc
    std::string name =    query.getColumn(0);
    std::string version = query.getColumn(1);
    std::string pkg_set = query.getColumn(2);
    std::string desc =    query.getColumn(3);
    std::string md = "## {} [**v{}**]\n**({})**\n\n{}";
    JsonObject result {};
    JsonObject contents {
      {"kind", JsonValue {"markdown"}},
      {"value", JsonValue {std::vformat(md, std::make_format_args(name, version, pkg_set, desc))}}
    };

    result.insert({"contents", JsonValue {contents}});
    response_obj.insert_or_assign("result", JsonValue {result});
  }

  stringified.clear();
  std::invoke(output, response_obj, stringified);
}

} // namespace lsp
