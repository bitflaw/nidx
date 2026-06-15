#include <cstdlib>
#include <filesystem>
#include <format>
#include <functional>
#include <getopt.h>
#include <JsonSerializer.hpp>
#include <nix_grammar.hpp>
#include <string_view>
#include <sys/wait.h>
#include <simdjson.h>
#include <SQLiteCpp/SQLiteCpp.h>
#include <tree_sitter/api.h>
#include <unistd.h>
#include <unordered_set>

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

struct QueryOpts
{
  bool enable_exact_matching {false};
  bool case_sensitive_search {false};
  bool use_unstable {false};
};

void usage(std::string_view pname);
fs::path get_data_dir();
void parse_pkgs(bool unstable = false);
void get_pkgs(bool get_unstable = false);
void query_pkgs(std::vector<std::string> &target_pkgs, std::string &pkg_set, QueryOpts);

namespace lsp
{

struct Zone { u32 start {}, end {}; };
enum ZoneType { WITH_LIST, STD_LIST };
using list_zone_T = std::vector<std::pair<ZoneType, Zone>>;

TSParser* init_parser ();
TSParser *parser = init_parser();

struct FileContext
{
  std::string src_code {};
  TSTree* CST = nullptr;
  list_zone_T interest_zones {};
  std::vector <u64> line_offsets {};

  FileContext () = default;
  ~FileContext ()
  {
    if (CST != nullptr) ts_tree_delete(CST);
  }
  FileContext(const FileContext&) = delete; 
  FileContext& operator=(const FileContext&) = delete;
  FileContext(FileContext&& other) noexcept
  {
    src_code = std::move (other.src_code);
    CST = other.CST;
    other.CST = nullptr;
    interest_zones = std::move(other.interest_zones);
    line_offsets = std::move(other.line_offsets);
  }
  FileContext& operator= (FileContext&& other) noexcept
  {
    if (this != &other)
    {
      if (CST != nullptr) ts_tree_delete(CST);
      CST = other.CST;
      src_code = std::move (other.src_code);
      interest_zones = std::move(other.interest_zones);
      line_offsets = std::move(other.line_offsets);
      other.CST = nullptr;
    }
    return *this;
  }
};
std::unordered_map<std::string, FileContext> file_ctxs {};

std::unordered_set<std::string> zone_keywords {
  "buildInputs", "nativeBuildInputs", "packages", "home.packages",
    "systemPackages", "environment.systemPackages"
};

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

i32 main(i32 argc, char *argv[])
{
  if (argc == 1)
  {
    usage(argv[0]);
    return EXIT_FAILURE;
  }

  parse_pkgs();
  i32 opt;

  QueryOpts q_opts {};
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
        std::cout << argv[0] << " v" << Version::string() << std::endl;
        exit(EXIT_SUCCESS);
      case 'l':
        lsp::start_lsp();
        exit(EXIT_SUCCESS);
      case 'U':
        {
          std::string update_opt = std::string{optarg};
          if (update_opt == "unstable")
          {
            fs::remove(unstable_db);
            parse_pkgs(true);
            exit(EXIT_SUCCESS);
          }
          else if (update_opt == "stable")
          {
            fs::remove(stable_db);
            parse_pkgs(false);
            exit(EXIT_SUCCESS);
          }
          else if (update_opt == "both")
          {
            fs::remove(stable_db);
            fs::remove(unstable_db);
            parse_pkgs(false);
            parse_pkgs(true);
            exit(EXIT_SUCCESS);
          }
          else
          {
            usage(argv[0]);
            exit(EXIT_FAILURE);
          }
        }
      case 'u':
        if (!fs::exists(unstable_db))
        {
          std::cout << "[INFO] FETCHING FROM UNSTABLE...\n";
          parse_pkgs(true);
          std::cout << "[INFO] OK" << std::endl;
        }
        q_opts.use_unstable = true;
        break;
      case 'p':
        search_pkgs.emplace_back(optarg);
        break;
      case 's':
        search_pkg_set = std::string{optarg};
        break;
      case 'e':
        q_opts.enable_exact_matching = true;
        break;
      case 'S':
        q_opts.case_sensitive_search = true;
        break;
      default:
        usage(argv[0]);
        exit(EXIT_FAILURE);
    }
  }
  for (i32 i = optind; i < argc; i++)
    search_pkgs.emplace_back(argv[i]);
  query_pkgs(search_pkgs, search_pkg_set, q_opts);

  exit(EXIT_SUCCESS);
}

void usage(std::string_view pname)
{
  std::string usage_str =
      "Usage: {}\n\t[-h help] [-v version] [-l enable lsp-mode] [-p package(s)]\n\t"
      "[-s package set ] [-S case-sensitive search][-e enable exact matching]\n\t"
      "[-U {{stable|unstable|both}} update pkg index] [-u search unstable too]\n";

  std::cout << std::vformat(usage_str, std::make_format_args(pname))
            << std::endl;
}

void query_pkgs(std::vector<std::string> &target_pkgs, std::string &pkg_set, QueryOpts q_opts)
{
  std::string name_column_to_use {"name"};
  if (q_opts.case_sensitive_search) name_column_to_use = "csname";

  std::string partial {};
  if (q_opts.enable_exact_matching)
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

  if (q_opts.use_unstable)
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
  if (q_opts.enable_exact_matching)
    for (size_t i = 0; i < n; i++)
    {
      stmt.bind(i + 1, target_pkgs[i]);
      if (q_opts.use_unstable)
        stmt.bind((i + 1) + n, target_pkgs[i]);
    }
  else
    for (size_t i = 0; i < n; i++)
    {
      stmt.bind(i + 1, target_pkgs[i] + "%");
      if (q_opts.use_unstable)
        stmt.bind((i + 1) + n, target_pkgs[i] + "%");
    }
  if (!pkg_set.empty())
  {
    stmt.bind(target_pkgs.size() + 1, pkg_set + "%");
    stmt.bind((n + 1) + n, pkg_set + "%");
  }
  try
  {
    size_t count = 0;
    std::string connector = "  \u2570\u2500\u2500 "; // "  ╰─── ";
    std::cout << "\n";
    std::string pkg_col, branch, pkg_set, pkg_set_out, desc, desc_out, ver, ver_out;
    while (stmt.executeStep())
    {
      //   0      1       2      3      4
      // csname  ver  set_pref  desc  branch
      ver     = stmt.getColumn(1).getString();
      pkg_set = stmt.getColumn(2).getString();
      desc    = stmt.getColumn(3).getString();
      branch  = stmt.getColumn(4).getString();

      pkg_col     = branch[0] == 'S' ? GREEN : YELLOW;
      pkg_set_out = pkg_set.empty() ? "" : DIM " (" + pkg_set + ")" RESET;
      ver_out     = ver.empty() ? "" : BLUE " v" + ver + RESET;
      desc_out    = desc.empty() ? "\n\n" : "\n   " DIM + connector + RESET + desc + "\n\n";

      std::cout << pkg_col + "\u25cf  "<< stmt.getColumn(0) << RESET
                << ver_out << pkg_set_out << desc_out;
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

  //TODO : recursively create the path...may be non-existent
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

  db.exec("CREATE INDEX idx_csname ON pkgs (csname)");
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

TSParser* init_parser ()
{
  TSParser* parser = ts_parser_new();
  ts_parser_set_language(parser, tree_sitter_nix());
  return parser;
}

i32 parse_header(std::string header)
{
  std::string::size_type idx = header.find(':');
  if (idx == std::string::npos)
  {
    std::cerr<< "[ERROR] Invalid format for header!\n" << std::flush;
    return -1;
  }
  return std::stoi(header.substr(++idx));
}

void idx_file_by_newline (FileContext& ctx)
{
  ctx.line_offsets.clear();
  u64 n = ctx.src_code.size();
  ctx.line_offsets.emplace_back(0);
  for (u64 i {0}; i < n; i++)
    if (ctx.src_code[i] == '\n') ctx.line_offsets.emplace_back(i+1);
}

void scan_for_keywords(FileContext& ctx)
{
  TSTree* tree = ctx.CST;
  TSNode root = ts_tree_root_node(tree);
  TSTreeCursor cursor = ts_tree_cursor_new(root);

  while (true)
  {
    TSNode current_node = ts_tree_cursor_current_node(&cursor);
    std::string_view type = ts_node_type(current_node);
    if (type == "binding")
    {
      TSNode key_node = ts_node_child_by_field_name(current_node, "attrpath", 8);
      TSNode val_node = ts_node_child_by_field_name(current_node, "expression", 10);
      if (!ts_node_is_null(key_node) && !ts_node_is_null(val_node))
      {
        uint32_t kw_start = ts_node_start_byte(key_node);
        uint32_t kw_end = ts_node_end_byte(key_node);
        std::string key_name = ctx.src_code.substr(kw_start, kw_end - kw_start);
        if (zone_keywords.contains(key_name))
        {
          std::string_view val_node_type = ts_node_type(val_node);
          if (val_node_type == "list_expression" || val_node_type == "with_expression")
          {
            TSPoint zone_start = ts_node_start_point(val_node);
            TSPoint zone_end = ts_node_end_point(val_node);

            ctx.interest_zones.emplace_back(std::pair {
                  val_node_type == "list_expression"? ZoneType::STD_LIST: ZoneType::WITH_LIST,
                  Zone {
                    .start = zone_start.row,
                    .end = zone_end.row
                  }
                });
          }
        }
      }
    }

    if (ts_tree_cursor_goto_first_child(&cursor)) continue;
    if (ts_tree_cursor_goto_next_sibling(&cursor)) continue;

    bool backtracking = true;
    while (backtracking)
    {
      if (!ts_tree_cursor_goto_parent(&cursor)) break;
      if (ts_tree_cursor_goto_next_sibling(&cursor)) backtracking = false;
    }
    if (backtracking) break;
  }
  ts_tree_cursor_delete(&cursor);
}

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

void start_lsp ()
{
  parse_pkgs();
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
    std::string_view content {buffer.data(), (size_t)len};
    sjod::parser sd_parser;
    simdjson::padded_string p{content};
    auto doc = sd_parser.iterate(p);
    auto obj = doc.get_object();
    std::string_view method = obj["method"].get_string();

    if      (method == "initialize")              handle_lsp_init(obj);
    else if (method == "textDocument/didOpen")    handle_lsp_file_open(obj);
    else if (method == "textDocument/didChange")  handle_lsp_file_change(obj);
    else if (method == "textDocument/completion") handle_lsp_completion(obj);
    else if (method == "textDocument/hover")      handle_lsp_hover(obj);
    else continue;
  }
}

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
        {"completionProvider",
          JsonValue {
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

void parse_src_code (FileContext& ctx)
{
  TSTree* old_tree = ctx.CST;
  ctx.CST = ts_parser_parse_string(
      parser,
      old_tree,
      ctx.src_code.data(),
      ctx.src_code.length()
      );
  ts_tree_delete(old_tree);
}

void handle_lsp_file_open(auto &json_obj)
{
  try
  {
    auto file_params = json_obj["params"]["textDocument"].get_object();
    std::string file_uri = std::string {file_params["uri"].get_string().value()};
    FileContext ctx;
    ctx.src_code = std::string {file_params["text"].get_string().value()};
    parse_src_code (ctx);
    scan_for_keywords (ctx);
    idx_file_by_newline(ctx);
    file_ctxs.insert ({file_uri, std::move(ctx)});
  } catch (const std::exception& e)
  {
    std::cerr << "[OPEN ERROR] " <<e.what()<<std::endl;
  }
}

void handle_lsp_file_change (auto &json_obj)
{
  try
  {
    auto file_params = json_obj["params"]["textDocument"].get_object();
    std::string file_uri = std::string {file_params["uri"].get_string().value()};
    auto file_changes = json_obj["params"]["contentChanges"].get_array();
    FileContext& ctx = file_ctxs.at(file_uri);
    ts_tree_delete(ctx.CST);
    ctx.CST = nullptr;
    ctx.src_code = file_changes.at(0).find_field("text").get_string().value();
    parse_src_code (ctx);
    scan_for_keywords (ctx);
    idx_file_by_newline(ctx);
    //TODO: IMPL incremental changes which i have tried twice btw :( skill issue prob :|
  } catch (const std::exception& e)
  {
    std::cerr << "[SYNC ERROR] " <<e.what()<<std::endl;
  }
}

bool check_zone (list_zone_T& interest_zones, u32 line)
{
  for (auto& zone : interest_zones)
    if (line >= zone.second.start && line <= zone.second.end) return true;
  return false;
}

std::string get_node_text (TSNode node, std::string& src_code)
{
  u32 start = ts_node_start_byte(node);
  u32 end = ts_node_end_byte(node);
  return src_code.substr(start, end-start);
}

//INFO: might replace with raw string manipulation to extract hover contents...
std::pair<std::string, std::string> get_hover_contents (FileContext& ctx, TSPoint cursor_pos)
{
  TSNode root = ts_tree_root_node(ctx.CST);

  TSNode node_at_cursor = ts_node_named_descendant_for_point_range(root, cursor_pos, cursor_pos);
  TSNode list_element = node_at_cursor;
  TSNode parent = ts_node_parent(list_element);
  std::string_view parent_type = ts_node_type(parent);

  while (
      !ts_node_is_null(parent) &&
      parent_type != "list_expression" &&
      parent_type != "with_expression"
      )
  {
    list_element = parent;
    parent = ts_node_parent(parent);
    parent_type = ts_node_type(parent);
  }

  ZoneType zone_type = parent_type == "list_expression" ? ZoneType::STD_LIST : ZoneType::WITH_LIST;

  std::string pkgset {};
  std::string pkg {};

  std::string_view elem_type = ts_node_type(list_element);

  if (elem_type == "select_expression")
  {
    TSNode base_expr = ts_node_child_by_field_name(list_element, "expression", 10);
    TSNode base_id_node = ts_node_child_by_field_name(base_expr, "name", 4);
    std::string base_name = get_node_text(base_id_node, ctx.src_code);

    TSNode attrpath_node = ts_node_child_by_field_name(list_element, "attrpath", 8);
    uint32_t attr_count = ts_node_child_count(attrpath_node);

    std::vector<std::string> attributes {};

    if (zone_type == ZoneType::WITH_LIST) attributes.push_back(base_name);

    for (u32 i = 0; i < attr_count; i++)
    {
      TSNode child = ts_node_child(attrpath_node, i);
      if (std::string_view{ts_node_type(child)} == "identifier")
        attributes.push_back(get_node_text(child, ctx.src_code));
    }

    if (!attributes.empty())
    {
      pkg = attributes.back();
      attributes.pop_back();
      for (std::string& attr : attributes)
        pkgset += attr + ".";
      if(!pkgset.empty()) pkgset.pop_back();
    }
  }
  else if (elem_type == "variable_expression")
  {
    TSNode elem_name_expr = ts_node_child_by_field_name(list_element, "name", 4);
    pkg = get_node_text(elem_name_expr, ctx.src_code);
  }
  return std::pair {pkgset, pkg};
}

u32 search_left (u32 pos, std::string_view search_line)
{
  for (u32 i {pos}; i > 0; i--)
  {
    char c = search_line[i];
    if (c == ' ' || c == '\t' || c == '[' || c == ';') return i+1;
  }
  return pos;
}

void handle_lsp_completion (auto &json_obj)
{
  auto id = json_obj["id"];
  JsonValue id_value {};
  try
  {
    id_value = JsonValue{std::to_string(id.get_uint64())};
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
    std::string file_uri {params["textDocument"]["uri"].get_string().value()};
    auto cmp_pos = params["position"].get_object();
    TSPoint cursor_pos {
      .row = cmp_pos["line"].get_uint32(),
      .column = cmp_pos["character"].get_uint32()
    };
    FileContext& ctx = file_ctxs.at(file_uri);
    if (!check_zone(ctx.interest_zones, cursor_pos.row))
    {
      send_null_response(std::get<std::string>(id_value.value));
      return;
    }

    std::string pkgset {}, pkg {};

    u64 cursor_byte_offset { ctx.line_offsets[cursor_pos.row] + cursor_pos.column };
    u64 start_idx { search_left(cursor_byte_offset, ctx.src_code) };
    std::string chunk = ctx.src_code.substr(start_idx, cursor_byte_offset - start_idx);

    size_t last_dot = chunk.rfind('.');
    if (last_dot != std::string::npos)
    {
      pkg = chunk.substr(last_dot + 1);
      std::string full_prefix = chunk.substr(0, last_dot);

      if (full_prefix.find("pkgs.") == 0) pkgset = full_prefix.substr(5);
      else pkgset = full_prefix;
    } else pkg = chunk;

    if (pkg.empty() && pkgset.empty())
    {
      send_null_response(std::get<std::string>(id_value.value));
      return;
    }

    const std::string prefix_sql {"SELECT csname, version, set_prefix, desc FROM pkgs WHERE {} LIMIT 30"};
    const std::string sql_suffix_one   {"csname LIKE '{}%'"};
    const std::string sql_suffix_two   {"csname LIKE '{}%' AND set_prefix LIKE '{}%'"};
    const std::string sql_suffix_three {"set_prefix LIKE '{}%'"};

    std::string query_plan {};
    if (pkg.empty())
      query_plan = std::vformat(sql_suffix_three, std::make_format_args(pkgset));
    else if (pkgset.empty())
      query_plan = std::vformat(sql_suffix_one, std::make_format_args(pkg));
    else
      query_plan = std::vformat(sql_suffix_two, std::make_format_args(pkg, pkgset));
    std::string sql = std::vformat (prefix_sql, std::make_format_args(query_plan));

    response_obj.insert_or_assign("result", JsonValue {std::monostate {}});
    SQLite::Database db {stable_db.string(), SQLite::OPEN_READWRITE};
    SQLite::Statement query {db, sql};

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

void handle_lsp_hover(auto &json_obj)
{
  auto id = json_obj["id"];
  JsonValue id_value{};
  try
  {
    id_value = JsonValue{std::to_string(id.get_uint64())};
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

    std::string file_uri {json_obj["params"]["textDocument"]["uri"].get_string().value()};
    TSPoint cursor_pos {
      .row    = json_obj["params"]["position"]["line"].get_uint32(),
      .column = json_obj["params"]["position"]["character"].get_uint32()
    };
    FileContext& ctx = file_ctxs.at(file_uri);
    if (!check_zone(ctx.interest_zones, cursor_pos.row))
    {
      send_null_response(std::get<std::string>(id_value.value));
      return;
    }

    auto [pkgset, pkg] = get_hover_contents (ctx, cursor_pos);
    response_obj.insert_or_assign("result", JsonValue {std::monostate {}});
    SQLite::Database db {stable_db.string(), SQLite::OPEN_READWRITE};
    std::string sql = "SELECT csname, version, set_prefix, desc FROM pkgs"
                      " WHERE csname = ?";
    if (pkgset.empty()) sql += " AND set_prefix IS NULL";
    else sql += " AND set_prefix = ?";

    SQLite::Statement query {db, sql};
    query.bind(1, pkg);
    if (!pkgset.empty()) query.bind(2, pkgset);

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
