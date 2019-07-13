#include <tree_sitter/parser.h>
#include <vector>
#include <string>
#include <cwctype>

namespace {

using std::vector;
using std::string;

enum TokenType {
  AUTOMATIC_SEMICOLON,
  HEREDOC,
  TEXT,
  END_TAG,
  START_TAG,
};

struct Heredoc {
  Heredoc() : end_word_indentation_allowed(false) {}

  string word;
  bool end_word_indentation_allowed;
};

struct Scanner {
  bool is_in_script_section;
  bool has_leading_whitespace;
  vector<Heredoc> open_heredocs;

  Scanner() {
    is_in_script_section = false;
    has_leading_whitespace = false;
  }

  void reset() {
    open_heredocs.clear();
  }

  enum ScanContentResult {
    Error,
    End
  };

  unsigned serialize(char *buffer) {
    unsigned i = 0;

    buffer[i++] = is_in_script_section;
    buffer[i++] = open_heredocs.size();
    for (
      vector<Heredoc>::iterator iter = open_heredocs.begin(),
      end = open_heredocs.end();
      iter != end;
      ++iter
    ) {
      if (i + 2 + iter->word.size() >= TREE_SITTER_SERIALIZATION_BUFFER_SIZE) return 0;
      buffer[i++] = iter->end_word_indentation_allowed;
      buffer[i++] = iter->word.size();
      iter->word.copy(&buffer[i], iter->word.size());
      i += iter->word.size();
    }

    return i;
  }

  void deserialize(const char *buffer, unsigned length) {
    unsigned i = 0;
    has_leading_whitespace = false;
    open_heredocs.clear();

    if (length == 0) return;

    is_in_script_section = buffer[i++];
    uint8_t open_heredoc_count = buffer[i++];
    for (unsigned j = 0; j < open_heredoc_count; j++) {
      Heredoc heredoc;
      heredoc.end_word_indentation_allowed = buffer[i++];
      uint8_t word_length = buffer[i++];
      heredoc.word.assign(buffer + i, buffer + i + word_length);
      i += word_length;
      open_heredocs.push_back(heredoc);
    }

    // assert(i == length);
  }

  void skip(TSLexer *lexer) {
    has_leading_whitespace = true;
    lexer->advance(lexer, true);
  }

  void advance(TSLexer *lexer) {
    lexer->advance(lexer, false);
  }

  bool lookahead_is_line_end(TSLexer *lexer) {
    if (lexer->lookahead == '\n') {
      return true;
    } else if (lexer->lookahead == '\r') {
      skip(lexer);
      if (lexer->lookahead == '\n') {
        return true;
      }
    }

    return false;
  }

  bool scan_whitespace(TSLexer *lexer) {
    for (;;) {
      while (iswspace(lexer->lookahead)) {
        advance(lexer);
      }

      if (lexer->lookahead == '/') {
        advance(lexer);

        if (lexer->lookahead == '/') {
          advance(lexer);
          while (lexer->lookahead != 0 && lexer->lookahead != '\n') {
            advance(lexer);
          }
        } else {
          return false;
        }
      } else {
        return true;
      }
    }
  }

  string scan_heredoc_word(TSLexer *lexer) {
    string result;
    int32_t quote;

    switch (lexer->lookahead) {
      case '\'':
        quote = lexer->lookahead;
        advance(lexer);
        while (lexer->lookahead != quote && lexer->lookahead != 0) {
          result += lexer->lookahead;
          advance(lexer);
        }
        advance(lexer);
        break;

      default:
        if (iswalnum(lexer->lookahead) || lexer->lookahead == '_') {
          result += lexer->lookahead;
          advance(lexer);
          while (iswalnum(lexer->lookahead) || lexer->lookahead == '_') {
            result += lexer->lookahead;
            advance(lexer);
          }
        }
        break;
    }

    return result;
  }


  ScanContentResult scan_heredoc_content(TSLexer *lexer) {
    if (open_heredocs.empty()) return Error;
    Heredoc heredoc = open_heredocs.front();
    size_t position_in_word = 0;

    for (;;) {
      if (position_in_word == heredoc.word.size()) {
        if (lexer->lookahead == ';' || lexer->lookahead == '\n') {
          open_heredocs.erase(open_heredocs.begin());
          return End;
        }

        position_in_word = 0;
      }
      if (lexer->lookahead == 0) {
        open_heredocs.erase(open_heredocs.begin());
        return Error;
      }

      if (lexer->lookahead == heredoc.word[position_in_word]) {
        advance(lexer);
        position_in_word++;
      } else {
        position_in_word = 0;
        advance(lexer);
      }
    }
  }

  bool scan_text_content(TSLexer *lexer) {
    enum {
      START,
      AFTER_LESS_THAN,
      AFTER_QUESTION,
      AFTER_P,
      AFTER_PH,
      AFTER_PHP,
      DONE
    } state = START;

    bool did_advance = false;
    bool has_content = false;

    while (state != DONE) {
      if (lexer->lookahead == 0) {
        if (did_advance) has_content = true;
        lexer->mark_end(lexer);
        state = DONE;
      }

      switch (state) {
        case START:
          if (lexer->lookahead == '<') {
            if (did_advance) has_content = true;
            lexer->mark_end(lexer);
            state = AFTER_LESS_THAN;
          }
          break;
        case AFTER_LESS_THAN:
          if (lexer->lookahead == '?') {
            state = AFTER_QUESTION;
          } else {
            state = START;
          }
          break;
        case AFTER_QUESTION:
          // Short tag <?
          if (iswspace(lexer->lookahead)) {
            state = DONE;
          } else if (lexer->lookahead == '=') {
            advance(lexer);
            if (iswspace(lexer->lookahead)) {
              state = DONE;
            } else {
              state = START;
            }
          } else if (lexer->lookahead == 'p' || lexer->lookahead == 'P') {
            state = AFTER_P;
          } else {
            state = START;
          }
          break;
        case AFTER_P:
          if (lexer->lookahead == 'h' || lexer->lookahead == 'H') {
            state = AFTER_PH;
          } else {
            state = START;
          }
          break;
        case AFTER_PH:
          if (lexer->lookahead == 'p' || lexer->lookahead == 'P') {
            state = AFTER_PHP;
          } else {
            state = START;
          }
          break;
        case AFTER_PHP:
          if (iswspace(lexer->lookahead)) {
            state = DONE;
          } else {
            state = START;
          }
          break;
      }

      advance(lexer);
      did_advance = true;
    }

    return has_content;
  }

  bool scan_start_tag(TSLexer *lexer) {
    if (lexer->lookahead != '<') return false;
    advance(lexer);
    if (lexer->lookahead != '?') return false;
    advance(lexer);
    if (lexer->lookahead == '=') {
      advance(lexer);
      if (iswspace(lexer->lookahead)) {
        lexer->mark_end(lexer);
        return true;
      }

      return false;
    }
    if (iswspace(lexer->lookahead)) {
      lexer->mark_end(lexer);
      return true;
    }
    if (lexer->lookahead != 'p' && lexer->lookahead != 'P') return false;
    advance(lexer);
    if (lexer->lookahead != 'h' && lexer->lookahead != 'H') return false;
    advance(lexer);
    if (lexer->lookahead != 'p' && lexer->lookahead != 'P') return false;
    advance(lexer);
    if (!iswspace(lexer->lookahead)) return false;
    lexer->mark_end(lexer);

    return true;
  }

  bool scan_end_tag(TSLexer *lexer) {
    if (lexer->lookahead != '?') return false;
    advance(lexer);
    if (lexer->lookahead != '>') return false;
    advance(lexer);
    lexer->mark_end(lexer);

    return true;
  }

  bool scan(TSLexer *lexer, const bool *valid_symbols) {
    has_leading_whitespace = false;

    lexer->mark_end(lexer);

    if (valid_symbols[TEXT]) {
      if (valid_symbols[START_TAG] && scan_start_tag(lexer)) {
        lexer->result_symbol = START_TAG;
        is_in_script_section = true;
        
        return true;
      }
      if (is_in_script_section) {
        return false;
      }

      lexer->result_symbol = TEXT;
      return scan_text_content(lexer);
    }

    if (valid_symbols[START_TAG]) {
      lexer->result_symbol = START_TAG;
      is_in_script_section = true;
      return scan_start_tag(lexer);
    }

    if (!scan_whitespace(lexer)) return false;

    if (valid_symbols[END_TAG]) {
      lexer->result_symbol = END_TAG;
      is_in_script_section = false;
      return scan_end_tag(lexer);
    }

    if (valid_symbols[HEREDOC]) {
      if (lexer->lookahead == '<') {
        advance(lexer);
        if (lexer->lookahead != '<') return false;
        advance(lexer);
        if (lexer->lookahead != '<') return false;
        advance(lexer);

        if (!scan_whitespace(lexer)) return false;

        // Found a heredoc
        Heredoc heredoc;
        heredoc.word = scan_heredoc_word(lexer);
        if (heredoc.word.empty()) return false;
        open_heredocs.push_back(heredoc);

        switch (scan_heredoc_content(lexer)) {
          case Error:
            return false;
          case End:
            lexer->result_symbol = HEREDOC;
            lexer->mark_end(lexer);
            return true;
        }
      }
    }

    if (valid_symbols[AUTOMATIC_SEMICOLON]) {
      lexer->result_symbol = AUTOMATIC_SEMICOLON;

      if (lexer->lookahead != '?') return false;

      advance(lexer);

      return lexer->lookahead == '>';
    }

    return false;
  }
};

}

extern "C" {

void *tree_sitter_php_external_scanner_create() {
  return new Scanner();
}

unsigned tree_sitter_php_external_scanner_serialize(void *payload, char *buffer) {
  Scanner *scanner = static_cast<Scanner *>(payload);
  return scanner->serialize(buffer);
}

void tree_sitter_php_external_scanner_deserialize(void *payload, const char *buffer, unsigned length) {
  Scanner *scanner = static_cast<Scanner *>(payload);
  scanner->deserialize(buffer, length);
}

void tree_sitter_php_external_scanner_destroy(void *payload) {
  Scanner *scanner = static_cast<Scanner *>(payload);
  delete scanner;
}

bool tree_sitter_php_external_scanner_scan(void *payload, TSLexer *lexer,
                                                  const bool *valid_symbols) {

  Scanner *scanner = static_cast<Scanner *>(payload);
  return scanner->scan(lexer, valid_symbols);
}

void tree_sitter_php_external_scanner_reset(void *p) {}

}
