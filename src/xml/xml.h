#ifndef SRC_XML_XML_H_
#define SRC_XML_XML_H_

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <fstream>
#include <iostream>
#include <map>
#include <vector>

namespace xml {

class Xml {
 public:
  typedef std::vector<char> token_t;

  struct level_t;
  typedef std::vector<level_t*> nodes_t;
  typedef std::map<std::string, std::string> opts_t;
  struct level_t {
    std::string tag;
    nodes_t nodes;
    opts_t opts;
  };
  typedef std::vector<level_t*> nodes_vec_t;
  typedef std::map<std::string, nodes_vec_t> map_t;

  enum { DECL_STATE, BODY_STATE };

  static Xml* Create(const std::string& file_name, const Xml* obj = NULL) {
    Xml* xml = new Xml(file_name.c_str(), obj);
    if (xml != NULL) {
      if (xml->Init() == false) {
        delete xml;
        xml = NULL;
      } else {
        const std::size_t pos = file_name.rfind('/');
        const std::string path = (pos != std::string::npos) ? file_name.substr(0, pos + 1) : "";

        xml->PreProcess();
        nodes_t incl_nodes;
        for (auto* node : xml->GetNodes("top.include")) {
          if (node->opts.find("touch") == node->opts.end()) {
            node->opts["touch"] = "";
            incl_nodes.push_back(node);
          }
        }
        for (auto* incl : incl_nodes) {
          const std::string& incl_name = path + incl->opts["file"];
          Xml *ixml = Create(incl_name, xml);
          if (ixml == NULL) {
            delete xml;
            xml = NULL;
            break;
          } else {
            delete(ixml);
          }
        }
        if (xml) {
          xml->Process();
        }
      }
    }

    return xml;
  }

  static void Destroy(Xml *xml) { delete xml; }

  void AddExpr(const std::string& full_tag, const std::string& name, const std::string& expr) {
    const std::size_t pos = full_tag.rfind('.');
    const std::size_t pos1 = (pos == std::string::npos) ? 0 : pos + 1;
    const std::string level_tag = full_tag.substr(pos1);
    level_t* level = new level_t;
    (*map_)[full_tag].push_back(level);
    level->tag = level_tag;
    level->opts["name"] = name;
    level->opts["expr"] = expr;
  }

  void AddConst(const std::string& full_tag, const std::string& name, const uint64_t& val) {
    std::ostringstream oss;
    oss << val;
    AddExpr(full_tag, name, oss.str());
  }

  nodes_t GetNodes(std::string global_tag) { return (*map_)[global_tag]; }

  void Print() const {
    std::cout << "XML file '" << file_name_ << "':" << std::endl;
    for (auto& elem : *map_) {
      for (auto node : elem.second) {
        if (node->opts.size()) {
          std::cout << elem.first << ":" << std::endl;
          for (auto& opt : node->opts) {
            std::cout << "  " << opt.first << " = " << opt.second << std::endl;
          }
        }
      }
    }
  }

 private:
  Xml(const char* file_name, const Xml* obj)
      : file_name_(strdup(file_name)),
        file_line_(0),
        data_size_(0),
        index_(0),
        state_(BODY_STATE),
        comment_(false),
        included_(false),
        level_(NULL),
        map_(NULL)
  {
    if (obj != NULL) {
      map_ = obj->map_;
      level_ = obj->level_;
      included_ = true;
    }
  }

  ~Xml() {
    if (included_ == false) delete map_;
  }

  bool Init() {
    fd_ = open(file_name_, O_RDONLY);
    if (fd_ == -1) {
      perror((std::string("open XML file ") + file_name_).c_str());
      return false;
    }

    if (map_ == NULL) {
      map_ = new map_t;
      if (map_ == NULL) return false;
      AddLevel("top");
    }

    return true;
  }

  void PreProcess() {
    uint32_t ind = 0;
    const uint32_t buf_size = 128;
    char buf[buf_size];
    bool error = false;

    while (1) {
      const uint32_t pos = lseek(fd_, 0, SEEK_CUR);
      uint32_t size = read(fd_, buf, buf_size);
      if (size <= 0) break;
      buf[size - 1] = '\0';

      if (strncmp(buf, "#include \"", 10) == 0) {
        for (ind = 0; (ind < size) && (buf[ind] != '\n'); ++ind);
        if (ind == size) {
          fprintf(stderr, "XML PreProcess failed, line size limit %d\n", (int)buf_size);
          error = true;
          break;
        }
        buf[ind] = '\0';
        size = ind;
        lseek(fd_, pos + ind + 1, SEEK_SET);

        for (ind = 10; (ind < size) && (buf[ind] != '"'); ++ind);
        if (ind == size) {
          error = true;
          break;
        }
        buf[ind] = '\0';

        AddLevel("include");
        AddOption("file", &buf[10]);
        UpLevel();
      }
    }

    if (error) {
      fprintf(stderr, "XML PreProcess failed, line '%s'\n", buf);
      exit(1);
    }

    lseek(fd_, 0, SEEK_SET);
  }

  void Process() {
    token_t remainder;

    while (1) {
      token_t token = (remainder.size()) ? remainder : NextToken();
      remainder.clear();

//      token_t token1 = token;
//      token1.push_back('\0');
//      std::cout << "> " << &token1[0] << std::endl;

      // End of file
      if (token.size() == 0) break;

      switch (state_) {
        case BODY_STATE:
          if (token[0] == '<') {
            bool node_begin = true;
            unsigned ind = 1;
            if (token[1] == '/') {
              node_begin = false;
              ++ind;
            }

            unsigned i = ind;
            while (i < token.size()) {
              if (token[i] == '>') break;
              ++i;
            }
            for (unsigned j = i + 1; j < token.size(); ++j) remainder.push_back(token[j]);

            if (i == token.size()) {
              if (node_begin)
                state_ = DECL_STATE;
              else
                BadFormat(token);
              token.push_back('\0');
            } else
              token[i] = '\0';

            const char* tag = strdup(&token[ind]);
            if (node_begin) {
              AddLevel(tag);
            } else {
              if (strncmp(CurrentLevel().c_str(), tag, strlen(tag))) {
                token.back() = '>';
                BadFormat(token);
              }
              UpLevel();
            }
          } else
            BadFormat(token);
          break;
        case DECL_STATE:
          if (token[0] == '>') {
            state_ = BODY_STATE;
            for (unsigned j = 1; j < token.size(); ++j) remainder.push_back(token[j]);
            continue;
          } else {
            token.push_back('\0');
            unsigned j = 0;
            for (j = 0; j < token.size(); ++j)
              if (token[j] == '=') break;
            if (j == token.size()) BadFormat(token);
            token[j] = '\0';
            const char* key = &token[0];
            const char* value = &token[j + 1];
            AddOption(key, value);
          }
          break;
        default:
          std::cout << "XML parser error: wrong state: " << state_ << std::endl;
          exit(1);
      }
    }
  }

  bool SpaceCheck() const {
    bool cond = ((buffer_[index_] == ' ') || (buffer_[index_] == '	'));
    return cond;
  }

  bool LineEndCheck() {
    bool found = false;
    if (buffer_[index_] == '\n') {
      buffer_[index_] = ' ';
      ++file_line_;
      found = true;
      comment_ = false;
    } else if (comment_ || (buffer_[index_] == '#')) {
      found = true;
      comment_ = true;
    }
    return found;
  }

  token_t NextToken() {
    token_t token;
    bool in_string = false;
    bool special_symb = false;

    while (1) {
      if (data_size_ == 0) {
        data_size_ = read(fd_, buffer_, buf_size_);
        if (data_size_ <= 0) break;
      }

      if (token.empty())
        while ((index_ < data_size_) && (SpaceCheck() || LineEndCheck())) {
          ++index_;
        }
      while ((index_ < data_size_) && (in_string || !(SpaceCheck() || LineEndCheck()))) {
        const char symb = buffer_[index_];
        bool skip_symb = false;

        switch (symb) {
          case '\\':
            if (special_symb) special_symb = false;
            else {
              special_symb = true;
              skip_symb = true;
            }
            break;
          case '"':
            if (special_symb) special_symb = false;
            else {
              in_string = !in_string;
              if (!in_string) {
                buffer_[index_] = ' ';
                --index_;
              }
              skip_symb = true;
            }
            break;
        }

        if (!skip_symb) token.push_back(symb);
        ++index_;
      }

      if (index_ == data_size_) {
        index_ = 0;
        data_size_ = 0;
      } else {
        if (special_symb || in_string) BadFormat(token);
        break;
      }
    }

    return token;
  }

  void BadFormat(token_t token) {
    token.push_back('\0');
    std::cout << "Error: " << file_name_ << ", line " << file_line_ << ", bad XML token '"
              << &token[0] << "'" << std::endl;
    exit(1);
  }

  void AddLevel(const std::string& tag) {
    level_t* level = new level_t;
    level->tag = tag;
    if (level_) {
      level_->nodes.push_back(level);
      stack_.push_back(level_);
    }
    level_ = level;

    std::string global_tag;
    for (level_t* level : stack_) {
      global_tag += level->tag + ".";
    }
    global_tag += tag;
    (*map_)[global_tag].push_back(level_);
  }

  void UpLevel() {
    level_ = stack_.back();
    stack_.pop_back();
  }

  std::string CurrentLevel() const { return level_->tag; }

  void AddOption(const std::string& key, const std::string& value) { level_->opts[key] = value; }

  const char* file_name_;
  unsigned file_line_;
  int fd_;

  static const unsigned buf_size_ = 256;
  char buffer_[buf_size_];

  unsigned data_size_;
  unsigned index_;
  unsigned state_;
  bool comment_;
  std::vector<level_t*> stack_;
  bool included_;
  level_t* level_;
  map_t* map_;
};

}  // namespace xml

#endif  // SRC_XML_XML_H_
