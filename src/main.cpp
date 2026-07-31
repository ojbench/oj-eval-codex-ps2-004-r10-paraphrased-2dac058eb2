#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

using i64 = long long;

struct Account {
  std::string id;
  std::string password;
  std::string name;
  int privilege = 0;
};

struct Book {
  int id = 0;
  std::string isbn;
  std::string name;
  std::string author;
  std::string keyword;
  std::vector<std::string> keyword_parts;
  i64 price_cents = 0;
  i64 quantity = 0;
};

struct Session {
  std::string user_id;
  int privilege = 0;
  int selected_book_id = 0;
};

struct Transaction {
  bool income = true;
  i64 cents = 0;
};

enum class OpCode : std::uint8_t {
  AccountUpsert = 1,
  AccountDelete = 2,
  BookUpsert = 3,
  Transaction = 4,
  LogEntry = 5,
};

std::string trim(const std::string &line) {
  std::size_t left = 0;
  while (left < line.size() && line[left] == ' ') {
    ++left;
  }
  std::size_t right = line.size();
  while (right > left && line[right - 1] == ' ') {
    --right;
  }
  return line.substr(left, right - left);
}

bool is_ascii_visible(char ch) {
  unsigned char uch = static_cast<unsigned char>(ch);
  return uch >= 32 && uch <= 126;
}

bool is_alnum_underscore(const std::string &value) {
  if (value.empty() || value.size() > 30) {
    return false;
  }
  for (char ch : value) {
    if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '_')) {
      return false;
    }
  }
  return true;
}

bool is_valid_username(const std::string &value) {
  if (value.empty() || value.size() > 30) {
    return false;
  }
  for (char ch : value) {
    if (!is_ascii_visible(ch)) {
      return false;
    }
  }
  return true;
}

bool is_valid_isbn(const std::string &value) {
  if (value.empty() || value.size() > 20) {
    return false;
  }
  for (char ch : value) {
    if (!is_ascii_visible(ch)) {
      return false;
    }
  }
  return true;
}

bool is_valid_book_string(const std::string &value) {
  if (value.empty() || value.size() > 60) {
    return false;
  }
  for (char ch : value) {
    if (!is_ascii_visible(ch) || ch == '"') {
      return false;
    }
  }
  return true;
}

std::optional<i64> parse_unsigned_limited(const std::string &value, std::size_t max_len) {
  if (value.empty() || value.size() > max_len) {
    return std::nullopt;
  }
  i64 result = 0;
  for (char ch : value) {
    if (!std::isdigit(static_cast<unsigned char>(ch))) {
      return std::nullopt;
    }
    result = result * 10 + (ch - '0');
    if (result > 2147483647LL) {
      return std::nullopt;
    }
  }
  return result;
}

std::optional<i64> parse_money_cents(const std::string &value, bool allow_zero) {
  if (value.empty() || value.size() > 13) {
    return std::nullopt;
  }
  std::size_t dot_pos = value.find('.');
  std::string int_part = value;
  std::string frac_part;
  if (dot_pos != std::string::npos) {
    if (value.find('.', dot_pos + 1) != std::string::npos) {
      return std::nullopt;
    }
    int_part = value.substr(0, dot_pos);
    frac_part = value.substr(dot_pos + 1);
    if (frac_part.empty() || frac_part.size() > 2) {
      return std::nullopt;
    }
  }
  if (int_part.empty()) {
    return std::nullopt;
  }
  i64 integral = 0;
  for (char ch : int_part) {
    if (!std::isdigit(static_cast<unsigned char>(ch))) {
      return std::nullopt;
    }
    integral = integral * 10 + (ch - '0');
  }
  i64 fractional = 0;
  if (!frac_part.empty()) {
    for (char ch : frac_part) {
      if (!std::isdigit(static_cast<unsigned char>(ch))) {
        return std::nullopt;
      }
      fractional = fractional * 10 + (ch - '0');
    }
    if (frac_part.size() == 1) {
      fractional *= 10;
    }
  }
  i64 cents = integral * 100 + fractional;
  if (!allow_zero && cents <= 0) {
    return std::nullopt;
  }
  return cents;
}

std::string format_money(i64 cents) {
  std::ostringstream out;
  out << (cents / 100) << '.' << std::setw(2) << std::setfill('0') << (std::llabs(cents) % 100);
  return out.str();
}

std::vector<std::string> split_keywords(const std::string &value) {
  std::vector<std::string> result;
  std::string current;
  for (char ch : value) {
    if (ch == '|') {
      result.push_back(current);
      current.clear();
    } else {
      current.push_back(ch);
    }
  }
  result.push_back(current);
  return result;
}

bool valid_keyword_string(const std::string &value, bool require_unique_parts) {
  if (!is_valid_book_string(value)) {
    return false;
  }
  auto parts = split_keywords(value);
  std::unordered_set<std::string> seen;
  for (const auto &part : parts) {
    if (part.empty()) {
      return false;
    }
    if (require_unique_parts && !seen.insert(part).second) {
      return false;
    }
  }
  return true;
}

std::vector<std::string> tokenize(const std::string &line) {
  std::vector<std::string> tokens;
  std::string current;
  bool in_quotes = false;
  for (char ch : line) {
    if (ch == '"') {
      current.push_back(ch);
      in_quotes = !in_quotes;
    } else if (ch == ' ' && !in_quotes) {
      if (!current.empty()) {
        tokens.push_back(current);
        current.clear();
      }
    } else {
      current.push_back(ch);
    }
  }
  if (in_quotes) {
    return {};
  }
  if (!current.empty()) {
    tokens.push_back(current);
  }
  return tokens;
}

class Store {
 public:
  Store() { load(); }

  bool process_line(const std::string &raw_line) {
    std::string line = trim(raw_line);
    if (line.empty()) {
      return true;
    }
    auto tokens = tokenize(line);
    if (tokens.empty()) {
      invalid();
      return true;
    }
    const std::string &cmd = tokens[0];
    try {
      if (cmd == "quit" || cmd == "exit") {
        if (tokens.size() != 1) {
          invalid();
          return true;
        }
        return false;
      }
      if (cmd == "su") {
        handle_su(tokens, line);
      } else if (cmd == "logout") {
        handle_logout(line);
      } else if (cmd == "register") {
        handle_register(tokens, line);
      } else if (cmd == "passwd") {
        handle_passwd(tokens, line);
      } else if (cmd == "useradd") {
        handle_useradd(tokens, line);
      } else if (cmd == "delete") {
        handle_delete(tokens, line);
      } else if (cmd == "show") {
        handle_show(tokens);
      } else if (cmd == "buy") {
        handle_buy(tokens, line);
      } else if (cmd == "select") {
        handle_select(tokens, line);
      } else if (cmd == "modify") {
        handle_modify(tokens, line);
      } else if (cmd == "import") {
        handle_import(tokens, line);
      } else if (cmd == "report") {
        handle_report(tokens);
      } else if (cmd == "log") {
        handle_log(tokens);
      } else {
        invalid();
      }
    } catch (...) {
      invalid();
    }
    return true;
  }

 private:
  std::unordered_map<std::string, Account> accounts_;
  std::unordered_map<int, Book> books_;
  std::map<std::string, int> isbn_to_id_;
  std::unordered_map<std::string, std::unordered_set<int>> name_index_;
  std::unordered_map<std::string, std::unordered_set<int>> author_index_;
  std::unordered_map<std::string, std::unordered_set<int>> keyword_index_;
  std::vector<Session> sessions_;
  std::vector<Transaction> transactions_;
  std::vector<std::string> logs_;
  int next_book_id_ = 1;
  std::filesystem::path db_path_ = "store.db";

  int current_privilege() const {
    return sessions_.empty() ? 0 : sessions_.back().privilege;
  }

  const std::string &current_user() const {
    static const std::string empty;
    return sessions_.empty() ? empty : sessions_.back().user_id;
  }

  void invalid() const { std::cout << "Invalid\n"; }

  void require_privilege(int need) {
    if (current_privilege() < need) {
      throw std::runtime_error("privilege");
    }
  }

  void write_string(std::ofstream &out, const std::string &value) {
    std::uint32_t len = static_cast<std::uint32_t>(value.size());
    out.write(reinterpret_cast<const char *>(&len), sizeof(len));
    out.write(value.data(), static_cast<std::streamsize>(value.size()));
  }

  bool read_string(std::ifstream &in, std::string &value) {
    std::uint32_t len = 0;
    if (!in.read(reinterpret_cast<char *>(&len), sizeof(len))) {
      return false;
    }
    value.resize(len);
    return static_cast<bool>(in.read(value.data(), static_cast<std::streamsize>(len)));
  }

  void append_account(const Account &account) {
    std::ofstream out(db_path_, std::ios::binary | std::ios::app);
    auto opcode = OpCode::AccountUpsert;
    out.write(reinterpret_cast<const char *>(&opcode), sizeof(opcode));
    write_string(out, account.id);
    write_string(out, account.password);
    write_string(out, account.name);
    out.write(reinterpret_cast<const char *>(&account.privilege), sizeof(account.privilege));
  }

  void append_account_delete(const std::string &user_id) {
    std::ofstream out(db_path_, std::ios::binary | std::ios::app);
    auto opcode = OpCode::AccountDelete;
    out.write(reinterpret_cast<const char *>(&opcode), sizeof(opcode));
    write_string(out, user_id);
  }

  void append_book(const Book &book) {
    std::ofstream out(db_path_, std::ios::binary | std::ios::app);
    auto opcode = OpCode::BookUpsert;
    out.write(reinterpret_cast<const char *>(&opcode), sizeof(opcode));
    out.write(reinterpret_cast<const char *>(&book.id), sizeof(book.id));
    write_string(out, book.isbn);
    write_string(out, book.name);
    write_string(out, book.author);
    write_string(out, book.keyword);
    out.write(reinterpret_cast<const char *>(&book.price_cents), sizeof(book.price_cents));
    out.write(reinterpret_cast<const char *>(&book.quantity), sizeof(book.quantity));
  }

  void append_transaction(bool income, i64 cents) {
    std::ofstream out(db_path_, std::ios::binary | std::ios::app);
    auto opcode = OpCode::Transaction;
    out.write(reinterpret_cast<const char *>(&opcode), sizeof(opcode));
    out.write(reinterpret_cast<const char *>(&income), sizeof(income));
    out.write(reinterpret_cast<const char *>(&cents), sizeof(cents));
  }

  void append_log(const std::string &line) {
    std::ofstream out(db_path_, std::ios::binary | std::ios::app);
    auto opcode = OpCode::LogEntry;
    out.write(reinterpret_cast<const char *>(&opcode), sizeof(opcode));
    write_string(out, line);
  }

  void rebuild_book_indexes() {
    isbn_to_id_.clear();
    name_index_.clear();
    author_index_.clear();
    keyword_index_.clear();
    for (auto &[id, book] : books_) {
      (void)id;
      index_book(book);
    }
  }

  void index_book(const Book &book) {
    isbn_to_id_[book.isbn] = book.id;
    if (!book.name.empty()) {
      name_index_[book.name].insert(book.id);
    }
    if (!book.author.empty()) {
      author_index_[book.author].insert(book.id);
    }
    for (const auto &part : book.keyword_parts) {
      keyword_index_[part].insert(book.id);
    }
  }

  void unindex_book(const Book &book) {
    isbn_to_id_.erase(book.isbn);
    if (!book.name.empty()) {
      auto it = name_index_.find(book.name);
      if (it != name_index_.end()) {
        it->second.erase(book.id);
        if (it->second.empty()) {
          name_index_.erase(it);
        }
      }
    }
    if (!book.author.empty()) {
      auto it = author_index_.find(book.author);
      if (it != author_index_.end()) {
        it->second.erase(book.id);
        if (it->second.empty()) {
          author_index_.erase(it);
        }
      }
    }
    for (const auto &part : book.keyword_parts) {
      auto it = keyword_index_.find(part);
      if (it != keyword_index_.end()) {
        it->second.erase(book.id);
        if (it->second.empty()) {
          keyword_index_.erase(it);
        }
      }
    }
  }

  void load() {
    accounts_.clear();
    books_.clear();
    transactions_.clear();
    logs_.clear();
    next_book_id_ = 1;

    if (!std::filesystem::exists(db_path_)) {
      Account root{"root", "sjtu", "root", 7};
      accounts_[root.id] = root;
      append_account(root);
      return;
    }

    std::ifstream in(db_path_, std::ios::binary);
    while (true) {
      OpCode opcode;
      if (!in.read(reinterpret_cast<char *>(&opcode), sizeof(opcode))) {
        break;
      }
      if (opcode == OpCode::AccountUpsert) {
        Account account;
        if (!read_string(in, account.id) || !read_string(in, account.password) ||
            !read_string(in, account.name) ||
            !in.read(reinterpret_cast<char *>(&account.privilege), sizeof(account.privilege))) {
          break;
        }
        accounts_[account.id] = account;
      } else if (opcode == OpCode::AccountDelete) {
        std::string user_id;
        if (!read_string(in, user_id)) {
          break;
        }
        accounts_.erase(user_id);
      } else if (opcode == OpCode::BookUpsert) {
        Book book;
        if (!in.read(reinterpret_cast<char *>(&book.id), sizeof(book.id)) || !read_string(in, book.isbn) ||
            !read_string(in, book.name) || !read_string(in, book.author) || !read_string(in, book.keyword) ||
            !in.read(reinterpret_cast<char *>(&book.price_cents), sizeof(book.price_cents)) ||
            !in.read(reinterpret_cast<char *>(&book.quantity), sizeof(book.quantity))) {
          break;
        }
        book.keyword_parts = book.keyword.empty() ? std::vector<std::string>{} : split_keywords(book.keyword);
        books_[book.id] = book;
        next_book_id_ = std::max(next_book_id_, book.id + 1);
      } else if (opcode == OpCode::Transaction) {
        Transaction tx;
        if (!in.read(reinterpret_cast<char *>(&tx.income), sizeof(tx.income)) ||
            !in.read(reinterpret_cast<char *>(&tx.cents), sizeof(tx.cents))) {
          break;
        }
        transactions_.push_back(tx);
      } else if (opcode == OpCode::LogEntry) {
        std::string entry;
        if (!read_string(in, entry)) {
          break;
        }
        logs_.push_back(entry);
      } else {
        break;
      }
    }
    rebuild_book_indexes();
    if (!accounts_.contains("root")) {
      Account root{"root", "sjtu", "root", 7};
      accounts_[root.id] = root;
      append_account(root);
    }
  }

  void log_success(const std::string &command, const std::string &actor_override = "") {
    std::string actor = actor_override.empty() ? (current_user().empty() ? "guest" : current_user()) : actor_override;
    std::string entry = actor + ": " + command;
    logs_.push_back(entry);
    append_log(entry);
  }

  Book *selected_book() {
    if (sessions_.empty() || sessions_.back().selected_book_id == 0) {
      return nullptr;
    }
    auto it = books_.find(sessions_.back().selected_book_id);
    if (it == books_.end()) {
      return nullptr;
    }
    return &it->second;
  }

  void handle_su(const std::vector<std::string> &tokens, const std::string &line) {
    if (tokens.size() != 2 && tokens.size() != 3) {
      throw std::runtime_error("su");
    }
    if (!is_alnum_underscore(tokens[1])) {
      throw std::runtime_error("su");
    }
    auto it = accounts_.find(tokens[1]);
    if (it == accounts_.end()) {
      throw std::runtime_error("su");
    }
    if (tokens.size() == 3) {
      if (!is_alnum_underscore(tokens[2]) || it->second.password != tokens[2]) {
        throw std::runtime_error("su");
      }
    } else if (current_privilege() <= it->second.privilege) {
      throw std::runtime_error("su");
    }
    sessions_.push_back(Session{it->second.id, it->second.privilege, 0});
    log_success(line);
  }

  void handle_logout(const std::string &line) {
    require_privilege(1);
    if (sessions_.empty()) {
      throw std::runtime_error("logout");
    }
    std::string actor = sessions_.back().user_id;
    sessions_.pop_back();
    log_success(line, actor);
  }

  void handle_register(const std::vector<std::string> &tokens, const std::string &line) {
    if (tokens.size() != 4 || !is_alnum_underscore(tokens[1]) || !is_alnum_underscore(tokens[2]) ||
        !is_valid_username(tokens[3]) || accounts_.contains(tokens[1])) {
      throw std::runtime_error("register");
    }
    Account account{tokens[1], tokens[2], tokens[3], 1};
    accounts_[account.id] = account;
    append_account(account);
    log_success(line);
  }

  void handle_passwd(const std::vector<std::string> &tokens, const std::string &line) {
    require_privilege(1);
    if ((tokens.size() != 3 && tokens.size() != 4) || !is_alnum_underscore(tokens[1])) {
      throw std::runtime_error("passwd");
    }
    auto it = accounts_.find(tokens[1]);
    if (it == accounts_.end()) {
      throw std::runtime_error("passwd");
    }
    std::string new_password;
    if (tokens.size() == 3) {
      if (current_privilege() != 7 || !is_alnum_underscore(tokens[2])) {
        throw std::runtime_error("passwd");
      }
      new_password = tokens[2];
    } else {
      if (!is_alnum_underscore(tokens[2]) || !is_alnum_underscore(tokens[3]) || it->second.password != tokens[2]) {
        throw std::runtime_error("passwd");
      }
      new_password = tokens[3];
    }
    it->second.password = new_password;
    append_account(it->second);
    log_success(line);
  }

  void handle_useradd(const std::vector<std::string> &tokens, const std::string &line) {
    require_privilege(3);
    if (tokens.size() != 5 || !is_alnum_underscore(tokens[1]) || !is_alnum_underscore(tokens[2]) ||
        !is_valid_username(tokens[4]) || accounts_.contains(tokens[1])) {
      throw std::runtime_error("useradd");
    }
    if (tokens[3].size() != 1 || !std::isdigit(static_cast<unsigned char>(tokens[3][0]))) {
      throw std::runtime_error("useradd");
    }
    int privilege = tokens[3][0] - '0';
    if ((privilege != 1 && privilege != 3 && privilege != 7) || privilege >= current_privilege()) {
      throw std::runtime_error("useradd");
    }
    Account account{tokens[1], tokens[2], tokens[4], privilege};
    accounts_[account.id] = account;
    append_account(account);
    log_success(line);
  }

  void handle_delete(const std::vector<std::string> &tokens, const std::string &line) {
    require_privilege(7);
    if (tokens.size() != 2 || !is_alnum_underscore(tokens[1])) {
      throw std::runtime_error("delete");
    }
    auto it = accounts_.find(tokens[1]);
    if (it == accounts_.end()) {
      throw std::runtime_error("delete");
    }
    for (const auto &session : sessions_) {
      if (session.user_id == tokens[1]) {
        throw std::runtime_error("delete");
      }
    }
    accounts_.erase(it);
    append_account_delete(tokens[1]);
    log_success(line);
  }

  void print_book(const Book &book) const {
    std::cout << book.isbn << '\t' << book.name << '\t' << book.author << '\t' << book.keyword << '\t'
              << format_money(book.price_cents) << '\t' << book.quantity << '\n';
  }

  void handle_show(const std::vector<std::string> &tokens) {
    if (tokens.size() >= 2 && tokens[1] == "finance") {
      require_privilege(7);
      if (tokens.size() > 3) {
        throw std::runtime_error("show finance");
      }
      if (tokens.size() == 2) {
        i64 income = 0;
        i64 expense = 0;
        for (const auto &tx : transactions_) {
          if (tx.income) {
            income += tx.cents;
          } else {
            expense += tx.cents;
          }
        }
        std::cout << "+ " << format_money(income) << " - " << format_money(expense) << '\n';
        return;
      }
      auto count = parse_unsigned_limited(tokens[2], 10);
      if (!count || *count > static_cast<i64>(transactions_.size())) {
        throw std::runtime_error("show finance");
      }
      if (*count == 0) {
        std::cout << '\n';
        return;
      }
      i64 income = 0;
      i64 expense = 0;
      for (std::size_t i = transactions_.size() - static_cast<std::size_t>(*count); i < transactions_.size(); ++i) {
        if (transactions_[i].income) {
          income += transactions_[i].cents;
        } else {
          expense += transactions_[i].cents;
        }
      }
      std::cout << "+ " << format_money(income) << " - " << format_money(expense) << '\n';
      return;
    }

    require_privilege(1);
    if (tokens.size() > 2) {
      throw std::runtime_error("show");
    }
    std::vector<int> result_ids;
    if (tokens.size() == 1) {
      result_ids.reserve(books_.size());
      for (const auto &[isbn, id] : isbn_to_id_) {
        (void)isbn;
        result_ids.push_back(id);
      }
    } else {
      const auto &token = tokens[1];
      auto collect = [&](const std::unordered_set<int> *source) {
        if (source == nullptr) {
          return;
        }
        result_ids.insert(result_ids.end(), source->begin(), source->end());
      };
      if (token.rfind("-ISBN=", 0) == 0) {
        std::string value = token.substr(6);
        if (!is_valid_isbn(value)) {
          throw std::runtime_error("show");
        }
        auto it = isbn_to_id_.find(value);
        if (it != isbn_to_id_.end()) {
          result_ids.push_back(it->second);
        }
      } else if (token.rfind("-name=\"", 0) == 0 && token.size() >= 8 && token.back() == '"') {
        std::string value = token.substr(7, token.size() - 8);
        if (!is_valid_book_string(value)) {
          throw std::runtime_error("show");
        }
        auto it = name_index_.find(value);
        collect(it == name_index_.end() ? nullptr : &it->second);
      } else if (token.rfind("-author=\"", 0) == 0 && token.size() >= 10 && token.back() == '"') {
        std::string value = token.substr(9, token.size() - 10);
        if (!is_valid_book_string(value)) {
          throw std::runtime_error("show");
        }
        auto it = author_index_.find(value);
        collect(it == author_index_.end() ? nullptr : &it->second);
      } else if (token.rfind("-keyword=\"", 0) == 0 && token.size() >= 11 && token.back() == '"') {
        std::string value = token.substr(10, token.size() - 11);
        if (!valid_keyword_string(value, false) || value.find('|') != std::string::npos) {
          throw std::runtime_error("show");
        }
        auto it = keyword_index_.find(value);
        collect(it == keyword_index_.end() ? nullptr : &it->second);
      } else {
        throw std::runtime_error("show");
      }
    }
    std::sort(result_ids.begin(), result_ids.end(), [&](int lhs, int rhs) { return books_[lhs].isbn < books_[rhs].isbn; });
    for (int id : result_ids) {
      print_book(books_[id]);
    }
    if (result_ids.empty()) {
      std::cout << '\n';
    }
  }

  void handle_buy(const std::vector<std::string> &tokens, const std::string &line) {
    require_privilege(1);
    if (tokens.size() != 3 || !is_valid_isbn(tokens[1])) {
      throw std::runtime_error("buy");
    }
    auto quantity = parse_unsigned_limited(tokens[2], 10);
    if (!quantity || *quantity <= 0) {
      throw std::runtime_error("buy");
    }
    auto it = isbn_to_id_.find(tokens[1]);
    if (it == isbn_to_id_.end()) {
      throw std::runtime_error("buy");
    }
    Book &book = books_[it->second];
    if (book.quantity < *quantity) {
      throw std::runtime_error("buy");
    }
    book.quantity -= *quantity;
    append_book(book);
    i64 total = book.price_cents * *quantity;
    transactions_.push_back(Transaction{true, total});
    append_transaction(true, total);
    log_success(line);
    std::cout << format_money(total) << '\n';
  }

  void handle_select(const std::vector<std::string> &tokens, const std::string &line) {
    require_privilege(3);
    if (tokens.size() != 2 || !is_valid_isbn(tokens[1])) {
      throw std::runtime_error("select");
    }
    int id = 0;
    auto it = isbn_to_id_.find(tokens[1]);
    if (it == isbn_to_id_.end()) {
      Book book;
      book.id = next_book_id_++;
      book.isbn = tokens[1];
      id = book.id;
      books_[book.id] = book;
      index_book(books_[book.id]);
      append_book(books_[book.id]);
    } else {
      id = it->second;
    }
    sessions_.back().selected_book_id = id;
    log_success(line);
  }

  void handle_modify(const std::vector<std::string> &tokens, const std::string &line) {
    require_privilege(3);
    if (tokens.size() < 2) {
      throw std::runtime_error("modify");
    }
    Book *book = selected_book();
    if (book == nullptr) {
      throw std::runtime_error("modify");
    }

    std::optional<std::string> new_isbn;
    std::optional<std::string> new_name;
    std::optional<std::string> new_author;
    std::optional<std::string> new_keyword;
    std::optional<i64> new_price;
    std::set<std::string> seen;

    for (std::size_t i = 1; i < tokens.size(); ++i) {
      const auto &token = tokens[i];
      if (token.rfind("-ISBN=", 0) == 0) {
        if (!seen.insert("ISBN").second) {
          throw std::runtime_error("modify");
        }
        std::string value = token.substr(6);
        if (!is_valid_isbn(value) || value == book->isbn) {
          throw std::runtime_error("modify");
        }
        auto it = isbn_to_id_.find(value);
        if (it != isbn_to_id_.end() && it->second != book->id) {
          throw std::runtime_error("modify");
        }
        new_isbn = value;
      } else if (token.rfind("-name=\"", 0) == 0 && token.size() >= 8 && token.back() == '"') {
        if (!seen.insert("name").second) {
          throw std::runtime_error("modify");
        }
        std::string value = token.substr(7, token.size() - 8);
        if (!is_valid_book_string(value)) {
          throw std::runtime_error("modify");
        }
        new_name = value;
      } else if (token.rfind("-author=\"", 0) == 0 && token.size() >= 10 && token.back() == '"') {
        if (!seen.insert("author").second) {
          throw std::runtime_error("modify");
        }
        std::string value = token.substr(9, token.size() - 10);
        if (!is_valid_book_string(value)) {
          throw std::runtime_error("modify");
        }
        new_author = value;
      } else if (token.rfind("-keyword=\"", 0) == 0 && token.size() >= 11 && token.back() == '"') {
        if (!seen.insert("keyword").second) {
          throw std::runtime_error("modify");
        }
        std::string value = token.substr(10, token.size() - 11);
        if (!valid_keyword_string(value, true)) {
          throw std::runtime_error("modify");
        }
        new_keyword = value;
      } else if (token.rfind("-price=", 0) == 0) {
        if (!seen.insert("price").second) {
          throw std::runtime_error("modify");
        }
        auto value = parse_money_cents(token.substr(7), true);
        if (!value) {
          throw std::runtime_error("modify");
        }
        new_price = *value;
      } else {
        throw std::runtime_error("modify");
      }
    }

    Book updated = *book;
    if (new_isbn) {
      updated.isbn = *new_isbn;
    }
    if (new_name) {
      updated.name = *new_name;
    }
    if (new_author) {
      updated.author = *new_author;
    }
    if (new_keyword) {
      updated.keyword = *new_keyword;
      updated.keyword_parts = split_keywords(*new_keyword);
    }
    if (new_price) {
      updated.price_cents = *new_price;
    }
    unindex_book(*book);
    *book = updated;
    index_book(*book);
    append_book(*book);
    log_success(line);
  }

  void handle_import(const std::vector<std::string> &tokens, const std::string &line) {
    require_privilege(3);
    if (tokens.size() != 3) {
      throw std::runtime_error("import");
    }
    auto quantity = parse_unsigned_limited(tokens[1], 10);
    auto total_cost = parse_money_cents(tokens[2], false);
    if (!quantity || *quantity <= 0 || !total_cost) {
      throw std::runtime_error("import");
    }
    Book *book = selected_book();
    if (book == nullptr) {
      throw std::runtime_error("import");
    }
    book->quantity += *quantity;
    append_book(*book);
    transactions_.push_back(Transaction{false, *total_cost});
    append_transaction(false, *total_cost);
    log_success(line);
  }

  void handle_report(const std::vector<std::string> &tokens) {
    require_privilege(7);
    if (tokens.size() != 2) {
      throw std::runtime_error("report");
    }
    if (tokens[1] == "finance") {
      i64 income = 0;
      i64 expense = 0;
      for (const auto &tx : transactions_) {
        if (tx.income) {
          income += tx.cents;
        } else {
          expense += tx.cents;
        }
      }
      std::cout << "finance report\n";
      std::cout << "income " << format_money(income) << '\n';
      std::cout << "expense " << format_money(expense) << '\n';
    } else if (tokens[1] == "employee") {
      std::cout << "employee report\n";
      for (const auto &entry : logs_) {
        std::cout << entry << '\n';
      }
    } else {
      throw std::runtime_error("report");
    }
  }

  void handle_log(const std::vector<std::string> &tokens) {
    require_privilege(7);
    if (tokens.size() != 1) {
      throw std::runtime_error("log");
    }
    for (const auto &entry : logs_) {
      std::cout << entry << '\n';
    }
  }
};

}  // namespace

int main() {
  std::ios::sync_with_stdio(false);
  std::cin.tie(nullptr);

  Store store;
  std::string line;
  while (std::getline(std::cin, line)) {
    if (!store.process_line(line)) {
      break;
    }
  }
  return 0;
}
