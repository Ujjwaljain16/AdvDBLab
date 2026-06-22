// Lab 7 - SQL WHERE clause via a recursive-descent parser (AST + tree walk)
// Ujjwal Jain (24BCS10173)
// a hand-written lexer turns the query into tokens, a recursive-descent parser
// builds an AST whose *shape* already encodes precedence (comparisons bind
// tightest, then AND, then OR), and the executor walks that tree once per row.
// because precedence lives in the grammar, the evaluator needs no precedence
// table at all.

#include <cctype>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

enum class Kind {
    Select, From, Where, And, Or,
    Name, Int, Cmp, LParen, RParen, Eof
};

struct Token {
    Kind kind;
    std::string text;
};

static std::string upper(const std::string& s) {
    std::string r;
    for (char c : s) r.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    return r;
}

static std::vector<Token> lex(const std::string& sql) {
    std::vector<Token> toks;
    std::size_t i = 0;
    while (i < sql.size()) {
        char c = sql[i];
        if (std::isspace(static_cast<unsigned char>(c))) { ++i; continue; }

        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            std::size_t j = i;
            while (j < sql.size() && (std::isalnum(static_cast<unsigned char>(sql[j])) || sql[j] == '_')) ++j;
            std::string w = sql.substr(i, j - i);
            i = j;
            std::string u = upper(w);
            if (u == "SELECT")     toks.push_back({Kind::Select, w});
            else if (u == "FROM")  toks.push_back({Kind::From,  w});
            else if (u == "WHERE") toks.push_back({Kind::Where, w});
            else if (u == "AND")   toks.push_back({Kind::And,   w});
            else if (u == "OR")    toks.push_back({Kind::Or,    w});
            else                   toks.push_back({Kind::Name,  w});
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(c))) {
            std::size_t j = i;
            while (j < sql.size() && std::isdigit(static_cast<unsigned char>(sql[j]))) ++j;
            toks.push_back({Kind::Int, sql.substr(i, j - i)});
            i = j;
            continue;
        }
        if ((c == '>' || c == '<') && i + 1 < sql.size() && sql[i + 1] == '=') {
            toks.push_back({Kind::Cmp, std::string() + c + '='});
            i += 2;
            continue;
        }
        if (c == '>' || c == '<' || c == '=') { toks.push_back({Kind::Cmp, std::string(1, c)}); ++i; continue; }
        if (c == '(') { toks.push_back({Kind::LParen, "("}); ++i; continue; }
        if (c == ')') { toks.push_back({Kind::RParen, ")"}); ++i; continue; }
        throw std::runtime_error(std::string("unexpected character: ") + c);
    }
    toks.push_back({Kind::Eof, ""});
    return toks;
}

// AST: a single tagged node type, no virtual hierarchy -> no dynamic_cast at eval.
enum class NodeType { Column, Number, BinOp };

struct Expr {
    NodeType type;
    std::string text;            // column name / number / operator
    std::unique_ptr<Expr> l, r;
};

static std::unique_ptr<Expr> col(const std::string& n) {
    return std::unique_ptr<Expr>(new Expr{NodeType::Column, n, nullptr, nullptr});
}
static std::unique_ptr<Expr> num(const std::string& n) {
    return std::unique_ptr<Expr>(new Expr{NodeType::Number, n, nullptr, nullptr});
}
static std::unique_ptr<Expr> bin(const std::string& op, std::unique_ptr<Expr> a, std::unique_ptr<Expr> b) {
    return std::unique_ptr<Expr>(new Expr{NodeType::BinOp, op, std::move(a), std::move(b)});
}

struct Query {
    std::string column, table;
    std::unique_ptr<Expr> where;
};

// grammar (precedence is encoded by the nesting order):
//   query  := SELECT Name FROM Name WHERE expr
//   expr   := term  (OR  term)*      <- OR binds loosest
//   term   := factor (AND factor)*   <- AND binds tighter
//   factor := '(' expr ')' | Name Cmp Int   <- comparisons bind tightest
class Parser {
public:
    explicit Parser(std::vector<Token> t) : toks(std::move(t)) {}

    Query parse() {
        expect(Kind::Select);
        std::string c = expect(Kind::Name).text;
        expect(Kind::From);
        std::string tbl = expect(Kind::Name).text;
        expect(Kind::Where);
        auto w = expr();
        expect(Kind::Eof);
        return Query{c, tbl, std::move(w)};
    }

private:
    std::unique_ptr<Expr> expr() {
        auto left = term();
        while (peek() == Kind::Or) { ++pos; left = bin("OR", std::move(left), term()); }
        return left;
    }
    std::unique_ptr<Expr> term() {
        auto left = factor();
        while (peek() == Kind::And) { ++pos; left = bin("AND", std::move(left), factor()); }
        return left;
    }
    std::unique_ptr<Expr> factor() {
        if (peek() == Kind::LParen) {
            ++pos;
            auto e = expr();
            expect(Kind::RParen);
            return e;
        }
        std::string c = expect(Kind::Name).text;
        std::string op = expect(Kind::Cmp).text;
        std::string n = expect(Kind::Int).text;
        return bin(op, col(c), num(n));
    }

    Kind peek() const { return toks[pos].kind; }
    Token expect(Kind k) {
        if (toks[pos].kind != k)
            throw std::runtime_error("unexpected token near '" + toks[pos].text + "'");
        return toks[pos++];
    }

    std::vector<Token> toks;
    std::size_t pos = 0;
};

struct Student {
    int id;
    std::string name;
    int age;
    int marks;
};

static int columnValue(const std::string& c, const Student& s) {
    if (c == "id")    return s.id;
    if (c == "age")   return s.age;
    if (c == "marks") return s.marks;
    throw std::runtime_error("unknown column: " + c);
}

static bool eval(const Expr* e, const Student& row) {
    if (e->text == "AND") return eval(e->l.get(), row) && eval(e->r.get(), row);
    if (e->text == "OR")  return eval(e->l.get(), row) || eval(e->r.get(), row);
    int a = columnValue(e->l->text, row);
    int b = std::stoi(e->r->text);
    const std::string& op = e->text;
    if (op == ">")  return a >  b;
    if (op == "<")  return a <  b;
    if (op == ">=") return a >= b;
    if (op == "<=") return a <= b;
    if (op == "=")  return a == b;
    throw std::runtime_error("unknown operator: " + op);
}

// pretty-print the AST so the encoded precedence is visible.
static void printAst(const Expr* e, int depth = 0) {
    std::cout << std::string(depth * 2, ' ') << e->text << "\n";
    if (e->type == NodeType::BinOp) {
        printAst(e->l.get(), depth + 1);
        printAst(e->r.get(), depth + 1);
    }
}

int main() {
    std::vector<Student> students = {
        {1, "Ishan", 19, 88},
        {2, "Tara",  22, 67},
        {3, "Veer",  20, 91},
        {4, "Anya",  23, 74},
        {5, "Dev",   21, 95},
        {6, "Riya",  18, 59},
    };

    std::string sql = "SELECT name FROM students WHERE marks >= 80 AND (age < 20 OR id = 5)";
    std::cout << "Query: " << sql << "\n\n";

    Parser parser(lex(sql));
    Query q = parser.parse();

    std::cout << "WHERE as an AST (precedence baked into the tree):\n";
    printAst(q.where.get());
    std::cout << "\n";

    std::cout << "Matching rows:\n";
    for (const Student& s : students) {
        if (!eval(q.where.get(), s)) continue;
        if      (q.column == "name")  std::cout << "  " << s.name  << "\n";
        else if (q.column == "id")    std::cout << "  " << s.id    << "\n";
        else if (q.column == "age")   std::cout << "  " << s.age   << "\n";
        else if (q.column == "marks") std::cout << "  " << s.marks << "\n";
    }
    return 0;
}