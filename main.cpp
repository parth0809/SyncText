#include <bits/stdc++.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
using namespace std;

static const char *SHM_NAME = "/synctext_registry_shm";
static const char *SHM_MQ_NAME = "/synctext_shm_mq";
static const string FIFO_DIR = "/tmp";
static const int MAX_USERS = 5;
static const int POLL_INTERVAL = 2;
static const size_t USER_LEN = 32;
static const size_t FIFO_LEN = 64;
static const size_t SHM_MQ_CAP = 256;
static const size_t SHM_MQ_MSG_SIZE = 2048;
static const int SYNC_BATCH_SIZE = 1;

static vector<string> INITIAL_CONTENT = {
    "Hello World", "This is a collaborative editor", "Welcome to SyncText",
    "Edit this document and see real-time updates"};

volatile sig_atomic_t terminate_requested = 0;
void handle_signal(int) { terminate_requested = 1; }

string now_time_str() {
  time_t t = time(nullptr);
  struct tm tm;
  localtime_r(&t, &tm);
  char buf[64];
  strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
  return string(buf);
}

string now_time_iso() {
  time_t t = time(nullptr);
  struct tm tm;
  localtime_r(&t, &tm);
  char buf[64];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
  return string(buf);
}

void clear_terminal() {
  printf("\033[2J\033[H");
  fflush(stdout);
}

struct Slot {
  char user[USER_LEN];
  char fifo[FIFO_LEN];
  uint64_t ts;
  uint32_t used;
  uint32_t pad;
};

struct Registry {
  uint32_t magic;
  uint32_t max_users;
  uint32_t version;
  uint32_t reserved;
  Slot slots[MAX_USERS];
};
static Registry *map_registry() {
  int fd = shm_open(SHM_NAME, O_RDWR | O_CREAT, 0600);
  if (fd < 0)
    return nullptr;
  size_t sz = sizeof(Registry);
  if (ftruncate(fd, sz) != 0) {
    close(fd);
    return nullptr;
  }
  void *m = mmap(nullptr, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  close(fd);
  if (m == MAP_FAILED)
    return nullptr;
  Registry *r = reinterpret_cast<Registry *>(m);
  if (r->magic != 0xDEADBEEF) {
    memset(r, 0, sizeof(Registry));
    r->magic = 0xDEADBEEF;
    r->max_users = MAX_USERS;
    r->version = 1;
    for (int i = 0; i < MAX_USERS; ++i) {
      __atomic_store_n(&r->slots[i].used, 0u, __ATOMIC_SEQ_CST);
    }
  }
  return r;
}

vector<tuple<string, string, long long>> read_registry() {
  vector<tuple<string, string, long long>> out;
  Registry *r = map_registry();
  if (!r)
    return out;
  for (int i = 0; i < (int)r->max_users; ++i) {
    uint32_t u = __atomic_load_n(&r->slots[i].used, __ATOMIC_SEQ_CST);
    if (u == 1) {
      string user(r->slots[i].user, strnlen(r->slots[i].user, USER_LEN));
      string fifo(r->slots[i].fifo, strnlen(r->slots[i].fifo, FIFO_LEN));
      long long ts = (long long)r->slots[i].ts;
      out.emplace_back(user, fifo, ts);
    }
  }
  return out;
}

bool register_user(const string &user_id, const string &fifo_path) {
  if (user_id.size() >= USER_LEN)
    return false;
  if (fifo_path.size() >= FIFO_LEN)
    return false;
  Registry *r = map_registry();
  if (!r)
    return false;
    for (int i = 0; i < (int)r->max_users; ++i) {
      uint32_t u = __atomic_load_n(&r->slots[i].used, __ATOMIC_SEQ_CST);
      if (u == 1) {
        if (strncmp(r->slots[i].user, user_id.c_str(), USER_LEN) == 0) {
          return false; 
        }
      }
    }
  int count = 0;
  for (int i = 0; i < (int)r->max_users; ++i) {
    uint32_t u = __atomic_load_n(&r->slots[i].used, __ATOMIC_SEQ_CST);
    if (u == 1)
      ++count;
  }
  if (count >= (int)r->max_users)
    return false;
  for (int i = 0; i < (int)r->max_users; ++i) {
    uint32_t expected = 0;
    if (__atomic_compare_exchange_n(&r->slots[i].used, &expected, 2u, false,
                                    __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
      memset(r->slots[i].user, 0, USER_LEN);
      memset(r->slots[i].fifo, 0, FIFO_LEN);
      strncpy(r->slots[i].user, user_id.c_str(), USER_LEN - 1);
      strncpy(r->slots[i].fifo, fifo_path.c_str(), FIFO_LEN - 1);
      r->slots[i].ts = (uint64_t)time(nullptr);
      __atomic_store_n(&r->slots[i].used, 1u, __ATOMIC_SEQ_CST);
      int final_count = 0;
      for (int j = 0; j < (int)r->max_users; ++j) {
        uint32_t uu = __atomic_load_n(&r->slots[j].used, __ATOMIC_SEQ_CST);
        if (uu == 1)
          ++final_count;
      }
      if (final_count > (int)r->max_users) {
        __atomic_store_n(&r->slots[i].used, 0u, __ATOMIC_SEQ_CST);
        return false;
      }
      return true;
    }
  }
  return false;
}

void unregister_user(const string &user_id) {
  Registry *r = map_registry();
  if (!r)
    return;
  for (int i = 0; i < (int)r->max_users; ++i) {
    uint32_t u = __atomic_load_n(&r->slots[i].used, __ATOMIC_SEQ_CST);
    if (u == 1) {
      if (strncmp(r->slots[i].user, user_id.c_str(), USER_LEN) == 0) {
        __atomic_store_n(&r->slots[i].used, 0u, __ATOMIC_SEQ_CST);
        memset(r->slots[i].user, 0, USER_LEN);
        memset(r->slots[i].fifo, 0, FIFO_LEN);
        r->slots[i].ts = 0;
      }
    }
  }
}

int get_user_index(const string &user_id) {
  Registry *r = map_registry();
  if (!r)
    return -1;
  for (int i = 0; i < (int)r->max_users; ++i) {
    uint32_t u = __atomic_load_n(&r->slots[i].used, __ATOMIC_SEQ_CST);
    if (u == 1) {
      if (strncmp(r->slots[i].user, user_id.c_str(), USER_LEN) == 0)
        return i;
    }
  }
  return -1;
}

struct ShmMsgQueue {
  size_t head;
  size_t tail;
  char msgs[SHM_MQ_CAP][SHM_MQ_MSG_SIZE];
};

struct GlobalMQ {
  ShmMsgQueue q[MAX_USERS];
};

static GlobalMQ *map_global_mq() {
  int fd = shm_open(SHM_MQ_NAME, O_RDWR | O_CREAT, 0600);
  if (fd < 0)
    return nullptr;
  size_t sz = sizeof(GlobalMQ);
  if (ftruncate(fd, sz) != 0) {
    close(fd);
    return nullptr;
  }
  void *m = mmap(nullptr, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  close(fd);
  if (m == MAP_FAILED)
    return nullptr;
  GlobalMQ *g = reinterpret_cast<GlobalMQ *>(m);
  static bool init_done = false;
  if (!init_done) {
    for (int i = 0; i < MAX_USERS; ++i) {
      __atomic_store_n(&g->q[i].head, 0ul, __ATOMIC_SEQ_CST);
      __atomic_store_n(&g->q[i].tail, 0ul, __ATOMIC_SEQ_CST);
    }
    init_done = true;
  }
  return g;
}

bool shm_mq_send(GlobalMQ *g, int user_index, const string &msg) {
  if (!g)
    return false;
  if (user_index < 0 || user_index >= MAX_USERS)
    return false;
  ShmMsgQueue *q = &g->q[user_index];
  size_t tail = __atomic_load_n(&q->tail, __ATOMIC_SEQ_CST);
  size_t head = __atomic_load_n(&q->head, __ATOMIC_SEQ_CST);
  size_t next = (tail + 1) % SHM_MQ_CAP;
  if (next == head)
    return false;
  size_t len = msg.size();
  if (len >= SHM_MQ_MSG_SIZE)
    len = SHM_MQ_MSG_SIZE - 1;
  memcpy(q->msgs[tail], msg.c_str(), len);
  q->msgs[tail][len] = '\0';
  __atomic_store_n(&q->tail, next, __ATOMIC_SEQ_CST);
  return true;
}

bool shm_mq_recv(GlobalMQ *g, int user_index, string &out) {
  if (!g)
    return false;
  if (user_index < 0 || user_index >= MAX_USERS)
    return false;
  ShmMsgQueue *q = &g->q[user_index];
  size_t head = __atomic_load_n(&q->head, __ATOMIC_SEQ_CST);
  size_t tail = __atomic_load_n(&q->tail, __ATOMIC_SEQ_CST);
  if (head == tail)
    return false;
  out.assign(q->msgs[head]);
  __atomic_store_n(&q->head, (head + 1) % SHM_MQ_CAP, __ATOMIC_SEQ_CST);
  return true;
}

struct InboundBuffer {
  static const size_t CAP = 1024;
  struct SlotMsg {
    char data[SHM_MQ_MSG_SIZE];
  } buf[CAP];
  atomic<size_t> head{0};
  atomic<size_t> tail{0};
  bool push(const char *s, size_t len) {
    size_t t = tail.load(memory_order_relaxed);
    size_t h = head.load(memory_order_acquire);
    size_t next = (t + 1) % CAP;
    if (next == h)
      return false;
    size_t idx = t;
    if (len >= SHM_MQ_MSG_SIZE)
      len = SHM_MQ_MSG_SIZE - 1;
    memcpy(buf[idx].data, s, len);
    buf[idx].data[len] = '\0';
    tail.store(next, memory_order_release);
    return true;
  }
  bool pop(string &out) {
    size_t h = head.load(memory_order_relaxed);
    size_t t = tail.load(memory_order_acquire);
    if (h == t)
      return false;
    size_t idx = h;
    out.assign(buf[idx].data);
    head.store((h + 1) % CAP, memory_order_release);
    return true;
  }
} inbound_buffer;

int first_diff_index(const string &a, const string &b) {
  int n = min((int)a.size(), (int)b.size());
  for (int i = 0; i < n; i++)
    if (a[i] != b[i])
      return i;
  return n;
}

int last_diff_index_exclusive(const string &a, const string &b) {
  int na = a.size(), nb = b.size();
  int ia = na - 1, ib = nb - 1;
  while (ia >= 0 && ib >= 0 && a[ia] == b[ib]) {
    ia--;
    ib--;
  }
  return ia + 1;
}

struct Update {
  string op;
  int line_no;
  int col_start;
  int col_end;
  string old_content;
  string new_content;
  string timestamp; 
  string user;
};

vector<Update> compute_line_updates(const vector<string> &oldv,
  const vector<string> &newv,
  const string &user) {
vector<Update> outs;
int oldn = oldv.size(), newn = newv.size();
int maxn = max(oldn, newn);
for (int i = 0; i < maxn; i++) {
bool has_old = (i < oldn);
bool has_new = (i < newn);
if (has_old && has_new) {
const string &o = oldv[i];
const string &n = newv[i];
if (o != n) {
Update u{"replace", i, 0, (int)n.size(), o, n, now_time_iso(), user};
outs.push_back(u);
}
} else if (has_old && !has_new) {
Update u{"delete", i, 0, (int)oldv[i].size(), oldv[i], "", now_time_iso(), user};
outs.push_back(u);
} else if (!has_old && has_new) {
Update u{"insert", i, 0, (int)newv[i].size(), "", newv[i], now_time_iso(), user};
outs.push_back(u);
}
}
return outs;
}


string serialize_update(const Update &u) {
  auto esc = [](const string &s) -> string {
    string r;
    for (char c : s) {
      if (c == '"')
        r += "\\\"";
      else if (c == '\\')
        r += "\\\\";
      else
        r += c;
    }
    return r;
  };
  stringstream ss;
  ss << "{";
  ss << "\"op\":\"" << u.op << "\",";
  ss << "\"line\":" << u.line_no << ",";
  ss << "\"col_start\":" << u.col_start << ",";
  ss << "\"col_end\":" << u.col_end << ",";
  ss << "\"old\":\"" << esc(u.old_content) << "\",";
  ss << "\"new\":\"" << esc(u.new_content) << "\",";
  ss << "\"timestamp\":\"" << u.timestamp << "\",";
  ss << "\"user\":\"" << u.user << "\"";
  ss << "}";
  return ss.str();
}

struct AppState {
  string user_id;
  string doc_path;
  string fifo_path;
  vector<string> current_doc;
  time_t last_mtime;
  Registry *reg;
  vector<tuple<string, string, long long>> active_users;
  bool running = true;
};

vector<tuple<string, string, long long>> load_active_users() {
  return read_registry();
}
void refresh_active_users(AppState &st) {
  st.active_users = load_active_users();
}
void display_state(const AppState &st, const string &last_update_str = "",
                   const vector<Update> *upd = nullptr) {
  clear_terminal();
  printf("Document: %s\n", st.doc_path.c_str());
  printf("Last updated: %s\n", last_update_str.c_str());
  printf("----------------------------------------\n");
  for (size_t i = 0; i < st.current_doc.size(); ++i)
    printf("Line %zu: %s\n", i, st.current_doc[i].c_str());
  printf("----------------------------------------\n");
  vector<string> users;
  for (auto &e : st.active_users)
    users.push_back(get<0>(e));
  printf("Active users: ");
  if (users.empty())
    printf("(none)\n");
  else {
    for (size_t i = 0; i < users.size(); ++i) {
      printf("%s", users[i].c_str());
      if (i + 1 < users.size())
        printf(", ");
    }
    printf("\n");
  }
  printf("Monitoring for changes...\n");
  if (upd && !upd->empty()) {
    printf("\nChange detected(s):\n");
    for (auto &u : *upd) {
      if (u.op == "replace")
        printf("- %s: Line %d, cols %d-%d, \"%s\" -> \"%s\"\n", u.op.c_str(),
               u.line_no, u.col_start, u.col_end, u.old_content.c_str(),
               u.new_content.c_str());
      else if (u.op == "delete")
        printf("- %s: Line %d, cols %d-%d, removed \"%s\"\n", u.op.c_str(),
               u.line_no, u.col_start, u.col_end, u.old_content.c_str());
      else
        printf("- %s: Line %d, inserted \"%s\"\n", u.op.c_str(), u.line_no,
               u.new_content.c_str());
    }
  }
  fflush(stdout);
}

void broadcast_ops(GlobalMQ *g, const string &my_user,
                   const vector<Update> &ops) {
  Registry *r = map_registry();
  if (!r)
    return;
  for (int i = 0; i < (int)r->max_users; ++i) {
    uint32_t used = __atomic_load_n(&r->slots[i].used, __ATOMIC_SEQ_CST);
    if (used != 1)
      continue;
    string target(r->slots[i].user, strnlen(r->slots[i].user, USER_LEN));
    if (target == my_user)
      continue;
    for (auto &u : ops) {
      string msg = serialize_update(u);
      shm_mq_send(g, i, msg);
    }
  }
}

Update parse_update(const string &s) {
  Update u;
  auto g = [&](const string &k) {
    size_t a = s.find("\"" + k + "\"");
    if (a == string::npos)
      return string();
    a = s.find(":", a);
    a = s.find_first_of("\"0123456789-", a);
    size_t b;
    if (s[a] == '\"') {
      a++;
      b = s.find("\"", a);
      return s.substr(a, b - a);
    }
    b = s.find_first_not_of("0123456789-", a + 1);
    return s.substr(a, b - a);
  };
  u.op = g("op");
  u.line_no = stoi(g("line"));
  u.col_start = stoi(g("col_start"));
  u.col_end = stoi(g("col_end"));
  u.old_content = g("old");
  u.new_content = g("new");
  u.timestamp = g("timestamp");
  u.user = g("user");
  return u;
}
static bool overlaps(const Update &a, const Update &b) {
  if (a.line_no != b.line_no) return false;
  int a_start = a.col_start;
  int a_end = a.col_end;
  int b_start = b.col_start;
  int b_end = b.col_end;
  return max(a_start, b_start) < min(a_end, b_end);
}

void apply_update_inmemory(AppState &st, const Update &u) {
  if (u.line_no < 0) return;
  if (u.op == "insert") {
    if (u.line_no >= (int)st.current_doc.size())
      st.current_doc.resize(u.line_no + 1);
    st.current_doc[u.line_no] = u.new_content;
  } else if (u.op == "delete") {
    if (u.line_no < (int)st.current_doc.size())
      st.current_doc.erase(st.current_doc.begin() + u.line_no);
  } else if (u.op == "replace") {
    if (u.line_no < (int)st.current_doc.size())
      st.current_doc[u.line_no] = u.new_content;
    else {
      st.current_doc.resize(u.line_no + 1);
      st.current_doc[u.line_no] = u.new_content;
    }
  }
}

void listener_thread(AppState &st, GlobalMQ *g, int my_index) {
  string msg;
  while (!terminate_requested && st.running) {
    while (shm_mq_recv(g, my_index, msg)) {
      inbound_buffer.push(msg.c_str(), msg.size() + 1);
    }
    this_thread::sleep_for(chrono::milliseconds(50));
  }
}

vector<string> read_file_lines(const string &path) {
  vector<string> out;
  FILE *f = fopen(path.c_str(), "r");
  if (!f)
    return out;
  char *line = nullptr;
  size_t cap = 0;
  while (getline(&line, &cap, f) > 0) {
    string s(line);
    if (!s.empty() && s.back() == '\n')
      s.pop_back();
    out.push_back(s);
  }
  if (line)
    free(line);
  fclose(f);
  return out;
}

bool ensure_fifo_exists(const string &p) {
  struct stat st;
  if (stat(p.c_str(), &st) == 0) {
    if ((st.st_mode & S_IFIFO) == S_IFIFO)
      return true;
    unlink(p.c_str());
  }
  if (mkfifo(p.c_str(), 0600) != 0) {
    if (errno == EEXIST)
      return true;
    return false;
  }
  return true;
}

bool write_fifo_nonblock(const string &p, const string &data) {
  int fd = open(p.c_str(), O_WRONLY | O_NONBLOCK);
  if (fd < 0)
    return false;
  write(fd, data.c_str(), data.size());
  close(fd);
  return true;
}

vector<Update> merge_updates_crdt(vector<Update> &local_ops, vector<Update> &recv_ops) {
  vector<Update> all;
  all.insert(all.end(), local_ops.begin(), local_ops.end());
  all.insert(all.end(), recv_ops.begin(), recv_ops.end());
  unordered_map<int, vector<int>> idx_by_line;
  for (int i = 0; i < (int)all.size(); ++i) idx_by_line[all[i].line_no].push_back(i);
  vector<char> alive(all.size(), 1);

  for (auto &p : idx_by_line) {
    auto &indices = p.second;
    for (size_t i = 0; i < indices.size(); ++i) {
      for (size_t j = i + 1; j < indices.size(); ++j) {
        int ii = indices[i];
        int jj = indices[j];
        if (!alive[ii] || !alive[jj]) continue;
        if (overlaps(all[ii], all[jj])) {
          if (all[ii].timestamp > all[jj].timestamp) {
            alive[jj] = 0;
          } else if (all[ii].timestamp < all[jj].timestamp) {
            alive[ii] = 0;
          } else {
            if (all[ii].user <= all[jj].user) alive[jj] = 0;
            else alive[ii] = 0;
          }
        }
      }
    }
  }
  vector<Update> winners;
  for (int i = 0; i < (int)all.size(); ++i) {
    if (alive[i]) winners.push_back(all[i]);
  }
  sort(winners.begin(), winners.end(), [](const Update &a, const Update &b) {
    if (a.line_no != b.line_no) return a.line_no > b.line_no;
    return a.col_start > b.col_start;
  });
  return winners;
}

void watcher_thread(AppState &st, GlobalMQ *g, int my_index) {
  vector<Update> local_ops;   
  vector<Update> recv_ops;   
  while (!terminate_requested && st.running) {
    struct stat stbuf;
    if (stat(st.doc_path.c_str(), &stbuf) != 0) {
      this_thread::sleep_for(chrono::seconds(POLL_INTERVAL));
      continue;
    }
    time_t mtime = stbuf.st_mtime;
   if (mtime != st.last_mtime) {
      auto newdoc = read_file_lines(st.doc_path);
      auto updates = compute_line_updates(st.current_doc, newdoc, st.user_id);
       for (auto &u : updates) local_ops.push_back(u);
      st.current_doc = newdoc;
      st.last_mtime = mtime;
      string ts = now_time_str();
      refresh_active_users(st);
      display_state(st, ts, &updates);
    }
    string incoming;
    bool new_recv = false;
    
    while (inbound_buffer.pop(incoming)) {
      Update u = parse_update(incoming);
      if (u.user == st.user_id) continue;
      recv_ops.push_back(u);
      printf("\n[Buffered recv] %s\n", incoming.c_str());
      apply_update_inmemory(st, u);
      new_recv = true;
    }
    
    if (new_recv) {
      FILE *f = fopen(st.doc_path.c_str(), "w");
      if (f) {
        for (auto &l : st.current_doc)
          fprintf(f, "%s\n", l.c_str());
        fclose(f);
      }
      struct stat new_stbuf;
      if (stat(st.doc_path.c_str(), &new_stbuf) == 0)
        st.last_mtime = new_stbuf.st_mtime;
    }
    if ((int)(local_ops.size() + recv_ops.size()) >= SYNC_BATCH_SIZE) {
      vector<Update> winners = merge_updates_crdt(local_ops, recv_ops);
      for (auto &u : winners) {
        apply_update_inmemory(st, u);
      }
    
      FILE *f = fopen(st.doc_path.c_str(), "w");
      if (f) {
        for (auto &l : st.current_doc)
          fprintf(f, "%s\n", l.c_str());
        fclose(f);
      }
    
      struct stat new_stbuf;
      if (stat(st.doc_path.c_str(), &new_stbuf) == 0)
        st.last_mtime = new_stbuf.st_mtime;
    

      if (!winners.empty()) {
        broadcast_ops(g, st.user_id, winners);
      }
    
      local_ops.clear();
      recv_ops.clear();
    
      refresh_active_users(st);
      display_state(st, now_time_str(), &winners);
    }
    

    for (int i = 0; i < POLL_INTERVAL && !terminate_requested; i++)
      this_thread::sleep_for(chrono::seconds(1));
  }
}

int main(int argc, char **argv) {
  ios::sync_with_stdio(false);
  cin.tie(nullptr);
  if (argc != 2) {
    fprintf(stderr, "Usage: %s <user_id>\n", argv[0]);
    return 1;
  }
  string user_id = argv[1];
  if (user_id.empty())
    return 1;
  signal(SIGINT, handle_signal);
  signal(SIGTERM, handle_signal);
  AppState st;
  st.user_id = user_id;
  st.doc_path = user_id + "_doc.txt";
  st.fifo_path = FIFO_DIR + string("/editor_fifo_") + user_id;
  st.last_mtime = 0;
  if (!ensure_fifo_exists(st.fifo_path)) {
    fprintf(stderr, "Failed to create fifo %s\n", st.fifo_path.c_str());
    return 1;
  }
  if (!register_user(user_id, st.fifo_path)) {
    fprintf(stderr, "Failed to register user: maybe too many users (max %d)\n",
            MAX_USERS);
    unlink(st.fifo_path.c_str());
    return 1;
  }
  Registry *global_reg = map_registry();
  st.reg = global_reg;
  refresh_active_users(st);
  GlobalMQ *g = map_global_mq();
  if (!g) {
    unregister_user(user_id);
    unlink(st.fifo_path.c_str());
    fprintf(stderr, "Failed to map global shared message queues\n");
    return 1;
  }
  display_state(st, "", nullptr);

  int my_index = get_user_index(user_id);
  if (my_index < 0) {
    unregister_user(user_id);
    unlink(st.fifo_path.c_str());
    fprintf(stderr, "Failed to find user index after registration\n");
    return 1;
  }
  if (access(st.doc_path.c_str(), F_OK) != 0) {
    FILE *f = fopen(st.doc_path.c_str(), "w");
    if (f) {
      for (auto &L : INITIAL_CONTENT)
        fprintf(f, "%s\n", L.c_str());
      fclose(f);
    }
  }
  st.current_doc = read_file_lines(st.doc_path);
  struct stat stbuf;
  if (stat(st.doc_path.c_str(), &stbuf) == 0)
    st.last_mtime = stbuf.st_mtime;
  thread listener(listener_thread, ref(st), g, my_index);
  thread watcher(watcher_thread, ref(st), g, my_index);
  while (!terminate_requested) {
    refresh_active_users(st);
    display_state(st, "", nullptr);
    for (int i = 0; i < 3 && !terminate_requested; ++i)
      this_thread::sleep_for(chrono::seconds(1));
  }

  st.running = false;
  watcher.join();
  listener.join();
  unregister_user(user_id);
  unlink(st.fifo_path.c_str());
  clear_terminal();
  printf("User %s unregistered. Exiting.\n", user_id.c_str());
  return 0;
}
